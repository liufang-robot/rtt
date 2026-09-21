#include "unit.hpp"
#include <rtt/TaskContext.hpp>
#include <rtt/InputPort.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/internal/PortDataAccess.hpp>
#include <rtt/internal/ConnFactory.hpp>
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
thread_local bool countSourceCopies = false;
thread_local std::size_t sourceCopies = 0;
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
struct CopyCountedFrame {
    double positive = 0;
    double negative = 0;
    double payload[64] = {};
    CopyCountedFrame& operator=(const CopyCountedFrame& other) {
        if (countSourceCopies) ++sourceCopies;
        positive = other.positive;
        negative = other.negative;
        for (std::size_t i = 0; i != 64; ++i) payload[i] = other.payload[i];
        return *this;
    }
    template<class Archive> void serialize(Archive& ar, unsigned int) {
        ar & BOOST_SERIALIZATION_NVP(positive) & BOOST_SERIALIZATION_NVP(negative)
           & boost::serialization::make_nvp("payload", boost::serialization::make_array(payload, 64));
    }
};
struct TypesFixture {
    TypesFixture() {
        if (!types::Types()->type("cyclic_axis")) {
            types::Types()->addType(new types::StructTypeInfo<Axis, false>("cyclic_axis"));
            types::Types()->addType(new types::CArrayTypeInfo<types::carray<double> >("cyclic_doubles"));
            types::Types()->addType(new types::StructTypeInfo<Frame, false>("cyclic_frame"));
            types::Types()->addType(new types::StructTypeInfo<ShortFrame, false>("cyclic_short"));
            types::Types()->addType(new types::StructTypeInfo<SequenceFrame, false>("cyclic_sequence_frame"));
            types::Types()->addType(new types::StructTypeInfo<CopyCountedFrame, false>("cyclic_copy_counted_frame"));
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
    BOOST_CHECK(destination.getSourceConnections().empty());
    BOOST_REQUIRE(task.start()); step(task);
    BOOST_CHECK_EQUAL(destination.data().axis.position, 17.0);
    task.stop();
    OutputPort<double> replacement("replacement");
    BOOST_REQUIRE(connectMembers(replacement, "", destination, "axis.position"));
    const auto replacementSources = destination.getSourceConnections();
    BOOST_REQUIRE_EQUAL(replacementSources.size(), 1u);
    BOOST_CHECK_EQUAL(replacementSources[0].sourcePort, "replacement");
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
    BOOST_CHECK(destination.getSourceConnections().empty());
    BOOST_REQUIRE(task.start()); step(task); task.stop();
}

BOOST_AUTO_TEST_CASE(preexisting_multiple_whole_writers_fail_finalization) {
    TaskContext task("policy"); task.setActivity(new extras::SlaveActivity(0.01));
    InputPort<double> destination("destination");
    OutputPort<double> first("first"), second("second");
    BOOST_REQUIRE(first.connectTo(&destination));
    BOOST_REQUIRE(second.connectTo(&destination));
    task.addPort(destination);
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

BOOST_AUTO_TEST_CASE(member_fanout_source_copy_cost_is_independent_of_destination_count) {
    std::size_t singlePublishCopies = 0, singleAcquireCopies = 0;
    for (const std::size_t destinationCount : {std::size_t(1), std::size_t(16)}) {
        TaskContext task("fanout"); task.setActivity(new extras::SlaveActivity(0.01));
        OutputPort<CopyCountedFrame> source("source");
        std::vector<std::unique_ptr<InputPort<double>>> inputs;
        for (std::size_t i = 0; i != destinationCount; ++i) {
            inputs.emplace_back(new InputPort<double>("input_" + std::to_string(i)));
            auto service = i % 2 ? task.provides("io")->provides("nested") : task.provides();
            service->addPort(*inputs.back());
            BOOST_REQUIRE(connectMembers(source, i % 2 ? "negative" : "positive", *inputs.back(), ""));
        }
        BOOST_REQUIRE(task.start());
        source.data().positive = 1; source.data().negative = -1;
        BOOST_REQUIRE_EQUAL(PortDataAccess::commit(source), WriteSuccess);
        step(task);

        source.data().positive = 23; source.data().negative = -23;
        sourceCopies = 0;
        countSourceCopies = true;
        const auto publication = PortDataAccess::commit(source);
        countSourceCopies = false;
        const auto publicationCopies = sourceCopies;
        sourceCopies = 0;
        countSourceCopies = true;
        step(task);
        countSourceCopies = false;
        const auto acquisitionCopies = sourceCopies;
        BOOST_REQUIRE_EQUAL(publication, WriteSuccess);
        for (std::size_t i = 0; i != inputs.size(); ++i) {
            BOOST_CHECK_EQUAL(inputs[i]->data(), i % 2 ? -23.0 : 23.0);
            BOOST_CHECK_EQUAL(inputs[i]->status(), NewData);
        }
        if (destinationCount == 1) {
            singlePublishCopies = publicationCopies;
            singleAcquireCopies = acquisitionCopies;
            BOOST_CHECK_GT(singlePublishCopies, 0u);
            BOOST_CHECK_GT(singleAcquireCopies, 0u);
            // Acquiring the source must not also copy it into an unused staging snapshot.
            BOOST_CHECK_LE(singleAcquireCopies, 1u);
        } else {
            // More selected destinations must add scalar assignments, not full-frame copies.
            BOOST_CHECK_EQUAL(publicationCopies, singlePublishCopies);
            BOOST_CHECK_EQUAL(acquisitionCopies, singleAcquireCopies);
        }
        sourceCopies = 0;
        countSourceCopies = true;
        step(task);
        countSourceCopies = false;
        BOOST_CHECK_EQUAL(sourceCopies, 0u);
        for (const auto& input : inputs) BOOST_CHECK_EQUAL(input->status(), OldData);
        BOOST_REQUIRE(task.stop());
    }
}

BOOST_AUTO_TEST_CASE(shared_source_acquisition_keeps_distinct_input_ports_coherent) {
    TaskContext task("concurrent_fanout"); task.setActivity(new extras::SlaveActivity(0.01));
    InputPort<double> position("position"), velocity("velocity"), nestedPosition("position"), nestedVelocity("velocity");
    task.addPort(position); task.addPort(velocity);
    auto nested = task.provides("io")->provides("nested");
    nested->addPort(nestedPosition); nested->addPort(nestedVelocity);
    OutputPort<Frame> source("source");
    BOOST_REQUIRE(connectMembers(source, "axis.position", position, ""));
    BOOST_REQUIRE(connectMembers(source, "axis.velocity", velocity, ""));
    BOOST_REQUIRE(connectMembers(source, "axis.position", nestedPosition, ""));
    BOOST_REQUIRE(connectMembers(source, "axis.velocity", nestedVelocity, ""));
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
    bool coherent = true, consistentStatus = true;
    do {
        step(task);
        coherent = coherent && position.data() == -velocity.data()
            && position.data() == nestedPosition.data() && velocity.data() == nestedVelocity.data();
        consistentStatus = consistentStatus && position.status() == velocity.status()
            && position.status() == nestedPosition.status() && velocity.status() == nestedVelocity.status();
    } while (!done.load());
    producer.join();
    step(task);
    BOOST_CHECK(coherent);
    BOOST_CHECK(consistentStatus);
    BOOST_CHECK_EQUAL(position.data(), 20000.0);
    BOOST_CHECK_EQUAL(velocity.data(), -20000.0);
    BOOST_CHECK_EQUAL(nestedPosition.data(), 20000.0);
    BOOST_CHECK_EQUAL(nestedVelocity.data(), -20000.0);
    step(task);
    BOOST_CHECK_EQUAL(position.status(), OldData);
    BOOST_CHECK_EQUAL(nestedVelocity.status(), OldData);
    BOOST_REQUIRE(task.stop());
}

BOOST_AUTO_TEST_CASE(shared_source_fanout_survives_partial_disconnect_and_endpoint_destruction) {
    TaskContext task("fanout_lifetime"); task.setActivity(new extras::SlaveActivity(0.01));
    InputPort<double> first("first"), remaining("remaining");
    std::unique_ptr<InputPort<double>> temporary(new InputPort<double>("temporary"));
    std::unique_ptr<OutputPort<Frame>> source(new OutputPort<Frame>("source"));
    task.addPort(first); task.addPort(remaining);
    auto nested = task.provides("io"); nested->addPort(*temporary);
    BOOST_REQUIRE(connectMembers(*source, "axis.position", first, ""));
    BOOST_REQUIRE(connectMembers(*source, "axis.velocity", remaining, ""));
    BOOST_REQUIRE(connectMembers(*source, "axis.velocity", *temporary, ""));
    source->data().axis.position = 4; source->data().axis.velocity = -4;
    BOOST_REQUIRE_EQUAL(PortDataAccess::commit(*source), WriteSuccess);
    BOOST_REQUIRE(task.start()); step(task); BOOST_REQUIRE(task.stop());
    first.disconnect();
    BOOST_CHECK(!first.connected());
    BOOST_CHECK(first.getSourceConnections().empty());
    BOOST_CHECK(source->connectedTo(&remaining));
    BOOST_CHECK(source->connectedTo(temporary.get()));
    source->data().axis.position = 5; source->data().axis.velocity = -5;
    BOOST_REQUIRE_EQUAL(PortDataAccess::commit(*source), WriteSuccess);
    BOOST_REQUIRE(task.start()); step(task);
    BOOST_CHECK_EQUAL(first.data(), 4.0);
    BOOST_CHECK_EQUAL(first.status(), NoData);
    BOOST_CHECK_EQUAL(remaining.data(), -5.0);
    BOOST_CHECK_EQUAL(temporary->data(), -5.0);
    BOOST_REQUIRE(task.stop());

    BOOST_REQUIRE(connectMembers(*source, "axis.position", first, ""));
    BOOST_REQUIRE(task.start());
    temporary.reset();
    BOOST_CHECK(!task.isRunning());
    BOOST_CHECK(nested->getPort("temporary") == nullptr);
    BOOST_CHECK(source->connectedTo(&first));
    BOOST_CHECK(source->connectedTo(&remaining));
    source->data().axis.position = 7; source->data().axis.velocity = -7;
    BOOST_REQUIRE_EQUAL(PortDataAccess::commit(*source), WriteSuccess);
    BOOST_REQUIRE(task.start()); step(task);
    BOOST_CHECK_EQUAL(first.data(), 7.0);
    BOOST_CHECK_EQUAL(remaining.data(), -7.0);

    source.reset();
    BOOST_CHECK(!task.isRunning());
    BOOST_CHECK(!first.connected()); BOOST_CHECK(!remaining.connected());
    BOOST_CHECK(first.getSourceConnections().empty());
    BOOST_CHECK(remaining.getSourceConnections().empty());
    BOOST_REQUIRE(task.start()); step(task);
    BOOST_CHECK_EQUAL(first.data(), 7.0);
    BOOST_CHECK_EQUAL(remaining.data(), -7.0);
    BOOST_CHECK_EQUAL(first.status(), NoData);
    BOOST_CHECK_EQUAL(remaining.status(), NoData);
    BOOST_REQUIRE(task.stop());
}

BOOST_AUTO_TEST_CASE(shared_source_fanout_consumers_acquire_independent_cycles) {
    TaskContext first("first_consumer"), second("second_consumer");
    first.setActivity(new extras::SlaveActivity(0.01)); second.setActivity(new extras::SlaveActivity(0.01));
    InputPort<double> firstPosition("position"), firstVelocity("velocity"), secondPosition("position"), secondVelocity("velocity");
    first.addPort(firstPosition); first.provides("io")->addPort(firstVelocity);
    second.addPort(secondPosition); second.provides("io")->addPort(secondVelocity);
    OutputPort<Frame> source("source");
    BOOST_REQUIRE(connectMembers(source, "axis.position", firstPosition, ""));
    BOOST_REQUIRE(connectMembers(source, "axis.velocity", firstVelocity, ""));
    BOOST_REQUIRE(connectMembers(source, "axis.position", secondPosition, ""));
    BOOST_REQUIRE(connectMembers(source, "axis.velocity", secondVelocity, ""));
    BOOST_REQUIRE(first.start()); BOOST_REQUIRE(second.start());
    source.data().axis.position = 1; source.data().axis.velocity = -1;
    BOOST_REQUIRE_EQUAL(PortDataAccess::commit(source), WriteSuccess);
    step(first);
    BOOST_CHECK_EQUAL(firstPosition.data(), 1.0); BOOST_CHECK_EQUAL(firstVelocity.data(), -1.0);
    BOOST_CHECK_EQUAL(firstPosition.status(), NewData); BOOST_CHECK_EQUAL(firstVelocity.status(), NewData);
    BOOST_CHECK_EQUAL(secondPosition.status(), NoData); BOOST_CHECK_EQUAL(secondVelocity.status(), NoData);
    step(first);
    BOOST_CHECK_EQUAL(firstPosition.status(), OldData); BOOST_CHECK_EQUAL(firstVelocity.status(), OldData);
    source.data().axis.position = 2; source.data().axis.velocity = -2;
    BOOST_REQUIRE_EQUAL(PortDataAccess::commit(source), WriteSuccess);
    step(second);
    BOOST_CHECK_EQUAL(secondPosition.data(), 2.0); BOOST_CHECK_EQUAL(secondVelocity.data(), -2.0);
    BOOST_CHECK_EQUAL(secondPosition.status(), NewData); BOOST_CHECK_EQUAL(secondVelocity.status(), NewData);
    BOOST_CHECK_EQUAL(firstPosition.data(), 1.0); BOOST_CHECK_EQUAL(firstVelocity.data(), -1.0);
    step(second);
    BOOST_CHECK_EQUAL(secondPosition.status(), OldData); BOOST_CHECK_EQUAL(secondVelocity.status(), OldData);
    step(first);
    BOOST_CHECK_EQUAL(firstPosition.data(), 2.0); BOOST_CHECK_EQUAL(firstVelocity.data(), -2.0);
    BOOST_CHECK_EQUAL(firstPosition.status(), NewData); BOOST_CHECK_EQUAL(firstVelocity.status(), NewData);
    BOOST_REQUIRE(first.stop()); BOOST_REQUIRE(second.stop());
}

BOOST_AUTO_TEST_CASE(shared_source_new_destinations_join_pending_publications_without_replaying_consumed_data) {
    TaskContext task("joining_consumer"); task.setActivity(new extras::SlaveActivity(0.01));
    InputPort<double> first("first"), second("second"), third("third");
    task.addPort(first); task.provides("io")->addPort(second); task.addPort(third);
    third.setDataSample(99.0);
    OutputPort<Frame> source("source");
    BOOST_REQUIRE(connectMembers(source, "axis.position", first, ""));
    source.data().axis.position = 42;
    BOOST_REQUIRE_EQUAL(PortDataAccess::commit(source), WriteSuccess);
    BOOST_REQUIRE(connectMembers(source, "axis.position", second, ""));
    BOOST_REQUIRE(task.start()); step(task);
    BOOST_CHECK_EQUAL(first.data(), 42.0); BOOST_CHECK_EQUAL(second.data(), 42.0);
    BOOST_CHECK_EQUAL(first.status(), NewData); BOOST_CHECK_EQUAL(second.status(), NewData);
    BOOST_REQUIRE(task.stop());

    BOOST_REQUIRE(connectMembers(source, "axis.position", third, ""));
    BOOST_REQUIRE(task.start()); step(task);
    BOOST_CHECK_EQUAL(first.data(), 42.0); BOOST_CHECK_EQUAL(second.data(), 42.0);
    BOOST_CHECK_EQUAL(first.status(), OldData); BOOST_CHECK_EQUAL(second.status(), OldData);
    BOOST_CHECK_EQUAL(third.data(), 99.0); BOOST_CHECK_EQUAL(third.status(), NoData);
    BOOST_REQUIRE(task.stop());

    InputPort<double> invalid("invalid"); task.addPort(invalid);
    source.data().axis.position = 43;
    BOOST_REQUIRE_EQUAL(PortDataAccess::commit(source), WriteSuccess);
    BOOST_CHECK(!connectMembers(source, "axis.missing", invalid, ""));
    BOOST_CHECK(!invalid.connected());
    BOOST_REQUIRE(task.start()); step(task);
    for (auto* input : {&first, &second, &third}) {
        BOOST_CHECK_EQUAL(input->data(), 43.0);
        BOOST_CHECK_EQUAL(input->status(), NewData);
    }
    BOOST_REQUIRE(task.stop());
    first.disconnect(); second.disconnect(); third.disconnect();
    BOOST_CHECK(!source.connected());
    BOOST_CHECK(first.getSourceConnections().empty());
    BOOST_CHECK(second.getSourceConnections().empty());
    BOOST_CHECK(third.getSourceConnections().empty());
}

BOOST_AUTO_TEST_CASE(shared_source_feedback_observes_the_previous_owner_cycle) {
    class Feedback : public TaskContext {
    public:
        OutputPort<Frame> output{"output"};
        InputPort<double> position{"position"}, velocity{"velocity"};
        Feedback() : TaskContext("feedback") {
            setActivity(new extras::SlaveActivity(0.01));
            addPort(output); addPort(position); provides("io")->addPort(velocity);
        }
        void updateHook() override {
            output.data().axis.position = position.data() + 1;
            output.data().axis.velocity = velocity.data() - 1;
        }
    } task;
    BOOST_REQUIRE(connectMembers(task.output, "axis.position", task.position, ""));
    BOOST_REQUIRE(connectMembers(task.output, "axis.velocity", task.velocity, ""));
    BOOST_REQUIRE(task.start()); step(task);
    BOOST_CHECK_EQUAL(task.position.status(), NoData); BOOST_CHECK_EQUAL(task.velocity.status(), NoData);
    BOOST_CHECK_EQUAL(task.position.data(), 0.0); BOOST_CHECK_EQUAL(task.velocity.data(), 0.0);
    BOOST_CHECK_EQUAL(task.output.data().axis.position, 1.0);
    step(task);
    BOOST_CHECK_EQUAL(task.position.status(), NewData); BOOST_CHECK_EQUAL(task.velocity.status(), NewData);
    BOOST_CHECK_EQUAL(task.position.data(), 1.0); BOOST_CHECK_EQUAL(task.velocity.data(), -1.0);
    BOOST_CHECK_EQUAL(task.output.data().axis.position, 2.0);
    step(task);
    BOOST_CHECK_EQUAL(task.position.data(), 2.0); BOOST_CHECK_EQUAL(task.velocity.data(), -2.0);
    BOOST_REQUIRE(task.stop());
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

BOOST_AUTO_TEST_CASE(non_data_policy_kinds_reject_connection_creation) {
    for (int kind : {1, 2, 42, -2, ConnPolicy::UNBUFFERED}) {
        OutputPort<double> source("source"); InputPort<double> input("input");
        ConnPolicy policy = ConnPolicy::data(); policy.type = kind; policy.size = 3;
        BOOST_CHECK(!source.createConnection(input, policy));
        BOOST_CHECK(!source.connected()); BOOST_CHECK(!input.connected());
        base::ChannelElementBase::shared_ptr storage(internal::ConnFactory::buildDataStorage<double>(policy));
        BOOST_CHECK(!storage);
    }
}

BOOST_AUTO_TEST_CASE(owned_inputs_reject_second_whole_writer_including_shared_joins) {
    for (bool shared : {false, true}) {
        TaskContext task("single_writer"); InputPort<double> input("input"); task.addPort(input);
        OutputPort<double> first("first"), second("second");
        ConnPolicy policy = ConnPolicy::data(); if (shared) policy.buffer_policy = Shared;
        BOOST_REQUIRE(first.createConnection(input, policy));
        BOOST_CHECK(!second.createConnection(input, policy));
        if (shared) BOOST_CHECK(!second.createConnection(first.getSharedConnection(), policy));
        BOOST_CHECK_EQUAL(input.getSourceConnections().size(), 1u);
        BOOST_CHECK(task.finalizeConnections());
    }
}

BOOST_AUTO_TEST_CASE(shared_input_can_attach_before_its_only_writer) {
    ConnPolicy policy = ConnPolicy::data(); policy.buffer_policy = Shared;
    TaskContext task("consumer"); InputPort<double> input("input"); task.addPort(input);
    auto shared = internal::ConnFactory::buildSharedConnection<double>(0, &input, policy);
    BOOST_REQUIRE(shared);
    BOOST_REQUIRE(input.createConnection(shared, policy));
    OutputPort<double> source("source");
    BOOST_REQUIRE(source.createConnection(shared, policy));
    BOOST_REQUIRE_EQUAL(input.getSourceConnections().size(), 1u);
    BOOST_CHECK_EQUAL(input.getSourceConnections()[0].sourcePort, "source");
    BOOST_CHECK(task.finalizeConnections());
}

BOOST_AUTO_TEST_CASE(owned_input_rejects_existing_shared_multiwriter_graph) {
    ConnPolicy policy = ConnPolicy::data(); policy.buffer_policy = Shared;
    OutputPort<double> first("first"), second("second"); InputPort<double> staging("staging");
    BOOST_REQUIRE(first.createConnection(staging, policy));
    BOOST_REQUIRE(second.createConnection(staging, policy));
    TaskContext task("consumer"); InputPort<double> input("input"); task.addPort(input);
    BOOST_CHECK(!input.createConnection(first.getSharedConnection(), policy));
    // Registration can expose a graph assembled while the input was unowned.
    task.addPort(staging);
    BOOST_CHECK(!task.finalizeConnections());
    BOOST_CHECK(!task.start());
}

BOOST_AUTO_TEST_CASE(advanced_whole_channel_addition_cannot_inject_a_second_writer) {
    ConnPolicy policy = ConnPolicy::data();
    TaskContext task("consumer"); task.setActivity(new extras::SlaveActivity(0.01));
    InputPort<double> input("input"); task.addPort(input);
    OutputPort<double> first("first"), second("second");
    BOOST_REQUIRE(first.createConnection(input, policy));
    BOOST_REQUIRE_EQUAL(PortDataAccess::publish(first, 1.0), WriteSuccess);
    PortDataAccess::publish(second, 99.0);
    auto channel = boost::get<1>(first.getManager()->getConnections().front());
    BOOST_REQUIRE(task.start());
    std::unique_ptr<internal::ConnID> id(input.getPortID());
    BOOST_CHECK(!second.addConnection(id.get(), channel, policy));
    if (second.getManager()->connected()) id.release();
    step(task);
    BOOST_CHECK_EQUAL(input.data(), 1.0);
    BOOST_REQUIRE(task.stop());
}

BOOST_AUTO_TEST_CASE(rejected_remote_channel_handshake_preserves_the_existing_writer) {
    TaskContext task("consumer");
    InputPort<double> input("input"); task.addPort(input);
    OutputPort<double> source("source");
    const ConnPolicy policy = ConnPolicy::data();
    BOOST_REQUIRE(source.createConnection(input, policy));
    const auto channel = boost::get<1>(source.getManager()->getConnections().front());
    // A remote handshake supplies no local port ID. Repeated rejection must
    // release the temporary ID as well as retaining the established connection.
    for (int attempt = 0; attempt != 4; ++attempt)
        BOOST_CHECK(!input.getEndpoint()->channelReady(channel, policy, nullptr));
    BOOST_REQUIRE_EQUAL(input.getSourceConnections().size(), 1u);
    BOOST_CHECK_EQUAL(input.getSourceConnections()[0].sourcePort, "source");
    BOOST_CHECK(source.connected());
}

BOOST_AUTO_TEST_CASE(advanced_shared_connection_addition_and_removal_preserve_running_graph) {
    ConnPolicy policy = ConnPolicy::data(); policy.buffer_policy = Shared;
    TaskContext task("consumer"); task.setActivity(new extras::SlaveActivity(0.01));
    InputPort<double> input("input"); task.addPort(input);
    OutputPort<double> first("first"), second("second");
    BOOST_REQUIRE(first.createConnection(input, policy));
    auto shared = first.getSharedConnection();
    BOOST_REQUIRE(task.start());
    std::unique_ptr<internal::ConnID> id(shared->getConnID());
    BOOST_CHECK(!second.addConnection(id.get(), shared, policy));
    if (second.getManager()->connected()) id.release();
    std::unique_ptr<internal::ConnID> removal(shared->getConnID());
    BOOST_CHECK(!input.removeConnection(removal.get()));
    BOOST_CHECK(input.connected());
    BOOST_REQUIRE(task.stop());
}

BOOST_AUTO_TEST_CASE(shared_connections_freeze_indirect_topology_of_running_components) {
    ConnPolicy policy = ConnPolicy::data(); policy.buffer_policy = Shared;
    TaskContext task("consumer"); task.setActivity(new extras::SlaveActivity(0.01));
    InputPort<double> input("input"); task.addPort(input);
    OutputPort<double> first("first"), second("second"); InputPort<double> extra("extra");
    BOOST_REQUIRE(first.createConnection(input, policy));
    const auto channel = first.getSharedConnection();
    BOOST_REQUIRE(task.start());
    BOOST_CHECK(!second.createConnection(channel, policy));
    BOOST_CHECK(!extra.createConnection(channel, policy));
    first.disconnect();
    BOOST_CHECK(first.connected());
    BOOST_REQUIRE(task.stop());
    BOOST_CHECK(extra.createConnection(channel, policy));
    first.disconnect();
    BOOST_CHECK(!first.connected());
}

BOOST_AUTO_TEST_CASE(source_connections_describe_logical_whole_and_member_endpoints) {
    TaskContext producer("producer"), scalarProducer("scalar"), consumer("consumer");
    consumer.setActivity(new extras::SlaveActivity(0.01));
    OutputPort<Frame> source("state");
    OutputPort<double> scalar("value");
    InputPort<Frame> assembled("assembled"), whole("whole");
    InputPort<double> selected("selected");
    producer.provides("motion")->provides("feedback")->addPort(source);
    scalarProducer.addPort(scalar);
    consumer.provides("io")->addPort(assembled);
    consumer.provides("io")->addPort(whole);
    consumer.provides("io")->addPort(selected);

    BOOST_REQUIRE(source.connectTo(&whole));
    BOOST_REQUIRE(connectMembers(source, "axis.velocity", assembled, "axis.velocity"));
    BOOST_REQUIRE(connectMembers(source, "values[01]", assembled, "values[02]"));
    BOOST_REQUIRE(connectMembers(scalar, "", assembled, "axis.position"));
    BOOST_REQUIRE(connectMembers(source, "axis.position", selected, ""));

    const auto wholeSources = whole.getSourceConnections();
    BOOST_REQUIRE_EQUAL(wholeSources.size(), 1u);
    BOOST_CHECK_EQUAL(wholeSources[0].sourcePort, "producer.motion.feedback.state");
    BOOST_CHECK(wholeSources[0].sourceMember.empty());
    BOOST_CHECK(wholeSources[0].destinationMember.empty());
    const auto memberSources = assembled.getSourceConnections();
    BOOST_REQUIRE_EQUAL(memberSources.size(), 3u);
    BOOST_CHECK_EQUAL(memberSources[0].sourcePort, "scalar.value");
    BOOST_CHECK(memberSources[0].sourceMember.empty());
    BOOST_CHECK_EQUAL(memberSources[0].destinationMember, "axis.position");
    BOOST_CHECK_EQUAL(memberSources[1].sourcePort, "producer.motion.feedback.state");
    BOOST_CHECK_EQUAL(memberSources[1].sourceMember, "axis.velocity");
    BOOST_CHECK_EQUAL(memberSources[1].destinationMember, "axis.velocity");
    BOOST_CHECK_EQUAL(memberSources[2].sourcePort, "producer.motion.feedback.state");
    BOOST_CHECK_EQUAL(memberSources[2].sourceMember, "values[1]");
    BOOST_CHECK_EQUAL(memberSources[2].destinationMember, "values[2]");
    const auto selectedSources = selected.getSourceConnections();
    BOOST_REQUIRE_EQUAL(selectedSources.size(), 1u);
    BOOST_CHECK_EQUAL(selectedSources[0].sourcePort, "producer.motion.feedback.state");
    BOOST_CHECK_EQUAL(selectedSources[0].sourceMember, "axis.position");
    BOOST_CHECK(selectedSources[0].destinationMember.empty());

    source.data().axis.position = 3;
    source.data().axis.velocity = 4;
    source.data().values[1] = 5;
    BOOST_REQUIRE_EQUAL(PortDataAccess::commit(source), WriteSuccess);
    BOOST_REQUIRE_EQUAL(PortDataAccess::publish(scalar, 9.0), WriteSuccess);
    // Inspecting the graph must neither consume publications nor update images.
    BOOST_CHECK_EQUAL(assembled.getSourceConnections().size(), 3u);
    BOOST_CHECK_EQUAL(assembled.status(), NoData);
    BOOST_CHECK_EQUAL(assembled.data().axis.position, 0.0);
    BOOST_REQUIRE(consumer.start());
    step(consumer);
    BOOST_CHECK_EQUAL(assembled.data().axis.position, 9.0);
    BOOST_CHECK_EQUAL(assembled.data().axis.velocity, 4.0);
    BOOST_CHECK_EQUAL(assembled.data().values[2], 5.0);
    BOOST_CHECK_EQUAL(selected.data(), 3.0);
    BOOST_CHECK_EQUAL(assembled.status(), NewData);
    BOOST_CHECK_EQUAL(assembled.getSourceConnections().size(), 3u);
    BOOST_CHECK_EQUAL(assembled.status(), NewData);
    BOOST_REQUIRE(consumer.stop());
    source.disconnect();
    BOOST_CHECK(whole.getSourceConnections().empty());
    BOOST_CHECK(selected.getSourceConnections().empty());
    BOOST_REQUIRE_EQUAL(assembled.getSourceConnections().size(), 1u);
    BOOST_CHECK_EQUAL(assembled.getSourceConnections()[0].sourcePort, "scalar.value");
    BOOST_CHECK_EQUAL(memberSources[1].sourcePort, "producer.motion.feedback.state");
}

BOOST_AUTO_TEST_CASE(source_connections_enumerate_all_shared_writers_without_consuming_data) {
    ConnPolicy policy = ConnPolicy::data(ConnPolicy::LOCKED);
    policy.buffer_policy = Shared;
    OutputPort<double> first("first"), second("second");
    InputPort<double> input("input", policy);
    BOOST_REQUIRE(second.createConnection(input));
    BOOST_REQUIRE(first.createConnection(input));
    BOOST_REQUIRE_EQUAL(PortDataAccess::publish(second, 42.0), WriteSuccess);
    const auto sources = input.getSourceConnections();
    BOOST_REQUIRE_EQUAL(sources.size(), 2u);
    BOOST_CHECK_EQUAL(sources[0].sourcePort, "first");
    BOOST_CHECK_EQUAL(sources[1].sourcePort, "second");
    BOOST_CHECK(sources[0].destinationMember.empty());
    BOOST_CHECK(sources[0].sourceMember.empty());
    double value = 0;
    BOOST_CHECK_EQUAL(PortDataAccess::receive(input, value), NewData);
    BOOST_CHECK_EQUAL(value, 42.0);
    first.disconnect();
    const auto remaining = input.getSourceConnections();
    BOOST_REQUIRE_EQUAL(remaining.size(), 1u);
    BOOST_CHECK_EQUAL(remaining[0].sourcePort, "second");
    input.disconnect();
    BOOST_CHECK(input.getSourceConnections().empty());
}

BOOST_AUTO_TEST_CASE(source_connections_preserve_unknown_transport_identity_alongside_local_sources) {
    // Model a transport endpoint whose identity is a stream, not a local port.
    struct StreamOutput : OutputPort<double> {
        StreamOutput() : OutputPort<double>("must_not_be_reported") {}
        internal::ConnID* getPortID() const override {
            return new internal::StreamConnID("transport_stream");
        }
    } stream;
    OutputPort<double> local("local");
    InputPort<double> input("input");
    BOOST_REQUIRE(local.connectTo(&input));
    BOOST_REQUIRE(stream.connectTo(&input));
    const auto sources = input.getSourceConnections();
    BOOST_REQUIRE_EQUAL(sources.size(), 2u);
    BOOST_CHECK(sources[0].sourcePort.empty());
    BOOST_CHECK(sources[0].sourceMember.empty());
    BOOST_CHECK(sources[0].destinationMember.empty());
    BOOST_CHECK_EQUAL(sources[1].sourcePort, "local");
    input.disconnect();
    BOOST_CHECK(input.getSourceConnections().empty());
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
