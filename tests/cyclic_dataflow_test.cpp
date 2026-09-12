#include "unit.hpp"
#include <rtt/TaskContext.hpp>
#include <rtt/InputPort.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/internal/PortDataAccess.hpp>
#include <rtt/extras/SlaveActivity.hpp>
#include <rtt/types/StructTypeInfo.hpp>
#include <rtt/types/CArrayTypeInfo.hpp>
#include <rtt/types/TypeInfoRepository.hpp>
#include <boost/serialization/nvp.hpp>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <new>

namespace {
thread_local bool countCycleAllocations = false;
thread_local std::size_t cycleAllocations = 0;
}
void* operator new(std::size_t size) {
    if (countCycleAllocations) ++cycleAllocations;
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
[[gnu::noinline]] void operator delete(void* pointer) noexcept { std::free(pointer); }
[[gnu::noinline]] void operator delete[](void* pointer) noexcept { std::free(pointer); }
[[gnu::noinline]] void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
[[gnu::noinline]] void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

using namespace RTT;
using RTT::internal::PortDataAccess;

namespace {
struct Axis {
    double position = 0;
    double velocity = 0;
    template<class Archive> void serialize(Archive& ar, unsigned int) {
        ar & BOOST_SERIALIZATION_NVP(position) & BOOST_SERIALIZATION_NVP(velocity);
    }
};
struct Frame {
    Axis axis;
    double values[3] = {};
    int mode = 0;
    template<class Archive> void serialize(Archive& ar, unsigned int) {
        ar & BOOST_SERIALIZATION_NVP(axis) & boost::serialization::make_nvp("values", boost::serialization::make_array(values, 3)) & BOOST_SERIALIZATION_NVP(mode);
    }
};
struct ShortFrame {
    double values[2] = {};
    template<class Archive> void serialize(Archive& ar, unsigned int) {
        ar & boost::serialization::make_nvp("values", boost::serialization::make_array(values, 2));
    }
};
struct SequenceFrame {
    std::vector<double> values;
    template<class Archive> void serialize(Archive& ar, unsigned int) { ar & BOOST_SERIALIZATION_NVP(values); }
};
struct TypesFixture {
    TypesFixture() {
        if (!types::Types()->type("cyclic_axis")) {
            types::Types()->addType(new types::StructTypeInfo<Axis, false>("cyclic_axis"));
            types::Types()->addType(new types::CArrayTypeInfo<types::carray<double> >("cyclic_doubles"));
            types::Types()->addType(new types::StructTypeInfo<Frame, false>("cyclic_frame"));
            types::Types()->addType(new types::StructTypeInfo<ShortFrame, false>("cyclic_short"));
            types::Types()->addType(new types::StructTypeInfo<SequenceFrame, false>("cyclic_sequence_frame"));
        }
    }
};
class CyclicTask : public TaskContext {
public:
    InputPort<double> input;
    OutputPort<double> output;
    InputPort<double>* observer = 0;
    double observedInput = -1;
    double observedOutput = -1;
    FlowStatus observedStatus = NoData;
    int failure = 0;
    OutputPort<double>* changingSource = 0;
    double inputAfterPublish = -1;
    CyclicTask(const std::string& name) : TaskContext(name), input("input"), output("output") {
        setActivity(new extras::SlaveActivity(0.01));
        addPort(input); addPort(output);
    }
    ~CyclicTask() { if (isRunning()) stop(); }
    void updateHook() override {
        observedInput = input.data();
        output.data() = observedInput + 1;
        if (changingSource) PortDataAccess::publish(*changingSource, 99.0);
        inputAfterPublish = input.data();
        if (observer) observedStatus = PortDataAccess::receive(*observer, observedOutput);
        if (failure == 1) throw std::runtime_error("hook fault");
        if (failure == 2) error();
        if (failure == 3) stop();
    }
};
void step(TaskContext& task) { task.getActivity()->execute(); }
}

BOOST_FIXTURE_TEST_SUITE(CyclicDataFlowTest, TypesFixture)

BOOST_AUTO_TEST_CASE(native_cycle_refreshes_before_hook_and_publishes_after_hook) {
    OutputPort<double> source("source");
    InputPort<double> observer("observer");
    CyclicTask task("consumer");
    BOOST_REQUIRE(source.connectTo(&task.input));
    BOOST_REQUIRE(task.output.connectTo(&observer));
    task.observer = &observer;
    BOOST_REQUIRE_EQUAL(PortDataAccess::publish(source, 41.0), WriteSuccess);
    BOOST_REQUIRE(task.start());
    step(task);
    BOOST_CHECK_EQUAL(task.observedInput, 41.0);
    BOOST_CHECK_EQUAL(task.observedStatus, NoData);
    double value = -1;
    BOOST_CHECK_EQUAL(PortDataAccess::receive(observer, value), NewData);
    BOOST_CHECK_EQUAL(value, 42.0);
    BOOST_CHECK_EQUAL(task.input.status(), NewData);
    step(task);
    BOOST_CHECK_EQUAL(task.input.status(), OldData);
    BOOST_CHECK_EQUAL(task.observedInput, 41.0);
}

BOOST_AUTO_TEST_CASE(failed_or_stopped_hooks_do_not_publish_partial_outputs) {
    for (int failure = 1; failure <= 3; ++failure) {
        CyclicTask task("faulting");
        InputPort<double> observer("observer");
        BOOST_REQUIRE(task.output.connectTo(&observer));
        BOOST_REQUIRE(task.start());
        step(task);
        double value = -1;
        BOOST_REQUIRE_EQUAL(PortDataAccess::receive(observer, value), NewData);
        BOOST_REQUIRE_EQUAL(value, 1.0);
        task.failure = failure;
        step(task);
        BOOST_CHECK_EQUAL(PortDataAccess::receive(observer, value), OldData);
        BOOST_CHECK_EQUAL(value, 1.0);
    }
}

BOOST_AUTO_TEST_CASE(nested_service_ports_share_owner_cycle) {
    CyclicTask task("nested");
    InputPort<double> deepInput("in");
    OutputPort<double> deepOutput("out");
    OutputPort<double> source("source");
    InputPort<double> observer("observer");
    Service::shared_ptr nested = task.provides("first")->provides("second");
    nested->addPort(deepInput); nested->addPort(deepOutput);
    BOOST_CHECK_EQUAL(deepInput.getFullName(), "nested.first.second.in");
    BOOST_REQUIRE(source.connectTo(&deepInput));
    BOOST_REQUIRE(deepOutput.connectTo(&observer));
    BOOST_REQUIRE_EQUAL(PortDataAccess::publish(source, 12.0), WriteSuccess);
    deepOutput.data() = 23;
    BOOST_REQUIRE(task.start()); step(task);
    BOOST_CHECK_EQUAL(deepInput.data(), 12);
    double value = 0;
    BOOST_CHECK_EQUAL(PortDataAccess::receive(observer, value), NewData);
    BOOST_CHECK_EQUAL(value, 23);
    task.stop();
}

BOOST_AUTO_TEST_CASE(typed_members_assemble_multiple_sources_and_retain_unmapped_fields) {
    TaskContext task("assembly"); task.setActivity(new extras::SlaveActivity(0.01));
    InputPort<Frame> destination("destination"); task.addPort(destination);
    OutputPort<double> scalar("scalar"); OutputPort<Frame> frame("frame");
    BOOST_REQUIRE(connectMembers(scalar, "", destination, "axis.position"));
    BOOST_REQUIRE(connectMembers(frame, "axis.velocity", destination, "axis.velocity"));
    BOOST_REQUIRE(connectMembers(frame, "values[1]", destination, "values[2]"));
    BOOST_REQUIRE(scalar.connectedTo(&destination));
    BOOST_REQUIRE(destination.connectedTo(&scalar));
    frame.data().axis.velocity = 7.5; frame.data().values[1] = 91;
    PortDataAccess::commit(frame); PortDataAccess::publish(scalar, 4.5);
    BOOST_REQUIRE(task.finalizeConnections()); BOOST_REQUIRE(task.start()); step(task);
    BOOST_CHECK_EQUAL(destination.data().axis.position, 4.5);
    BOOST_CHECK_EQUAL(destination.data().axis.velocity, 7.5);
    BOOST_CHECK_EQUAL(destination.data().values[2], 91);
    BOOST_CHECK_EQUAL(destination.data().mode, 0);
    BOOST_CHECK_EQUAL(destination.status(), NewData);
    step(task); BOOST_CHECK_EQUAL(destination.status(), OldData);
    PortDataAccess::publish(scalar, 8.5); step(task);
    BOOST_CHECK_EQUAL(destination.data().axis.position, 8.5);
    BOOST_CHECK_EQUAL(destination.data().axis.velocity, 7.5);
    task.stop();
    BOOST_CHECK(scalar.disconnect(&destination));
    BOOST_CHECK(!scalar.connectedTo(&destination));
}

BOOST_AUTO_TEST_CASE(selected_structs_whole_values_and_fixed_arrays_are_copied) {
    TaskContext task("selection"); task.setActivity(new extras::SlaveActivity(0.01));
    InputPort<Axis> axis("axis"); InputPort<Frame> destination("destination");
    InputPort<double> scalar("scalar");
    task.addPort(axis); task.addPort(destination); task.addPort(scalar);
    OutputPort<Frame> source("source"); OutputPort<Axis> axisSource("axis_source");
    BOOST_REQUIRE(connectMembers(source, "axis", axis, ""));
    BOOST_REQUIRE(connectMembers(axisSource, "", destination, "axis"));
    BOOST_REQUIRE(connectMembers(source, "values", destination, "values"));
    BOOST_REQUIRE(connectMembers(source, "axis.position", scalar, ""));
    source.data().axis.position = 31; source.data().axis.velocity = 32;
    source.data().values[0] = 5; source.data().values[1] = 6; source.data().values[2] = 7;
    axisSource.data().position = 81; axisSource.data().velocity = 82;
    PortDataAccess::commit(source); PortDataAccess::commit(axisSource);
    BOOST_REQUIRE(task.start()); step(task);
    BOOST_CHECK_EQUAL(axis.data().position, 31); BOOST_CHECK_EQUAL(axis.data().velocity, 32);
    BOOST_CHECK_EQUAL(destination.data().axis.position, 81);
    BOOST_CHECK_EQUAL(destination.data().axis.velocity, 82);
    BOOST_CHECK_EQUAL(destination.data().values[0], 5);
    BOOST_CHECK_EQUAL(destination.data().values[1], 6);
    BOOST_CHECK_EQUAL(destination.data().values[2], 7);
    BOOST_CHECK_EQUAL(scalar.data(), 31);
    source.data().values[0] = 100;
    BOOST_CHECK_EQUAL(destination.data().values[0], 5);
    task.stop();
}

BOOST_AUTO_TEST_CASE(invalid_types_paths_shapes_and_overlapping_writers_are_rejected) {
    TaskContext task("invalid"); task.setActivity(new extras::SlaveActivity(0.01)); InputPort<Frame> destination("destination"); task.addPort(destination);
    OutputPort<double> scalar("scalar"); OutputPort<int> integer("integer");
    OutputPort<Frame> source("source"); OutputPort<ShortFrame> shortSource("short");
    BOOST_CHECK(!connectMembers(scalar, "", destination, "missing"));
    BOOST_CHECK(!connectMembers(integer, "", destination, "axis.position"));
    BOOST_CHECK(!connectMembers(source, "values[-1]", destination, "values[0]"));
    BOOST_CHECK(!connectMembers(source, "values[3]", destination, "values[0]"));
    BOOST_CHECK(!connectMembers(source, "values[1x]", destination, "values[0]"));
    BOOST_CHECK(!connectMembers(source, "values[1].", destination, "values[0]"));
    BOOST_CHECK(!connectMembers(shortSource, "values", destination, "values"));
    BOOST_REQUIRE(connectMembers(source, "axis", destination, "axis"));
    BOOST_CHECK(!connectMembers(scalar, "", destination, "axis.position"));
    BOOST_CHECK(!connectMembers(source, "", destination, ""));
    BOOST_REQUIRE(task.start());
    BOOST_CHECK(!connectMembers(scalar, "", destination, "values[0]"));
    BOOST_CHECK(!source.disconnect(&destination));
    BOOST_CHECK(source.connectedTo(&destination));
    task.stop();
}

BOOST_AUTO_TEST_CASE(input_image_remains_stable_when_next_sample_arrives_during_hook) {
    CyclicTask task("stable"); OutputPort<double> source("source");
    BOOST_REQUIRE(source.connectTo(&task.input)); task.changingSource = &source;
    PortDataAccess::publish(source, 41.0);
    BOOST_REQUIRE(task.start()); step(task);
    BOOST_CHECK_EQUAL(task.observedInput, 41.0);
    BOOST_CHECK_EQUAL(task.inputAfterPublish, 41.0);
    step(task);
    BOOST_CHECK_EQUAL(task.observedInput, 99.0);
}

BOOST_AUTO_TEST_CASE(source_and_destination_destruction_invalidate_and_allow_reconfiguration) {
    TaskContext task("lifetime"); task.setActivity(new extras::SlaveActivity(0.01));
    InputPort<Frame> destination("destination"); task.addPort(destination);
    std::unique_ptr<OutputPort<double> > source(new OutputPort<double>("source"));
    BOOST_REQUIRE(connectMembers(*source, "", destination, "axis.position"));
    PortDataAccess::publish(*source, 17.0);
    BOOST_REQUIRE(task.start()); step(task);
    BOOST_REQUIRE_EQUAL(destination.data().axis.position, 17.0);
    source.reset();
    BOOST_CHECK(!task.isRunning());
    BOOST_CHECK(!destination.connected());
    BOOST_REQUIRE(task.start()); step(task);
    BOOST_CHECK_EQUAL(destination.data().axis.position, 17.0);
    task.stop();
    OutputPort<double> replacement("replacement");
    BOOST_REQUIRE(connectMembers(replacement, "", destination, "axis.position"));
    PortDataAccess::publish(replacement, 29.0);
    BOOST_REQUIRE(task.start()); step(task);
    BOOST_CHECK_EQUAL(destination.data().axis.position, 29.0);
    task.stop();
    std::unique_ptr<InputPort<double> > temporary(new InputPort<double>("temporary"));
    task.addPort(*temporary);
    BOOST_REQUIRE(connectMembers(replacement, "", *temporary, ""));
    BOOST_REQUIRE(task.start()); temporary.reset();
    BOOST_CHECK(!task.isRunning());
    BOOST_CHECK(task.getPort("temporary") == 0);
    BOOST_REQUIRE(task.start()); step(task); task.stop();
}

BOOST_AUTO_TEST_CASE(service_removal_detaches_nested_mapping_endpoints_and_active_mutation_is_rejected) {
    TaskContext task("services"); task.setActivity(new extras::SlaveActivity(0.01));
    InputPort<Frame> destination("destination"); OutputPort<double> source("source");
    Service::shared_ptr outer = task.provides("outer");
    Service::shared_ptr inner = outer->provides("inner"); inner->addPort(destination);
    BOOST_REQUIRE(connectMembers(source, "", destination, "axis.position"));
    BOOST_REQUIRE(task.start());
    BOOST_CHECK_THROW(task.provides()->removeService("outer"), std::runtime_error);
    BOOST_CHECK_THROW(inner->removePort("destination"), std::runtime_error);
    BOOST_CHECK(source.connectedTo(&destination)); task.stop();
    task.provides()->removeService("outer");
    BOOST_CHECK(!source.connectedTo(&destination));
    BOOST_CHECK(destination.getInterface() == 0);
    BOOST_REQUIRE(task.start()); step(task); task.stop();
}

BOOST_AUTO_TEST_CASE(queued_state_and_multiple_whole_writers_fail_finalization) {
    TaskContext task("policy"); task.setActivity(new extras::SlaveActivity(0.01));
    InputPort<double> destination("destination"); task.addPort(destination);
    OutputPort<double> first("first"), second("second");
    BOOST_REQUIRE(first.createConnection(destination, ConnPolicy::buffer(4)));
    BOOST_CHECK(!task.finalizeConnections()); BOOST_CHECK(!task.start());
    first.disconnect();
    BOOST_REQUIRE(first.connectTo(&destination));
    BOOST_REQUIRE(second.connectTo(&destination));
    BOOST_CHECK(!task.finalizeConnections()); BOOST_CHECK(!task.start());
}

BOOST_AUTO_TEST_CASE(fixed_struct_mapping_cycles_allocate_no_cpp_heap_storage) {
    TaskContext task("bounded"); task.setActivity(new extras::SlaveActivity(0.01));
    InputPort<Frame> destination("destination"); task.provides("nested")->addPort(destination);
    OutputPort<Frame> source("source");
    BOOST_REQUIRE(connectMembers(source, "axis", destination, "axis"));
    BOOST_REQUIRE(connectMembers(source, "values", destination, "values"));
    source.data().axis.position = 4;
    BOOST_REQUIRE(task.start()); PortDataAccess::commit(source); step(task);
    cycleAllocations = 0;
    countCycleAllocations = true;
    for (int i = 0; i < 10000; ++i) {
        source.data().axis.position = i;
        PortDataAccess::commit(source); step(task);
    }
    countCycleAllocations = false;
    BOOST_CHECK_EQUAL(cycleAllocations, std::size_t(0));
    BOOST_CHECK_EQUAL(destination.data().axis.position, 9999);
    task.stop();
}

BOOST_AUTO_TEST_CASE(shared_source_snapshot_keeps_concurrent_related_fields_coherent) {
    TaskContext task("concurrent"); task.setActivity(new extras::SlaveActivity(0.01));
    InputPort<Frame> destination("destination"); task.addPort(destination);
    OutputPort<Frame> source("source");
    BOOST_REQUIRE(connectMembers(source, "axis.position", destination, "axis.position"));
    BOOST_REQUIRE(connectMembers(source, "axis.velocity", destination, "axis.velocity"));
    BOOST_REQUIRE(task.start());
    std::atomic<bool> done(false);
    std::thread producer([&] {
        Frame value;
        for (int i = 1; i <= 20000; ++i) {
            value.axis.position = i; value.axis.velocity = -i;
            PortDataAccess::publish(source, value);
        }
        done.store(true);
    });
    bool coherent = true;
    do {
        step(task);
        coherent = coherent && destination.data().axis.position == -destination.data().axis.velocity;
    } while (!done.load());
    producer.join();
    BOOST_CHECK(coherent);
    task.stop();
}

BOOST_AUTO_TEST_CASE(parameterless_disconnect_rejects_a_running_source_endpoint) {
    for (int members = 0; members != 2; ++members) {
        TaskContext producer("producer"), consumer("consumer");
        producer.setActivity(new extras::SlaveActivity(0.01));
        consumer.setActivity(new extras::SlaveActivity(0.01));
        OutputPort<Frame> source("source"); InputPort<Frame> destination("destination");
        producer.addPort(source); consumer.addPort(destination);
        BOOST_REQUIRE(connectMembers(source, members ? "axis" : "", destination, members ? "axis" : ""));
        BOOST_REQUIRE(producer.start());
        destination.disconnect();
        BOOST_CHECK(source.connectedTo(&destination));
        producer.stop();
        destination.disconnect();
        BOOST_CHECK(!source.connectedTo(&destination));
    }
}

BOOST_AUTO_TEST_CASE(operation_only_services_do_not_change_running_port_topology) {
    CyclicTask task("operations"); InputPort<double> observer("observer");
    BOOST_REQUIRE(task.output.connectTo(&observer));
    BOOST_REQUIRE(task.start());
    BOOST_REQUIRE(task.provides()->addService(Service::shared_ptr(new Service("utility"))));
    step(task);
    double value = 0;
    BOOST_CHECK_EQUAL(PortDataAccess::receive(observer, value), NewData);
    BOOST_CHECK_EQUAL(value, 1.0);
    task.provides()->removeService("utility");
    step(task);
    BOOST_CHECK_EQUAL(PortDataAccess::receive(observer, value), NewData);
}

BOOST_AUTO_TEST_CASE(dynamic_sequence_indices_and_root_array_views_are_rejected) {
    TaskContext task("sequence_validation");
    InputPort<Frame> destination("destination"); task.addPort(destination);
    OutputPort<std::vector<double> > source("dynamic_source"); source.data() = {11, 22};
    BOOST_CHECK(!connectMembers(source, "[0]", destination, "axis.position"));
    double backing[2] = {4, 5}, other[2] = {};
    OutputPort<types::carray<double> > view("view");
    InputPort<types::carray<double> > inputView("input_view");
    view.setDataSample(types::carray<double>(backing, 2));
    inputView.setDataSample(types::carray<double>(other, 2));
    BOOST_CHECK(!view.connectTo(&inputView));
    task.addPort(view);
    BOOST_CHECK(!task.finalizeConnections());
}

BOOST_AUTO_TEST_CASE(whole_dynamic_sequence_members_follow_resized_values) {
    TaskContext task("dynamic_values"); task.setActivity(new extras::SlaveActivity(0.01));
    OutputPort<std::vector<double> > source("source");
    InputPort<SequenceFrame> destination("destination"); task.addPort(destination);
    source.data() = {1, 2};
    BOOST_REQUIRE(connectMembers(source, "", destination, "values"));
    BOOST_REQUIRE(task.start()); PortDataAccess::commit(source); step(task);
    BOOST_REQUIRE_EQUAL(destination.data().values.size(), std::size_t(2));
    BOOST_CHECK_EQUAL(destination.data().values[1], 2);
    source.data() = {5, 6, 7, 8}; PortDataAccess::commit(source); step(task);
    BOOST_REQUIRE_EQUAL(destination.data().values.size(), std::size_t(4));
    BOOST_CHECK_EQUAL(destination.data().values[3], 8);
    source.data().clear(); PortDataAccess::commit(source); step(task);
    BOOST_CHECK(destination.data().values.empty());
    task.stop();
}

BOOST_AUTO_TEST_CASE(cyclic_and_missing_service_graphs_fail_finalization_and_teardown_safely) {
    TaskContext task("service_graph");
    Service::shared_ptr first = task.provides("first");
    Service::shared_ptr second = first->provides("second");
    second->setOwner(0);
    BOOST_REQUIRE(second->addService(first));
    BOOST_CHECK(!task.finalizeConnections());
    second->removeService("first");
    BOOST_REQUIRE(task.finalizeConnections());
}

BOOST_AUTO_TEST_SUITE_END()
