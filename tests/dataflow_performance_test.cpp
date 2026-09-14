#include "unit.hpp"
#include <rtt/TaskContext.hpp>
#include <rtt/ExecutionEngine.hpp>
#include <rtt/InputPort.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/extras/SlaveActivity.hpp>
#include <chrono>
#include <array>
#include <iostream>

using namespace RTT;

// Measure complete automatic I/O boundaries, including the channel copies and
// committed observers, rather than timing only the component's data() access.
// FIFO drains and competing writers are channel tests in ports_test; they are
// deliberately not component execution modes in the cyclic API.
namespace {
class CyclicScalar : public TaskContext {
public:
    InputPort<double> input{"input"};
    OutputPort<double> output{"output"};
    double increment{1.0};
    explicit CyclicScalar(const std::string& name) : TaskContext(name) {
        setActivity(new extras::SlaveActivity(0.001));
        addPort(input);
        addPort(output);
    }
    void updateHook() override { output.data() = input.data() + increment; }
};

template<class Cycle>
void measure(const char* label, Cycle cycle) {
    for (unsigned i = 0; i < 10000; ++i) cycle();
    constexpr unsigned count = 100000;
    const auto begin = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < count; ++i) cycle();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const double ns = std::chrono::duration<double, std::nano>(elapsed).count() / count;
    std::cout << "CYCLIC_BENCH " << label << " ns/cycle=" << ns << " cycles=" << count << '\n';
    // This is evidence, not a machine-dependent latency assertion. Functional
    // assertions below ensure the measured loop actually moved data.
}
}

BOOST_AUTO_TEST_CASE(automatic_whole_port_two_component_cycle)
{
    CyclicScalar producer("producer"), consumer("consumer");
    BOOST_REQUIRE(producer.output.connectTo(&consumer.input));
    BOOST_REQUIRE(producer.start());
    BOOST_REQUIRE(consumer.start());
    measure("whole_scalar_pair", [&] {
        producer.getActivity()->execute();
        consumer.getActivity()->execute();
    });
    BOOST_CHECK_EQUAL(consumer.input.data(), 1.0);
    BOOST_CHECK_EQUAL(consumer.output.snapshot(), 2.0);
    BOOST_CHECK_EQUAL(consumer.input.status(), NewData);
    BOOST_CHECK(consumer.stop());
    BOOST_CHECK(producer.stop());
}

BOOST_AUTO_TEST_CASE(automatic_whole_port_fanout_cycle)
{
    CyclicScalar producer("producer"), a("a"), b("b"), c("c"), d("d");
    std::array<CyclicScalar*, 4> consumers{{&a, &b, &c, &d}};
    for (auto* consumer : consumers) BOOST_REQUIRE(producer.output.connectTo(&consumer->input));
    BOOST_REQUIRE(producer.start());
    for (auto* consumer : consumers) BOOST_REQUIRE(consumer->start());
    measure("whole_scalar_fanout_4", [&] {
        producer.getActivity()->execute();
        for (auto* consumer : consumers) consumer->getActivity()->execute();
    });
    for (auto* consumer : consumers) {
        BOOST_CHECK_EQUAL(consumer->input.data(), 1.0);
        BOOST_CHECK_EQUAL(consumer->output.snapshot(), 2.0);
        BOOST_CHECK(consumer->stop());
    }
    BOOST_CHECK(producer.stop());
}
