#include "CyclicDataFlow.hpp"
#include "PortDataAccess.hpp"
#include "DataSource.hpp"
#include "DataSources.hpp"
#include "../TaskContext.hpp"
#include "../base/InputPortInterface.hpp"
#include "../base/OutputPortInterface.hpp"
#include "../base/ActionInterface.hpp"
#include "../types/TypeInfo.hpp"
#include "../Logger.hpp"
#include <algorithm>
#include <set>
#include <atomic>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace RTT { namespace internal {
namespace {
typedef base::DataSourceBase::shared_ptr Sample;
typedef boost::shared_ptr<base::ActionInterface> Assignment;
struct Segment { std::string name; bool index; };
typedef std::vector<Segment> Path;

std::string pathText(const Path& path) {
    std::string text;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i].index) text += "[" + path[i].name + "]";
        else {
            if (!text.empty()) text += ".";
            text += path[i].name;
        }
    }
    return text;
}

bool parse(const std::string& text, Path& result) {
    size_t pos = 0;
    bool member = true;
    while (pos < text.size()) {
        if (text[pos] == '[') {
            ++pos;
            size_t begin = pos;
            while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
            if (begin == pos || pos == text.size() || text[pos] != ']') return false;
            std::string number = text.substr(begin, pos - begin);
            size_t first = number.find_first_not_of('0');
            result.push_back(Segment{first == std::string::npos ? "0" : number.substr(first), true});
            ++pos; member = false;
        } else {
            if (!member || !(std::isalpha(static_cast<unsigned char>(text[pos])) || text[pos] == '_')) return false;
            size_t begin = pos++;
            while (pos < text.size() && (std::isalnum(static_cast<unsigned char>(text[pos])) || text[pos] == '_')) ++pos;
            result.push_back(Segment{text.substr(begin, pos-begin), false});
            member = false;
        }
        if (pos < text.size() && text[pos] == '.') {
            ++pos; member = true;
            if (pos == text.size() || text[pos] == '[') return false;
        } else if (pos < text.size() && text[pos] != '[') return false;
    }
    return true;
}

bool sequence(Sample sample, int& size) {
    std::vector<std::string> names = sample->getMemberNames();
    if (names.size() != 2 || names[0] != "size" || names[1] != "capacity") return false;
    Sample ds = sample->getMember("size");
    DataSource<int>* count = dynamic_cast<DataSource<int>*>(ds.get());
    if (!count) return false;
    size = count->get();
    return size >= 0;
}

bool fixedArray(Sample sample) {
    int count = 0;
    return sample && sequence(sample, count) &&
        dynamic_cast<ConstantDataSource<int>*>(sample->getMember("size").get());
}
bool owningImage(Sample image) {
    if (!image || fixedArray(image)) {
        Logger::log().logf(Logger::Error, "CyclicDataFlow", "Cyclic root ports require owning values; wrap fixed C arrays in an owning struct");
        return false;
    }
    return true;
}

Sample select(Sample sample, const Path& path) {
    for (size_t i = 0; i < path.size(); ++i) {
        if (!sample) return Sample();
        if (path[i].index) {
            int count = 0;
            if (!sequence(sample, count) || !fixedArray(sample)) return Sample();
            std::istringstream stream(path[i].name);
            unsigned long long index;
            if (!(stream >> index) || index >= static_cast<unsigned long long>(count)) return Sample();
        } else {
            std::vector<std::string> names = sample->getMemberNames();
            if (std::find(names.begin(), names.end(), path[i].name) == names.end()) return Sample();
        }
        sample = sample->getMember(path[i].name);
    }
    return sample;
}

bool overlaps(const Path& a, const Path& b) {
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        if (a[i].name != b[i].name || a[i].index != b[i].index) return false;
    return true;
}

bool bind(Sample source, Sample destination, std::vector<Assignment>& assignments, unsigned depth = 0) {
    if (!source || !destination || !destination->isAssignable() ||
        source->getTypeInfo() != destination->getTypeInfo() || depth > 64) return false;
    int fromSize = 0, toSize = 0;
    bool fromArray = sequence(source, fromSize), toArray = sequence(destination, toSize);
    if (fromArray != toArray) return false;
    if (fromArray && !fixedArray(source)) {
        Assignment action(destination->updateAction(source.get()));
        if (!action) return false;
        assignments.push_back(action);
        return true;
    }
    if (fromArray) {
        if (fromSize != toSize) return false;
        for (int i = 0; i < fromSize; ++i) {
            std::ostringstream index; index << i;
            if (!bind(source->getMember(index.str()), destination->getMember(index.str()), assignments, depth + 1)) return false;
        }
        return true;
    }
    // Descend into reflected structs so nested C array extents are validated and
    // carray wrappers are copied elementwise rather than aliasing their storage.
    std::vector<std::string> members = source->getMemberNames();
    if (!members.empty()) {
        for (size_t i = 0; i < members.size(); ++i)
            if (!bind(source->getMember(members[i]), destination->getMember(members[i]), assignments, depth + 1)) return false;
        return true;
    }
    Assignment action(destination->updateAction(source.get()));
    if (!action) return false;
    assignments.push_back(action);
    return true;
}

bool compatible(Sample source, Sample destination, unsigned depth = 0) {
    if (!source || !destination || source->getTypeInfo() != destination->getTypeInfo() || depth > 64) return false;
    int sourceCount = 0, destinationCount = 0;
    bool sourceSequence = sequence(source, sourceCount), destinationSequence = sequence(destination, destinationCount);
    if (sourceSequence != destinationSequence) return false;
    if (sourceSequence) {
        // Fixed C arrays expose a constant extent; dynamic sequence lengths are
        // values, so a whole sequence remains an ordinary typed channel value.
        Sample count = source->getMember("size");
        if (!dynamic_cast<ConstantDataSource<int>*>(count.get())) return true;
        if (sourceCount != destinationCount) return false;
        for (int i = 0; i < sourceCount; ++i) {
            std::ostringstream index; index << i;
            if (!compatible(source->getMember(index.str()), destination->getMember(index.str()), depth + 1)) return false;
        }
        return true;
    }
    std::vector<std::string> names = source->getMemberNames();
    for (size_t i = 0; i < names.size(); ++i)
        if (!compatible(source->getMember(names[i]), destination->getMember(names[i]), depth + 1)) return false;
    return true;
}

bool collect(Service::shared_ptr service, std::vector<base::PortInterface*>& ports, std::set<Service*>& visited) {
    if (!service || !visited.insert(service.get()).second) return false;
    DataFlowInterface::Ports local = service->getPorts();
    ports.insert(ports.end(), local.begin(), local.end());
    std::vector<std::string> children = service->getProviderNames();
    for (size_t i = 0; i < children.size(); ++i)
        if (!collect(service->getService(children[i]), ports, visited)) return false;
    return true;
}
}

struct CyclicDataFlow::Impl {
    struct Mapping {
        Path sourcePath;
        Path destinationPath;
        std::vector<Assignment> assignments;
    };
    struct Subscription {
        base::OutputPortInterface* source;
        base::InputPortInterface* destination;
        boost::shared_ptr<base::InputPortInterface> input;
        std::vector<Mapping> mappings;
        bool received = false;
    };
    struct Assembly {
        base::InputPortInterface* destination;
        std::vector<Subscription*> sources;
    };
    TaskContext& task;
    CyclicDataFlow& self;
    std::atomic<bool> prepared{false};
    std::vector<boost::shared_ptr<Subscription> > subscriptions;
    std::vector<base::InputPortInterface*> inputs;
    std::vector<base::OutputPortInterface*> outputs;
    std::vector<Assembly> assemblies;
    std::vector<base::PortInterface*> watched;
    Impl(TaskContext& owner, CyclicDataFlow& runtime) : task(owner), self(runtime) {}
    void watch(base::PortInterface& port) {
        if (std::find(watched.begin(), watched.end(), &port) == watched.end()) {
            watched.push_back(&port);
            port.addCyclicDependency(&self);
        }
    }
    void unwatch() {
        for (size_t i = 0; i < watched.size(); ++i) watched[i]->removeCyclicDependency(&self);
        watched.clear();
    }
};

bool CyclicDataFlow::validateWhole(base::OutputPortInterface& source, base::InputPortInterface& destination) {
    try {
        Sample from = PortDataAccess::image(source), to = PortDataAccess::image(destination);
        return owningImage(from) && owningImage(to) && compatible(from, to);
    }
    catch (const std::exception&) { return false; }
}

CyclicDataFlow::CyclicDataFlow(TaskContext& owner) : impl(new Impl(owner, *this)) {}
CyclicDataFlow::~CyclicDataFlow() { impl->unwatch(); }
TaskContext& CyclicDataFlow::owner() const { return impl->task; }
bool CyclicDataFlow::valid() const { return impl->prepared; }
void CyclicDataFlow::invalidate() { impl->prepared = false; }

bool CyclicDataFlow::connect(base::OutputPortInterface& source, const std::string& sourcePath,
                             base::InputPortInterface& destination, const std::string& destinationPath) {
    if (!source.connectionChangeAllowed() || !destination.connectionChangeAllowed()) return false;
    if (!owningImage(PortDataAccess::image(source)) || !owningImage(PortDataAccess::image(destination))) return false;
    Path from, to;
    if (!parse(sourcePath, from) || !parse(destinationPath, to)) return false;
    if (destination.getManager()->connected()) return false;
    boost::shared_ptr<Impl::Subscription> subscription;
    for (size_t i = 0; i < impl->subscriptions.size(); ++i) {
        Impl::Subscription& existing = *impl->subscriptions[i];
        if (existing.destination != &destination) continue;
        for (size_t j = 0; j < existing.mappings.size(); ++j)
            if (overlaps(existing.mappings[j].destinationPath, to)) return false;
        if (existing.source == &source) subscription = impl->subscriptions[i];
    }
    bool isNew = !subscription;
    if (isNew) {
        subscription.reset(new Impl::Subscription());
        subscription->source = &source; subscription->destination = &destination;
        subscription->input.reset(dynamic_cast<base::InputPortInterface*>(source.antiClone()));
        if (!subscription->input) return false;
        // Source-typed storage is sized once before any member handles are bound.
        if (!PortDataAccess::image(*subscription->input)->update(PortDataAccess::image(source).get())) return false;
    }
    Impl::Mapping mapping;
    mapping.sourcePath = from;
    mapping.destinationPath = to;
    try {
        if (!bind(select(PortDataAccess::image(*subscription->input), from),
                  select(PortDataAccess::image(destination), to), mapping.assignments)) return false;
    } catch (const std::exception&) { return false; }
    if (isNew) {
        ConnPolicy policy = ConnPolicy::data(); policy.init = false;
        if (!source.createConnection(*subscription->input, policy)) return false;
        impl->subscriptions.push_back(subscription);
    }
    subscription->mappings.push_back(mapping);
    impl->watch(source); impl->watch(destination);
    invalidate();
    return true;
}

bool CyclicDataFlow::finalize() {
    if (owner().base::TaskCore::isRunning()) return false;
    invalidate();
    impl->inputs.clear(); impl->outputs.clear(); impl->assemblies.clear();
    impl->unwatch();
    for (size_t i = 0; i < impl->subscriptions.size(); ++i) {
        Impl::Subscription& subscription = *impl->subscriptions[i];
        impl->watch(*subscription.source); impl->watch(*subscription.destination);
        try {
            TaskContext* sourceOwner = subscription.source->getInterface() ? subscription.source->getInterface()->getOwner() : 0;
            if ((!sourceOwner || !sourceOwner->base::TaskCore::isRunning()) &&
                !PortDataAccess::image(*subscription.input)->update(PortDataAccess::image(*subscription.source).get())) return false;
            for (size_t m = 0; m < subscription.mappings.size(); ++m) {
                Impl::Mapping& mapping = subscription.mappings[m];
                std::vector<Assignment> assignments;
                if (!bind(select(PortDataAccess::image(*subscription.input), mapping.sourcePath),
                          select(PortDataAccess::image(*subscription.destination), mapping.destinationPath), assignments)) return false;
                mapping.assignments.swap(assignments);
            }
        } catch (const std::exception&) { return false; }
    }
    std::vector<base::PortInterface*> ports;
    std::set<Service*> visited;
    if (!collect(owner().provides(), ports, visited)) return false;
    for (size_t i = 0; i < ports.size(); ++i) {
        impl->watch(*ports[i]);
        if (base::InputPortInterface* input = dynamic_cast<base::InputPortInterface*>(ports[i])) {
            if (!owningImage(PortDataAccess::image(*input))) return false;
            Impl::Assembly assembly; assembly.destination = input;
            for (size_t j = 0; j < impl->subscriptions.size(); ++j)
                if (impl->subscriptions[j]->destination == input) assembly.sources.push_back(impl->subscriptions[j].get());
            internal::ConnectionManager::Connections connections = input->getManager()->getConnections();
            if (connections.size() > 1 || (!assembly.sources.empty() && !connections.empty())) return false;
            for (internal::ConnectionManager::Connections::const_iterator c = connections.begin(); c != connections.end(); ++c)
                if (boost::get<2>(*c).type != ConnPolicy::DATA) return false;
            if (assembly.sources.empty()) impl->inputs.push_back(input);
            else impl->assemblies.push_back(assembly);
        } else if (base::OutputPortInterface* output = dynamic_cast<base::OutputPortInterface*>(ports[i])) {
            if (!owningImage(PortDataAccess::image(*output))) return false;
            impl->outputs.push_back(output);
        }
    }
    impl->prepared = true;
    return true;
}

bool CyclicDataFlow::refresh() {
    if (!valid()) return false;
    for (size_t i = 0; i < impl->inputs.size(); ++i) PortDataAccess::refresh(*impl->inputs[i]);
    for (size_t i = 0; i < impl->assemblies.size(); ++i) {
        Impl::Assembly& assembly = impl->assemblies[i];
        FlowStatus status = NoData;
        for (size_t j = 0; j < assembly.sources.size(); ++j) {
            Impl::Subscription& source = *assembly.sources[j];
            FlowStatus current = PortDataAccess::refresh(*source.input);
            if (current == NewData) {
                source.received = true; status = NewData;
                for (size_t m = 0; m < source.mappings.size(); ++m)
                    for (size_t a = 0; a < source.mappings[m].assignments.size(); ++a) {
                        base::ActionInterface& action = *source.mappings[m].assignments[a];
                        action.readArguments();
                        if (!action.execute()) { invalidate(); return false; }
                    }
            } else if (source.received && status == NoData) status = OldData;
        }
        PortDataAccess::status(*assembly.destination, status);
    }
    return true;
}

bool CyclicDataFlow::commit() {
    if (!valid()) return false;
    for (size_t i = 0; i < impl->outputs.size(); ++i) PortDataAccess::commit(*impl->outputs[i]);
    return true;
}

bool CyclicDataFlow::connectionChangeAllowed(const base::PortInterface& port) const {
    for (size_t i = 0; i < impl->subscriptions.size(); ++i) {
        const Impl::Subscription& s = *impl->subscriptions[i];
        if (s.source != &port && s.destination != &port) continue;
        TaskContext* sourceOwner = s.source->getInterface() ? s.source->getInterface()->getOwner() : 0;
        TaskContext* destinationOwner = s.destination->getInterface() ? s.destination->getInterface()->getOwner() : 0;
        if ((sourceOwner && (sourceOwner->base::TaskCore::isRunning() || sourceOwner->base::TaskCore::getTargetState() >= base::TaskCore::Running)) ||
            (destinationOwner && (destinationOwner->base::TaskCore::isRunning() || destinationOwner->base::TaskCore::getTargetState() >= base::TaskCore::Running))) return false;
    }
    return true;
}

bool CyclicDataFlow::contains(const base::PortInterface& port, const base::PortInterface* other) const {
    for (size_t i = 0; i < impl->subscriptions.size(); ++i) {
        const Impl::Subscription& s = *impl->subscriptions[i];
        if ((s.source == &port && (!other || s.destination == other)) ||
            (s.destination == &port && (!other || s.source == other))) return true;
    }
    return false;
}

base::InputPortInterface::SourceConnections CyclicDataFlow::sources(const base::InputPortInterface& port) const {
    base::InputPortInterface::SourceConnections result;
    for (size_t i = 0; i < impl->subscriptions.size(); ++i) {
        const Impl::Subscription& subscription = *impl->subscriptions[i];
        if (subscription.destination != &port) continue;
        for (size_t m = 0; m < subscription.mappings.size(); ++m) {
            const Impl::Mapping& mapping = subscription.mappings[m];
            result.push_back(base::InputPortInterface::SourceConnection{
                subscription.source->getFullName(), pathText(mapping.sourcePath), pathText(mapping.destinationPath)});
        }
    }
    return result;
}

bool CyclicDataFlow::disconnect(base::PortInterface& port, base::PortInterface* other) {
    bool removed = false;
    for (size_t i = 0; i < impl->subscriptions.size();) {
        Impl::Subscription& s = *impl->subscriptions[i];
        if ((s.source == &port && (!other || s.destination == other)) ||
            (s.destination == &port && (!other || s.source == other))) {
            invalidate(); impl->assemblies.clear();
            impl->subscriptions.erase(impl->subscriptions.begin() + i);
            removed = true;
        } else ++i;
    }
    return removed;
}

void CyclicDataFlow::forget(base::PortInterface& port) {
    invalidate();
    impl->inputs.clear(); impl->outputs.clear(); impl->assemblies.clear();
    disconnect(port);
    impl->watched.erase(std::remove(impl->watched.begin(), impl->watched.end(), &port), impl->watched.end());
    port.removeCyclicDependency(this);
}
}}
