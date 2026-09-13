#include "ObservationPath.hpp"
#include "DataSource.hpp"
#include "DataSources.hpp"
#include "../types/TypeInfo.hpp"
#include <algorithm>
namespace RTT { namespace internal {
ObservationPath::ObservationPath(base::DataSourceBase::shared_ptr source) : root(std::move(source)) {}
base::DataSourceBase::shared_ptr ObservationPath::resolve(bool* available) const {
    if (available) *available = false;
    if (!root) return {};
    const bool evaluated = root->evaluate();
    const auto* status = dynamic_cast<const ObservationStatus*>(root.get());
    present = status ? status->available() : evaluated;
    auto selected = root;
    for (const auto& selector : selectors) {
        if (!selected) return {};
        if (selector.index) {
            auto size = selected->getMember("size");
            auto count = dynamic_cast<DataSource<int>*>(size.get());
            auto converted = DataSource<unsigned int>::GetTypeInfo()->convert(selector.index);
            auto index = dynamic_cast<DataSource<unsigned int>*>(converted.get());
            if (!count || !index || index->get() >= static_cast<unsigned int>(count->get())) return {};
            selected = selected->getMember(selector.index, {});
        } else {
            const auto names = selected->getMemberNames();
            if (std::find(names.begin(), names.end(), selector.name) == names.end()) return {};
            selected = selected->getMember(selector.name);
        }
    }
    if (available) *available = present;
    return selected;
}
bool ObservationPath::available() const { return resolve() && present; }
ObservationPath::shared_ptr ObservationPath::copy(std::map<const base::DataSourceBase*, base::DataSourceBase*>& replacements) const {
    shared_ptr result(new ObservationPath(root->copy(replacements)));
    result->selectors = selectors;
    for (auto& selector : result->selectors) if (selector.index) selector.index = selector.index->copy(replacements);
    return result;
}
ObservationPath::shared_ptr ObservationPath::select(const std::string& canonical) const {
    shared_ptr next(new ObservationPath(*this));
    std::size_t pos = 0;
    while (pos < canonical.size()) {
        if (canonical[pos] == '.') { ++pos; continue; }
        if (canonical[pos] == '[') {
            auto end = canonical.find(']', ++pos);
            if (end == std::string::npos) return {};
            unsigned long index = std::stoul(canonical.substr(pos, end - pos));
            next->selectors.push_back(Selector{{}, new ConstantDataSource<unsigned int>(index)});
            pos = end + 1;
        } else {
            auto end = canonical.find_first_of(".[", pos);
            if (end == std::string::npos) end = canonical.size();
            next->selectors.push_back(Selector{canonical.substr(pos, end - pos), {}});
            pos = end;
        }
    }
    return next;
}
base::DataSourceBase::shared_ptr ObservationPath::expression(bool live) const {
    auto selected = resolve();
    if (!selected) return {};
    return selected->getTypeInfo()->buildReadOnlyExpression(shared_ptr(new ObservationPath(*this)), live);
}
base::DataSourceBase::shared_ptr ObservationPath::member(const std::string& name) const {
    ObservationPath next(*this); next.selectors.push_back(Selector{name, {}}); return next.expression();
}
base::DataSourceBase::shared_ptr ObservationPath::member(base::DataSourceBase::shared_ptr index) const {
    ObservationPath next(*this); next.selectors.push_back(Selector{{}, index}); return next.expression();
}
}}
