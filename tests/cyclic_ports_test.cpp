#include <boost/test/unit_test.hpp>
#include <rtt/InputPort.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/extras/SlaveActivity.hpp>
#include <memory>
#include <type_traits>
#include <atomic>
#include <thread>
#include <vector>
#include <condition_variable>
#include <chrono>
#include <rtt/types/TemplateTypeInfo.hpp>
#include <rtt/types/TypeInfoRepository.hpp>

#if __has_include(<rtt/internal/PortDataAccess.hpp>)
#include <rtt/internal/PortDataAccess.hpp>
#define RTT_HAS_PORT_IMAGES 1
#endif

using namespace RTT;

template<class P>
concept ConsumingPort = requires(P& port, double& sample) { port.read(sample); };

template<class P>
concept PublishingPort = requires(P& port, double sample) { port.write(sample); };

template<class Input, class Output>
void checkImageBoundary()
{
    if constexpr (requires(Input& in, Output& out) { in.data(); out.data(); }) {
#ifdef RTT_HAS_PORT_IMAGES
        static_assert(std::is_same_v<decltype(std::declval<Input&>().data()), const double&>);
        static_assert(std::is_same_v<decltype(std::declval<Output&>().data()), double&>);
        Input input("input");
        Output output("output");
        BOOST_REQUIRE(output.connectTo(&input));
        output.data() = 7.0;
        BOOST_CHECK_EQUAL(internal::PortDataAccess::refresh(input), NoData);
        BOOST_CHECK_EQUAL(input.data(), 0.0);
        double snapshot = -1.0;
        BOOST_CHECK(!output.snapshot(snapshot));
        BOOST_CHECK_EQUAL(internal::PortDataAccess::commit(output), WriteSuccess);
        BOOST_CHECK_EQUAL(internal::PortDataAccess::refresh(input), NewData);
        BOOST_CHECK_EQUAL(input.data(), 7.0);
        BOOST_CHECK(output.snapshot(snapshot));
        BOOST_CHECK_EQUAL(snapshot, 7.0);
        output.data() = 19.0;
        BOOST_CHECK_EQUAL(input.data(), 7.0);
        BOOST_CHECK_EQUAL(internal::PortDataAccess::refresh(input), OldData);
        BOOST_CHECK_EQUAL(input.data(), 7.0);
        BOOST_CHECK(output.snapshot(snapshot));
        BOOST_CHECK_EQUAL(snapshot, 7.0);
#else
        BOOST_FAIL("native port image transfer capability is missing");
#endif
    } else {
        BOOST_FAIL("ports do not expose stable cyclic data images");
    }
}

BOOST_AUTO_TEST_CASE(component_ports_have_no_manual_transfer_api)
{
    BOOST_CHECK(!ConsumingPort<InputPort<double>>);
    BOOST_CHECK(!PublishingPort<OutputPort<double>>);
    BOOST_CHECK(!ConsumingPort<base::InputPortInterface>);
}

BOOST_AUTO_TEST_CASE(component_port_images_change_only_at_runtime_boundaries)
{
    checkImageBoundary<InputPort<double>, OutputPort<double>>();
}

BOOST_AUTO_TEST_CASE(script_port_services_do_not_expose_manual_transfers)
{
    InputPort<double> input("input");
    OutputPort<double> output("output");
    TaskContext owner("script_owner");
    owner.addPort(input);
    owner.addPort(output);
    auto input_service = owner.provides()->getService("input");
    auto output_service = owner.provides()->getService("output");
    BOOST_REQUIRE(input_service);
    BOOST_REQUIRE(output_service);
    BOOST_CHECK(!input_service->hasOperation("read"));
    BOOST_CHECK(!input_service->hasOperation("clear"));
    BOOST_CHECK(!output_service->hasOperation("write"));
}

BOOST_AUTO_TEST_CASE(snapshot_observers_are_independent_and_survive_port_destruction)
{
    internal::DataSource<double>::shared_ptr first, second;
    {
        OutputPort<double> output("output");
        first = boost::dynamic_pointer_cast<internal::DataSource<double>>(output.getDataSource());
        second = boost::dynamic_pointer_cast<internal::DataSource<double>>(output.getDataSource());
        BOOST_REQUIRE(first);
        BOOST_REQUIRE(second);
        BOOST_CHECK(!first->evaluate());
        output.data() = 4;
        internal::PortDataAccess::commit(output);
        BOOST_CHECK(first->evaluate());
        BOOST_CHECK(!first->evaluate());
        BOOST_CHECK(second->evaluate());
        BOOST_CHECK_EQUAL(second->get(), 4);
        output.data() = 9;
        internal::PortDataAccess::commit(output);
        // An evaluated sample remains stable through decomposition/member gets.
        BOOST_CHECK_EQUAL(first->get(), 4);
        BOOST_CHECK_EQUAL(second->get(), 4);
        BOOST_CHECK(first->evaluate());
        BOOST_CHECK_EQUAL(first->value(), 9);
    }
    BOOST_CHECK(second->evaluate());
    BOOST_CHECK_EQUAL(second->value(), 9);
    BOOST_CHECK(!first->evaluate());
    BOOST_CHECK_EQUAL(first->value(), 9);
}

BOOST_AUTO_TEST_CASE(existing_snapshot_observer_detects_first_commit_after_reconfiguration)
{
    OutputPort<double> output("output");
    auto observer = boost::dynamic_pointer_cast<internal::DataSource<double>>(output.getDataSource());
    output.data() = 7;
    internal::PortDataAccess::commit(output);
    BOOST_REQUIRE(observer->evaluate());
    output.clear();
    BOOST_CHECK(!observer->evaluate());
    output.setDataSample(9);
    BOOST_CHECK(!observer->evaluate());
    internal::PortDataAccess::commit(output);
    BOOST_CHECK(observer->evaluate());
    BOOST_CHECK_EQUAL(observer->value(), 9);
}

BOOST_AUTO_TEST_CASE(configured_input_default_is_observable_without_new_publication)
{
    InputPort<double> input("input");
    input.setDataSample(23);
    internal::DataSource<double>::shared_ptr observer =
        dynamic_cast<internal::DataSource<double>*>(input.getDataSource());
    BOOST_CHECK(!observer->evaluate());
    BOOST_CHECK_EQUAL(observer->value(), 23);
    BOOST_CHECK_EQUAL(input.status(), NoData);
}

BOOST_AUTO_TEST_CASE(observers_remain_safe_while_stopped_storage_is_resized)
{
    OutputPort<std::vector<int>> output("output");
    auto observer = boost::dynamic_pointer_cast<internal::DataSource<std::vector<int>>>(output.getDataSource());
    std::atomic<bool> stop{false};
    std::atomic<bool> coherent{true};
    std::thread reader([&] {
        while (!stop.load()) {
            observer->evaluate();
            const auto& value = observer->rvalue();
            for (auto member : value)
                if (member != value.front()) coherent.store(false);
        }
    });
    for (int i = 1; i <= 1000; ++i) {
        output.setDataSample(std::vector<int>(i % 127 + 1, i));
        internal::PortDataAccess::commit(output);
    }
    stop.store(true);
    reader.join();
    BOOST_CHECK(coherent.load());
    observer->evaluate();
    BOOST_CHECK_EQUAL(observer->rvalue().front(), 1000);
}

namespace {
std::mutex observer_gate;
std::condition_variable observer_changed;
int observers_entered = 0;
bool observers_released = false;
thread_local bool slow_observer = false;
struct ObservedSample {
    int value = 0;
    ObservedSample() = default;
    ObservedSample(const ObservedSample& rhs) : value(rhs.value) {}
    ObservedSample& operator=(const ObservedSample& rhs) {
        if (slow_observer) {
            std::unique_lock<std::mutex> lock(observer_gate);
            ++observers_entered;
            observer_changed.notify_all();
            observer_changed.wait(lock, [] { return observers_released; });
        }
        value = rhs.value;
        return *this;
    }
};
}

BOOST_AUTO_TEST_CASE(slow_observers_cannot_suppress_component_delivery)
{
    types::Types()->addType(new types::TemplateTypeInfo<ObservedSample, false>("observed_sample"));
    OutputPort<ObservedSample> output("output");
    InputPort<ObservedSample> input("input");
    BOOST_REQUIRE(output.connectTo(&input));
    std::vector<std::thread> readers;
    ObservedSample received;
    for (int i = 1; i <= 3; ++i) {
        output.data().value = i;
        BOOST_CHECK_EQUAL(internal::PortDataAccess::commit(output), WriteSuccess);
        BOOST_CHECK_EQUAL(internal::PortDataAccess::receive(input, received), NewData);
        readers.emplace_back([&] {
            slow_observer = true;
            ObservedSample observed;
            output.snapshot(observed);
        });
        std::unique_lock<std::mutex> lock(observer_gate);
        BOOST_CHECK(observer_changed.wait_for(lock, std::chrono::seconds(2), [&] { return observers_entered == i; }));
    }
    output.data().value = 4;
    const auto committed = internal::PortDataAccess::commit(output);
    const auto incoming = internal::PortDataAccess::receive(input, received);
    {
        std::lock_guard<std::mutex> lock(observer_gate);
        observers_released = true;
        observer_changed.notify_all();
    }
    for (auto& reader : readers) reader.join();
    BOOST_CHECK_EQUAL(committed, WriteSuccess);
    BOOST_CHECK_EQUAL(incoming, NewData);
    BOOST_CHECK_EQUAL(received.value, 4);
}

BOOST_AUTO_TEST_CASE(shared_state_fanout_refreshes_each_component_and_observer)
{
    for (BufferPolicy placement : {PerConnection, PerInputPort, PerOutputPort, Shared}) {
        BOOST_TEST_CONTEXT("storage placement " << placement) {
            TaskContext producer("producer"), first("first"), second("second");
            producer.setActivity(new extras::SlaveActivity(0.01));
            first.setActivity(new extras::SlaveActivity(0.01));
            second.setActivity(new extras::SlaveActivity(0.01));
            OutputPort<double> output("output");
            InputPort<double> input_a("input"), input_b("input");
            producer.addPort(output); first.addPort(input_a); second.addPort(input_b);
            ConnPolicy policy = ConnPolicy::data(ConnPolicy::LOCKED, false);
            policy.buffer_policy = placement;
            BOOST_REQUIRE(output.connectTo(&input_a, policy));
            BOOST_REQUIRE(output.connectTo(&input_b, policy));
            internal::DataSource<double>::shared_ptr observer_a =
                dynamic_cast<internal::DataSource<double>*>(input_a.getDataSource());
            internal::DataSource<double>::shared_ptr observer_b =
                dynamic_cast<internal::DataSource<double>*>(input_b.getDataSource());
            BOOST_REQUIRE(producer.start());
            BOOST_REQUIRE(first.start());
            BOOST_REQUIRE(second.start());
            auto step = [](TaskContext& task) {
                return static_cast<extras::SlaveActivity*>(task.getActivity())->execute();
            };
            for (double sample : {17.0, 29.0, 29.0}) {
                output.data() = sample;
                BOOST_REQUIRE(step(producer));
                BOOST_REQUIRE(step(first));
                BOOST_REQUIRE(step(second));
                BOOST_CHECK_EQUAL(input_a.data(), sample);
                BOOST_CHECK_EQUAL(input_b.data(), sample);
                BOOST_CHECK_EQUAL(input_a.status(), NewData);
                BOOST_CHECK_EQUAL(input_b.status(), NewData);
                BOOST_CHECK(observer_a->evaluate());
                BOOST_CHECK(observer_b->evaluate());
                BOOST_CHECK_EQUAL(observer_a->value(), sample);
                BOOST_CHECK_EQUAL(observer_b->value(), sample);
                BOOST_REQUIRE(step(first));
                BOOST_REQUIRE(step(second));
                BOOST_CHECK_EQUAL(input_a.status(), OldData);
                BOOST_CHECK_EQUAL(input_b.status(), OldData);
                BOOST_CHECK(!observer_a->evaluate());
                BOOST_CHECK(!observer_b->evaluate());
            }
            second.stop(); first.stop(); producer.stop();
        }
    }
}

BOOST_AUTO_TEST_CASE(shared_input_join_rejects_a_different_sample_type)
{
    OutputPort<double> output("output");
    InputPort<double> staging("staging");
    InputPort<int> input("input");
    ConnPolicy policy = ConnPolicy::data();
    policy.buffer_policy = Shared;
    BOOST_REQUIRE(output.createConnection(staging, policy));
    BOOST_CHECK(!input.createConnection(output.getSharedConnection(), policy));
    BOOST_CHECK(!input.connected());
    BOOST_CHECK(input.getSourceConnections().empty());
    std::unique_ptr<internal::ConnID> id(output.getSharedConnection()->getConnID());
    const bool registered = input.addConnection(id.get(), output.getSharedConnection(), policy);
    if (registered) id.release();
    BOOST_CHECK(!registered);
    BOOST_CHECK(!input.getManager()->connected());
}

BOOST_AUTO_TEST_CASE(shared_readers_detect_republication_after_clear_and_reconnection)
{
    for (int locking : {ConnPolicy::LOCK_FREE, ConnPolicy::LOCKED, ConnPolicy::UNSYNC}) {
        for (BufferPolicy placement : {PerOutputPort, Shared}) {
            BOOST_TEST_CONTEXT("locking " << locking << ", storage " << placement) {
                ConnPolicy policy = ConnPolicy::data(locking, false);
                policy.buffer_policy = placement;
                OutputPort<double> output("output"), replacement("replacement");
                InputPort<double> first("first"), second("second");
                BOOST_REQUIRE(output.createConnection(first, policy));
                BOOST_REQUIRE(output.createConnection(second, policy));
                output.data() = 5;
                internal::PortDataAccess::commit(output);
                BOOST_CHECK_EQUAL(internal::PortDataAccess::refresh(second), NewData);
                BOOST_CHECK_EQUAL(internal::PortDataAccess::refresh(first), NewData);
                BOOST_CHECK_EQUAL(first.data(), 5);
                first.clear();
                BOOST_CHECK_EQUAL(internal::PortDataAccess::refresh(first), NoData);
                BOOST_CHECK_EQUAL(internal::PortDataAccess::refresh(second), NoData);
                internal::PortDataAccess::commit(output);
                BOOST_CHECK_EQUAL(internal::PortDataAccess::refresh(first), NewData);
                BOOST_CHECK_EQUAL(internal::PortDataAccess::refresh(second), NewData);
                BOOST_CHECK_EQUAL(second.data(), 5);
                BOOST_CHECK_EQUAL(internal::PortDataAccess::refresh(second), OldData);
                first.disconnect();
                second.disconnect();
                replacement.data() = 9;
                BOOST_REQUIRE(replacement.createConnection(first, policy));
                BOOST_REQUIRE(replacement.createConnection(second, policy));
                internal::PortDataAccess::commit(replacement);
                BOOST_CHECK_EQUAL(internal::PortDataAccess::refresh(second), NewData);
                BOOST_CHECK_EQUAL(internal::PortDataAccess::refresh(first), NewData);
                BOOST_CHECK_EQUAL(first.data(), 9);
                BOOST_CHECK_EQUAL(second.data(), 9);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(stalled_shared_reader_does_not_skip_a_concurrent_publication)
{
    if (!types::Types()->type("observed_sample"))
        types::Types()->addType(new types::TemplateTypeInfo<ObservedSample, false>("observed_sample"));
    for (BufferPolicy placement : {PerOutputPort, Shared}) {
        OutputPort<ObservedSample> output("output");
        InputPort<ObservedSample> first("first"), second("second");
        ConnPolicy policy = ConnPolicy::data(ConnPolicy::LOCK_FREE, false);
        policy.buffer_policy = placement;
        BOOST_REQUIRE(output.createConnection(first, policy));
        BOOST_REQUIRE(output.createConnection(second, policy));
        output.data().value = 5;
        BOOST_REQUIRE_EQUAL(internal::PortDataAccess::commit(output), WriteSuccess);
        {
            std::lock_guard<std::mutex> lock(observer_gate);
            observers_entered = 0;
            observers_released = false;
        }
        FlowStatus initial = NoData;
        std::thread reader([&] {
            slow_observer = true;
            initial = internal::PortDataAccess::refresh(first);
        });
        bool entered;
        {
            std::unique_lock<std::mutex> lock(observer_gate);
            entered = observer_changed.wait_for(lock, std::chrono::seconds(2), [] { return observers_entered != 0; });
        }
        WriteStatus written = WriteFailure;
        FlowStatus second_status = NoData;
        if (entered) {
            for (int value = 6; value <= 37; ++value) {
                output.data().value = value;
                written = internal::PortDataAccess::commit(output);
            }
            second_status = internal::PortDataAccess::refresh(second);
        }
        {
            std::lock_guard<std::mutex> lock(observer_gate);
            observers_released = true;
            observer_changed.notify_all();
        }
        reader.join();
        BOOST_REQUIRE(entered);
        BOOST_CHECK_EQUAL(initial, NewData);
        BOOST_CHECK_EQUAL(written, WriteSuccess);
        BOOST_CHECK_EQUAL(second_status, NewData);
        BOOST_CHECK_EQUAL(second.data().value, 37);
        BOOST_CHECK_EQUAL(first.data().value, 5);
        BOOST_CHECK_EQUAL(internal::PortDataAccess::refresh(first), NewData);
        BOOST_CHECK_EQUAL(first.data().value, 37);
        BOOST_CHECK_EQUAL(internal::PortDataAccess::refresh(first), OldData);
    }
}

BOOST_AUTO_TEST_CASE(reconnecting_a_shared_reader_releases_old_storage_before_the_cycle)
{
    struct TrackedStorage : internal::ChannelDataElement<double> {
        bool& destroyed;
        TrackedStorage(bool& destroyed, const ConnPolicy& policy)
            : internal::ChannelDataElement<double>(
                  base::DataObjectInterface<double>::shared_ptr(new base::DataObjectLocked<double>(0)), policy),
              destroyed(destroyed) {}
        ~TrackedStorage() { destroyed = true; }
    };
    bool destroyed = false;
    ConnPolicy policy = ConnPolicy::data(ConnPolicy::LOCKED, false);
    policy.buffer_policy = Shared;
    internal::SharedConnectionBase::shared_ptr shared(
        new internal::SharedConnection<double>(new TrackedStorage(destroyed, policy), policy));
    OutputPort<double> output("output"), replacement("replacement");
    InputPort<double> input("input");
    BOOST_REQUIRE(input.createConnection(shared, policy));
    BOOST_REQUIRE(output.createConnection(shared, policy));
    output.data() = 5;
    internal::PortDataAccess::commit(output);
    BOOST_REQUIRE_EQUAL(internal::PortDataAccess::refresh(input), NewData);
    input.disconnect();
    output.disconnect();
    shared.reset();
    BOOST_REQUIRE(replacement.createConnection(input, policy));
    BOOST_CHECK(destroyed);
}
