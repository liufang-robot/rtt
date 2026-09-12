#ifndef RTT_INTERNAL_CYCLIC_DATA_FLOW_HPP
#define RTT_INTERNAL_CYCLIC_DATA_FLOW_HPP

#include "../rtt-config.h"
#include "../rtt-fwd.hpp"
#include "../base/rtt-base-fwd.hpp"
#include <string>
#include <memory>

namespace RTT { namespace internal {
/** Prepared component-local I/O. All discovery and assignment construction is
 * performed while stopped; refresh/commit execute only prebound operations. */
class RTT_API CyclicDataFlow {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit CyclicDataFlow(TaskContext& owner);
    ~CyclicDataFlow();
    bool connect(base::OutputPortInterface&, const std::string&,
                 base::InputPortInterface&, const std::string&);
    static bool validateWhole(base::OutputPortInterface&, base::InputPortInterface&);
    bool finalize();
    void invalidate();
    bool refresh();
    bool commit();
    bool valid() const;
    TaskContext& owner() const;
    bool connectionChangeAllowed(const base::PortInterface&) const;
    bool contains(const base::PortInterface&, const base::PortInterface* other = 0) const;
    bool disconnect(base::PortInterface&, base::PortInterface* other = 0);
    void forget(base::PortInterface&);
};
}}
#endif
