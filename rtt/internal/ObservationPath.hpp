#ifndef RTT_INTERNAL_OBSERVATION_PATH_HPP
#define RTT_INTERNAL_OBSERVATION_PATH_HPP
#include "../base/DataSourceBase.hpp"
#include <boost/shared_ptr.hpp>
#include <stdexcept>
#include <string>
#include <vector>
namespace RTT { namespace internal {
class RTT_API ObservationUnavailable : public std::runtime_error {
public:
    ObservationUnavailable() : std::runtime_error("port observation is unavailable") {}
};
/** A live expression can provide one coherent, independently owned sample. */
class RTT_API ObservationExpression {
public:
    virtual ~ObservationExpression() = default;
    virtual base::DataSourceBase::shared_ptr frozen() const = 0;
};
/** Availability is independent of freshness for native port snapshots. */
class ObservationStatus {
public:
    virtual ~ObservationStatus() = default;
    virtual bool available() const = 0;
};
/** Non-real-time typed reflection path over independently owned observations. */
class RTT_API ObservationPath {
public:
    using shared_ptr = boost::shared_ptr<ObservationPath>;
    struct Selector {
        std::string name;
        base::DataSourceBase::shared_ptr index;
    };
private:
    base::DataSourceBase::shared_ptr root;
    std::vector<Selector> selectors;
    mutable bool present = false;
public:
    explicit ObservationPath(base::DataSourceBase::shared_ptr source);
    base::DataSourceBase::shared_ptr resolve(bool* available = nullptr) const;
    bool available() const;
    shared_ptr copy(std::map<const base::DataSourceBase*, base::DataSourceBase*>&) const;
    shared_ptr select(const std::string& canonical) const;
    base::DataSourceBase::shared_ptr expression(bool live = true) const;
    base::DataSourceBase::shared_ptr member(const std::string&) const;
    base::DataSourceBase::shared_ptr member(base::DataSourceBase::shared_ptr index) const;
};
}}
#endif
