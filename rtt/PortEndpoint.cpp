#include "PortEndpoint.hpp"
#include "Service.hpp"
#include "TaskContext.hpp"
#include "base/InputPortInterface.hpp"
#include "base/OutputPortInterface.hpp"
#include "internal/PortDataAccess.hpp"
#include "internal/CyclicDataFlow.hpp"
#include "internal/ObservationPath.hpp"
#include "types/TypeInfo.hpp"
#include <cctype>
#include <utility>
namespace RTT {
namespace {
bool fail(std::string* error, const std::string& message) { if (error) *error = message; return false; }
void success(std::string* error) { if (error) error->clear(); }
base::DataSourceBase::shared_ptr selected(const PortEndpoint& endpoint) {
    if (!endpoint.port) return {};
    auto root = endpoint.port->getObservationDataSource();
    if (!root) return {};
    root->evaluate();
    return internal::CyclicDataFlow::selectMember(root, endpoint.member);
}
}
const types::TypeInfo* PortEndpoint::getTypeInfo() const {
    auto value = selected(*this); return value ? value->getTypeInfo() : nullptr;
}
bool resolvePortEndpoint(Service& service, const std::string& path, PortEndpoint& result, std::string* error) {
    result = {};
    Service* current = &service;
    std::size_t pos = 0;
    if (path.empty() || path.find("::") != std::string::npos) return fail(error, "expected a dot/index port endpoint");
    while (pos < path.size()) {
        if (!(std::isalpha(static_cast<unsigned char>(path[pos])) || path[pos] == '_')) return fail(error, "invalid port endpoint identifier");
        const auto begin = pos++;
        while (pos < path.size() && (std::isalnum(static_cast<unsigned char>(path[pos])) || path[pos] == '_')) ++pos;
        const auto name = path.substr(begin, pos - begin);
        if (auto* port = current->getPort(name)) {
            std::string member;
            if (pos < path.size()) {
                if (path[pos] == '.') ++pos;
                else if (path[pos] != '[') return fail(error, "invalid port member separator");
                if (pos == path.size()) return fail(error, "missing port member");
                member = path.substr(pos);
            }
            auto root = port->getObservationDataSource();
            if (!root) return fail(error, "port has no observation source");
            root->evaluate();
            std::string canonical;
            if (!internal::CyclicDataFlow::selectMember(root, member, &canonical)) return fail(error, "invalid port member or fixed array index");
            result = {port, canonical}; success(error); return true;
        }
        if (pos == path.size() || path[pos++] != '.') return fail(error, "port does not exist");
        auto child = current->getService(name);
        if (!child) return fail(error, "service does not exist");
        current = child.get();
    }
    return fail(error, "missing port name");
}
struct PortObservation::Impl {
    internal::ObservationPath::shared_ptr path;
    base::DataSourceBase::shared_ptr value;
};
PortObservation::PortObservation(std::unique_ptr<Impl> value) : impl(std::move(value)) {}
PortObservation::~PortObservation() = default;
std::shared_ptr<PortObservation> PortObservation::create(const PortEndpoint& endpoint, std::string* error) {
    if (!endpoint.port) { fail(error, "port does not exist"); return {}; }
    auto root = endpoint.port->getObservationDataSource();
    if (!root) { fail(error, "port has no observation source"); return {}; }
    std::unique_ptr<Impl> state(new Impl);
    root->evaluate();
    std::string canonical;
    if (!internal::CyclicDataFlow::selectMember(root, endpoint.member, &canonical)) {
        fail(error, "invalid port member or fixed array index"); return {};
    }
    state->path.reset(new internal::ObservationPath(root));
    state->path = state->path->select(canonical);
    state->value = state->path->expression();
    if (!state->value) { fail(error, "port type does not support read-only expressions"); return {}; }
    success(error);
    return std::shared_ptr<PortObservation>(new PortObservation(std::move(state)));
}
base::DataSourceBase::shared_ptr PortObservation::dataSource() const { return impl->value; }
base::DataSourceBase::shared_ptr PortObservation::snapshot() const {
    // A live expression's factory freezes its selected value in owned typed storage.
    const auto* expression = dynamic_cast<const internal::ObservationExpression*>(impl->value.get());
    return expression ? expression->frozen() : impl->path->expression(false);
}
bool PortObservation::available() const { return impl->path->available(); }
struct PortInputSource::Impl {
    std::unique_ptr<base::OutputPortInterface> source;
    base::DataSourceBase::shared_ptr selection;
};
PortInputSource::PortInputSource(std::unique_ptr<Impl> value) : impl(std::move(value)) {}
PortInputSource::~PortInputSource() = default;
std::shared_ptr<PortInputSource> PortInputSource::create(const PortEndpoint& endpoint, std::string* error, const std::string& sourceName) {
    auto* input = dynamic_cast<base::InputPortInterface*>(endpoint.port);
    if (!input) { fail(error, "external writes require an input port"); return {}; }
    if (!input->connectionChangeAllowed()) { fail(error, "input source configuration requires stopped components"); return {}; }
    std::unique_ptr<Impl> state(new Impl);
    std::unique_ptr<base::PortInterface> antiPort(input->antiClone());
    auto* output = dynamic_cast<base::OutputPortInterface*>(antiPort.get());
    if (output) state->source.reset(static_cast<base::OutputPortInterface*>(antiPort.release()));
    if (!state->source) { fail(error, "cannot create external input source"); return {}; }
    if (!state->source->setName(sourceName)) { fail(error, "invalid external source name"); return {}; }
    auto image = internal::PortDataAccess::image(*state->source);
    auto initial = PortObservation::create({input, ""}, error);
    if (!initial || !internal::CyclicDataFlow::copySample(initial->snapshot(), image)) {
        fail(error, "cannot initialize external source image"); return {};
    }
    state->selection = internal::CyclicDataFlow::selectMember(image, endpoint.member);
    if (!state->selection || !state->selection->isAssignable()) { fail(error, "input selection is not writable"); return {}; }
    const bool connected = endpoint.member.empty() ? state->source->createConnection(*input)
        : RTT::connectMembers(*state->source, endpoint.member, *input, endpoint.member);
    if (!connected) { fail(error, "input region already has a writer or cannot be connected"); return {}; }
    success(error);
    return std::shared_ptr<PortInputSource>(new PortInputSource(std::move(state)));
}
const types::TypeInfo* PortInputSource::getTypeInfo() const { return impl->selection->getTypeInfo(); }
bool PortInputSource::connected() const { return impl->source->connected(); }
bool PortInputSource::stage(base::DataSourceBase::shared_ptr value, std::string* error) {
    if (!connected()) return fail(error, "input source is disconnected");
    if (!value || !internal::CyclicDataFlow::copySample(value, impl->selection)) return fail(error, "input sample type or shape does not match");
    if (internal::PortDataAccess::commit(*impl->source) != WriteSuccess) return fail(error, "input source publication failed");
    success(error); return true;
}
bool PortInputSource::disconnect(std::string* error) {
    if (!impl->source->connectionChangeAllowed()) return fail(error, "input source configuration requires stopped components");
    impl->source->disconnect(); success(error); return true;
}
}
