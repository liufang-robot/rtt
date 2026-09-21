#ifndef RTT_PORT_ENDPOINT_HPP
#define RTT_PORT_ENDPOINT_HPP

#include "rtt-config.h"
#include "base/DataSourceBase.hpp"
#include <memory>
#include <string>
namespace RTT {
class Service;
namespace base { class PortInterface; }
/** A component/service-relative port and fixed member/index selection. */
struct RTT_API PortEndpoint {
    base::PortInterface* port = nullptr;
    std::string member;
    const types::TypeInfo* getTypeInfo() const;
};
/** Resolve dot/index syntax, validating the selection without changing topology.
 * Empty member selects the whole port. Legacy :: syntax is rejected. */
RTT_API bool resolvePortEndpoint(Service& service, const std::string& path,
                                PortEndpoint& result, std::string* error = nullptr);
/** Passive read-only observation. Input defaults/acquired images and committed
 * output samples are owned independently of the observed port's lifetime.
 * Each instance is for one observer; serialize access to a shared instance. */
class RTT_API PortObservation {
    struct Impl;
    std::unique_ptr<Impl> impl;
    explicit PortObservation(std::unique_ptr<Impl>);
public:
    ~PortObservation();
    static std::shared_ptr<PortObservation> create(const PortEndpoint&, std::string* error = nullptr);
    /** Live readonly expression: evaluate() reports availability; get()/rvalue()
     * throw internal::ObservationUnavailable when a sample is unavailable. */
    base::DataSourceBase::shared_ptr dataSource() const;
    /** Capture once for coherent non-real-time transport encoding. The returned
     * source retains both its sample and availability, including after recovery. */
    base::DataSourceBase::shared_ptr snapshot() const;
    bool available() const;
};
/** Transport-owned external input source. Creation/release require stopped
 * topology; stage() publishes only source storage for the next input boundary.
 * Encoding, assignment discovery and allocation happen outside component cycles.
 * Callers serialize stage/disconnect. Destruction uses normal safe port teardown,
 * which stops connected running owners before releasing the source. */
class RTT_API PortInputSource {
    struct Impl;
    std::unique_ptr<Impl> impl;
    explicit PortInputSource(std::unique_ptr<Impl>);
public:
    ~PortInputSource();
    static std::shared_ptr<PortInputSource> create(const PortEndpoint&, std::string* error = nullptr,
                                                  const std::string& sourceName = "external_input");
    const types::TypeInfo* getTypeInfo() const;
    bool connected() const;
    bool stage(base::DataSourceBase::shared_ptr value, std::string* error = nullptr);
    bool disconnect(std::string* error = nullptr);
};
}
#endif
