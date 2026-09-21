#ifndef RTT_TRANSPORT_TEST_HPP
#define RTT_TRANSPORT_TEST_HPP

#include <rtt/InputPort.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/OperationCaller.hpp>
#include <rtt/internal/PortDataAccess.hpp>
#include <chrono>
#include <thread>

inline bool stepTransportPeer(RTT::TaskContext& peer)
{
    RTT::OperationCaller<bool()> step = peer.getOperation("stepPorts");
    return step.ready() && step();
}

// Await the expected value at an explicit receive boundary. Transport workers
// may deliver intermediate publications while a remote echo cycle catches up.
inline RTT::FlowStatus receiveTransportValue(RTT::InputPort<double>& input,
                                             double& value, double expected,
                                             RTT::TaskContext* echo_peer = nullptr)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    RTT::FlowStatus status;
    do {
        if (echo_peer && !stepTransportPeer(*echo_peer)) return RTT::NoData;
        status = RTT::internal::PortDataAccess::receive(input, value);
        if (status == RTT::NewData && value == expected) return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return status;
}

#endif
