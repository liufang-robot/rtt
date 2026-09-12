#ifndef RTT_INTERNAL_PORT_DATA_ACCESS_HPP
#define RTT_INTERNAL_PORT_DATA_ACCESS_HPP

#include "../base/InputPortInterface.hpp"
#include "../base/OutputPortInterface.hpp"

namespace RTT {
template<class T> class InputPort;
template<class T> class OutputPort;
namespace internal {
/** Runtime capability for engines and transport-owned staging endpoints.
 * Component algorithms use data(); they must not drive these transfer phases. */
class PortDataAccess {
public:
    static base::DataSourceBase::shared_ptr image(base::InputPortInterface& port) { return port.imageSource(); }
    static base::DataSourceBase::shared_ptr image(base::OutputPortInterface& port) { return port.imageSource(); }
    static FlowStatus refresh(base::InputPortInterface& port) { return port.refreshImage(); }
    static WriteStatus commit(base::OutputPortInterface& port) { return port.commitImage(); }
    static void status(base::InputPortInterface& port, FlowStatus value) { port.setImageStatus(value); }
    static FlowStatus receive(base::InputPortInterface& port, base::DataSourceBase::shared_ptr value, bool copy_old = true) {
        return port.receive(value, copy_old);
    }
    static WriteStatus publish(base::OutputPortInterface& port, base::DataSourceBase::shared_ptr value) {
        return port.publish(value);
    }
    template<class T>
    static FlowStatus receive(InputPort<T>& port, T& value, bool copy_old = true) { return port.receive(value, copy_old); }
    template<class T, class U>
    static WriteStatus publish(OutputPort<T>& port, const U& value) { return port.publish(value); }
};
}
}
#endif
