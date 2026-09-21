#include <boost/test/unit_test.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/array.hpp>
#include <rtt/InputPort.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/PortEndpoint.hpp>
#include <rtt/extras/SlaveActivity.hpp>
#include <rtt/internal/PortDataAccess.hpp>
#include <rtt/scripting/Parser.hpp>
#include <rtt/scripting/parse_exception.hpp>
#include <rtt/types/StructTypeInfo.hpp>
#include <rtt/types/CArrayTypeInfo.hpp>
#include <rtt/types/TypeInfoRepository.hpp>
#include <memory>

using namespace RTT;
namespace {
struct Axis {
    double position = 0;
    template<class Archive> void serialize(Archive& a, unsigned) {
        a & boost::serialization::make_nvp("position", position);
    }
};
struct Frame {
    double x = 0, y = 0;
    int name = 11, connected = 12, status = 13, snapshot = 14, data = 15;
    Axis axes[3]{};
    template<class Archive> void serialize(Archive& a, unsigned) {
        a & boost::serialization::make_nvp("x", x);
        a & boost::serialization::make_nvp("y", y);
        a & boost::serialization::make_nvp("name", name);
        a & boost::serialization::make_nvp("connected", connected);
        a & boost::serialization::make_nvp("status", status);
        a & boost::serialization::make_nvp("snapshot", snapshot);
        a & boost::serialization::make_nvp("data", data);
        a & boost::serialization::make_nvp("axes", boost::serialization::make_array(axes, 3));
    }
};
class RemoteFrame : public internal::ValueDataSource<Frame> {
    std::shared_ptr<bool> readable;
public:
    RemoteFrame(const Frame& value, std::shared_ptr<bool> state)
        : internal::ValueDataSource<Frame>(value), readable(std::move(state)) {}
    bool evaluate() const override { return *readable; }
    RemoteFrame* clone() const override { return new RemoteFrame(value(), readable); }
};
class CountedInput : public InputPort<double> {
    unsigned& destroyed;
public:
    explicit CountedInput(unsigned& count) : destroyed(count) {}
    ~CountedInput() override { ++destroyed; }
};
class InvalidAntiCloneInput : public InputPort<double> {
    unsigned& destroyed;
public:
    explicit InvalidAntiCloneInput(unsigned& count) : destroyed(count) {}
    base::PortInterface* antiClone() const override { return new CountedInput(destroyed); }
};
struct OpaqueValue { double value = 0; };
class OpaqueTypeInfo : public types::PrimitiveTypeInfo<OpaqueValue> {
public:
    OpaqueTypeInfo() : types::PrimitiveTypeInfo<OpaqueValue>("EndpointOpaque") {}
    base::DataSourceBase::shared_ptr buildReadOnlyExpression(
        boost::shared_ptr<internal::ObservationPath>, bool) const override { return {}; }
};
struct Fixture {
    TaskContext source{"source"}, sink{"sink"}, browser{"browser"};
    OutputPort<Frame> output{"output"};
    OutputPort<double> scalar{"scalar"};
    InputPort<Frame> input{"input"};
    scripting::Parser parser;
    unsigned mutations = 0;
    unsigned calls = 0;
    Fixture() {
        auto types = types::TypeInfoRepository::Instance();
        if (!types->type("EndpointFrame")) {
            types->addType(new types::StructTypeInfo<Axis>("EndpointAxis"));
            types->addType(new types::CArrayTypeInfo<types::carray<Axis>>("EndpointAxes"));
            types->addType(new types::StructTypeInfo<Frame>("EndpointFrame"));
        }
        source.setActivity(new extras::SlaveActivity());
        sink.setActivity(new extras::SlaveActivity());
        source.addPort(output); source.addPort(scalar);
        sink.provides("io")->addPort(input);
        browser.addPeer(&source); browser.addPeer(&sink);
        browser.addOperation("twice", &Fixture::twice, this, ClientThread);
        browser.addOperation("sum", &Fixture::sum, this, ClientThread);
        browser.addOperation("mutate", &Fixture::mutate, this, ClientThread);
        browser.addOperation("mutateFrame", &Fixture::mutateFrame, this, ClientThread);
        browser.addOperation("mutateAxes", &Fixture::mutateAxes, this, ClientThread);
    }
    double twice(double value) { ++calls; return value * 2; }
    double sum(const Frame& first, const Frame& second) { return first.y + second.y; }
    void mutate(double& value) { ++mutations; value = 999; }
    void mutateFrame(Frame& value) { ++mutations; value.y = 999; }
    void mutateAxes(types::carray<Axis>& value) { ++mutations; value.address()[0].position = 999; }
    template<class T> typename internal::DataSource<T>::shared_ptr read(const std::string& expression) {
        base::DataSourceBase::shared_ptr parsed;
        try { parsed = parser.parseExpression(expression, &browser); }
        catch (const parse_exception& error) { BOOST_FAIL(expression + ": " + error.what()); }
        auto result = boost::dynamic_pointer_cast<internal::DataSource<T>>(parsed);
        BOOST_REQUIRE(result);
        BOOST_CHECK(!result->isAssignable());
        return result;
    }
    void publish(double value) {
        output.data().x = value + 1; output.data().y = value;
        output.data().axes[2].position = value + 2;
        scalar.data() = value;
        internal::PortDataAccess::commit(output); internal::PortDataAccess::commit(scalar);
    }
    ~Fixture() { source.stop(); sink.stop(); }
};
}
BOOST_FIXTURE_TEST_SUITE(PortEndpointSuite, Fixture)
BOOST_AUTO_TEST_CASE(registration_preserves_real_services_without_generating_port_services) {
    BOOST_CHECK(!source.provides()->hasService("output"));
    BOOST_CHECK(!sink.provides("io")->hasService("input"));
    auto real = source.provides("named");
    Frame other; other.y = 99;
    real->addAttribute("y", other.y);
    OutputPort<Frame> named("named");
    source.addPort(named);
    internal::PortDataAccess::commit(named);
    BOOST_CHECK(source.provides()->getService("named") == real);
#ifndef ORO_DISABLE_PORT_DATA_SCRIPTING
    BOOST_CHECK_EQUAL(read<double>("source.named.y")->get(), 0);
#endif
    source.ports()->removePort("named");
    BOOST_CHECK(source.provides()->getService("named") == real);
}
#ifndef ORO_DISABLE_PORT_DATA_SCRIPTING
BOOST_AUTO_TEST_CASE(registered_port_precedence_matches_connection_endpoints) {
    publish(0);
    double shadow = 99;
    source.addAttribute("scalar", shadow);
    auto* bag = new Property<PropertyBag>("output", "");
    bag->set().ownProperty(new Property<double>("y", "", 88));
    source.properties()->ownProperty(bag);
    BOOST_CHECK_EQUAL(read<double>("source.scalar")->get(), 0);
    BOOST_CHECK_EQUAL(read<double>("source.output.y")->get(), 0);
}
BOOST_AUTO_TEST_CASE(unsupported_port_observation_does_not_fall_back_to_mutable_attributes) {
    if (!types::TypeInfoRepository::Instance()->type("EndpointOpaque"))
        types::TypeInfoRepository::Instance()->addType(new OpaqueTypeInfo);
    OutputPort<OpaqueValue> opaque("opaque"); source.addPort(opaque);
    double shadow = 173; source.addAttribute("opaque", shadow);
    BOOST_CHECK_THROW(parser.parseExpression("source.opaque", &browser), parse_exception);
    BOOST_CHECK_THROW(parser.parseValueChange("source.opaque = 181.0", &browser), parse_exception);
    BOOST_CHECK_EQUAL(shadow, 173);
}
BOOST_AUTO_TEST_CASE(direct_values_refresh_members_arrays_arithmetic_and_arguments) {
    publish(4);
    auto whole = read<Frame>("source.output");
    auto member = read<double>("source.output.y");
    auto indexed = read<double>("source.output.axes[2].position");
    auto scalar_value = read<double>("source.scalar");
    auto arithmetic = read<double>("source.output.y + 3.0");
    auto argument = read<double>("twice(source.output.y)");
    auto refs = read<double>("sum(source.output, source.output)");
    BOOST_CHECK_EQUAL(whole->get().y, 4); BOOST_CHECK_EQUAL(member->get(), 4);
    BOOST_CHECK_EQUAL(indexed->get(), 6); BOOST_CHECK_EQUAL(scalar_value->get(), 4);
    publish(9);
    BOOST_CHECK_EQUAL(whole->get().y, 9); BOOST_CHECK_EQUAL(member->get(), 9);
    BOOST_CHECK_EQUAL(member->rvalue(), 9); BOOST_CHECK_EQUAL(indexed->get(), 11);
    BOOST_CHECK_EQUAL(scalar_value->get(), 9); BOOST_CHECK_EQUAL(arithmetic->get(), 12);
    BOOST_CHECK_EQUAL(argument->get(), 18); BOOST_CHECK_EQUAL(refs->get(), 18);
    output.data().y = 80;
    BOOST_CHECK_EQUAL(member->get(), 9);
}
BOOST_AUTO_TEST_CASE(input_observation_changes_only_on_acquisition_and_retains_while_stopped) {
    BOOST_REQUIRE(output.connectTo(&input));
    auto observed = read<double>("sink.io.input.y");
    BOOST_CHECK_EQUAL(observed->get(), 0);
    publish(7);
    BOOST_CHECK_EQUAL(observed->get(), 0);
    BOOST_REQUIRE(sink.start()); BOOST_REQUIRE(sink.getActivity()->execute());
    BOOST_CHECK_EQUAL(observed->get(), 7);
    BOOST_REQUIRE(sink.stop()); publish(8);
    BOOST_CHECK_EQUAL(observed->get(), 7);
    BOOST_CHECK_EQUAL(input.status(), NewData);
}
BOOST_AUTO_TEST_CASE(old_management_names_are_plain_data_fields) {
    publish(0);
    BOOST_CHECK_EQUAL(read<int>("source.output.name")->get(), 11);
    BOOST_CHECK_EQUAL(read<int>("source.output.connected")->get(), 12);
    BOOST_CHECK_EQUAL(read<int>("source.output.status")->get(), 13);
    BOOST_CHECK_EQUAL(read<int>("source.output.snapshot")->get(), 14);
    BOOST_CHECK_EQUAL(read<int>("source.output.data")->get(), 15);
    BOOST_CHECK_THROW(parser.parseExpression("source.output.connected()", &browser), parse_exception);
}
BOOST_AUTO_TEST_CASE(observations_reject_all_assignment_and_mutable_reference_paths) {
    publish(5);
    for (auto text : {"source.output = source.output", "source.output.y = 9.0", "source.output.axes[2].position = 9.0"})
        BOOST_CHECK_THROW(parser.parseValueChange(text, &browser), parse_exception);
    for (auto text : {"mutate(source.output.y)", "mutateFrame(source.output)", "mutateAxes(source.output.axes)"})
        BOOST_CHECK_THROW(parser.parseExpression(text, &browser), parse_exception);
    BOOST_CHECK_EQUAL(mutations, 0u);
    BOOST_CHECK_EQUAL(read<double>("source.output.y")->get(), 5);
}
BOOST_AUTO_TEST_CASE(selected_observations_survive_port_destruction) {
    internal::DataSource<double>::shared_ptr result;
    {
        auto temporary = std::make_unique<OutputPort<Frame>>("temporary");
        source.addPort(*temporary);
        temporary->data().axes[2].position = 17;
        internal::PortDataAccess::commit(*temporary);
        result = read<double>("source.temporary.axes[2].position");
    }
    BOOST_CHECK_EQUAL(result->get(), 17);
}
BOOST_AUTO_TEST_CASE(unavailable_expressions_do_not_call_operations_and_recover_after_commit) {
    auto value = read<double>("source.output.y");
    auto arithmetic = read<double>("source.output.y + 3.0");
    auto argument = read<double>("twice(source.output.y)");
    BOOST_CHECK(!value->evaluate());
    BOOST_CHECK_THROW(value->get(), std::runtime_error);
    BOOST_CHECK_THROW(value->rvalue(), std::runtime_error);
    BOOST_CHECK_THROW(arithmetic->get(), std::runtime_error);
    BOOST_CHECK_THROW(argument->get(), std::runtime_error);
    BOOST_CHECK_EQUAL(calls, 0u);
    publish(6);
    BOOST_CHECK(value->evaluate());
    BOOST_CHECK_EQUAL(value->get(), 6);
    BOOST_CHECK_EQUAL(argument->get(), 12);
    BOOST_CHECK_EQUAL(calls, 1u);
}

#else
BOOST_AUTO_TEST_CASE(disabled_port_scripting_rejects_port_values) {
    BOOST_CHECK_THROW(parser.parseExpression("source.output", &browser), parse_exception);
}
#endif
BOOST_AUTO_TEST_CASE(endpoint_resolution_uses_fixed_dot_index_syntax) {
    PortEndpoint endpoint;
    BOOST_REQUIRE(resolvePortEndpoint(*source.provides(), "output.axes[02].position", endpoint));
    BOOST_CHECK(endpoint.port == &output);
    BOOST_CHECK_EQUAL(endpoint.member, "axes[2].position");
    BOOST_CHECK(endpoint.getTypeInfo() == internal::DataSource<double>::GetTypeInfo());
    BOOST_REQUIRE(resolvePortEndpoint(*sink.provides(), "io.input.y", endpoint));
    BOOST_CHECK(endpoint.port == &input);
    for (auto path : {"", "output::y", "output.", "output..y", "output.missing", "output.axes[-1]", "output.axes[3]", "output.axes[999999999999999999999]", "output.axes[1+1]", "output.axes[2].missing", "output.axes[]", "output.axes.[0]"})
        BOOST_CHECK_MESSAGE(!resolvePortEndpoint(*source.provides(), path, endpoint), path);
    BOOST_CHECK(!output.connected());
}
BOOST_AUTO_TEST_CASE(passive_observation_and_frozen_samples_do_not_change_topology) {
    BOOST_REQUIRE(output.connectTo(&input));
    BOOST_REQUIRE(source.start()); BOOST_REQUIRE(sink.start());
    auto out = PortObservation::create({&output, ""});
    auto in = PortObservation::create({&input, "y"});
    BOOST_REQUIRE(out); BOOST_REQUIRE(in);
    BOOST_CHECK(!out->available()); BOOST_CHECK(in->available());
    BOOST_CHECK(!out->dataSource()->evaluate());
    BOOST_CHECK(!out->snapshot()->evaluate());
    BOOST_CHECK(!out->dataSource()->getMember("y")->evaluate());
    publish(10);
    BOOST_CHECK(out->available());
    BOOST_CHECK(out->dataSource()->evaluate());
    BOOST_CHECK(out->dataSource()->getMember("y")->evaluate());
    auto frozen = boost::dynamic_pointer_cast<internal::DataSource<Frame>>(out->snapshot());
    BOOST_REQUIRE(frozen);
    BOOST_REQUIRE(sink.getActivity()->execute());
    BOOST_CHECK_EQUAL(boost::dynamic_pointer_cast<internal::DataSource<double>>(in->snapshot())->get(), 10);
    publish(20);
    BOOST_CHECK_EQUAL(frozen->get().y, 10);
    internal::DataSource<Frame>::shared_ptr clone = frozen->clone();
    BOOST_CHECK_EQUAL(clone->get().y, 10);
    BOOST_CHECK_EQUAL(boost::dynamic_pointer_cast<internal::DataSource<Frame>>(out->snapshot())->get().y, 20);
    BOOST_CHECK_EQUAL(input.getSourceConnections().size(), 1u);
    BOOST_CHECK_EQUAL(input.data().y, 10);
}
BOOST_AUTO_TEST_CASE(array_observation_owns_immutable_samples_and_stable_references) {
    publish(3);
    auto observation = PortObservation::create({&output, "axes"});
    BOOST_REQUIRE(observation);
    auto value = boost::dynamic_pointer_cast<internal::DataSource<types::carray<Axis>>>(observation->dataSource());
    BOOST_REQUIRE(value);
    auto frozen = boost::dynamic_pointer_cast<internal::DataSource<types::carray<Axis>>>(observation->snapshot());
    BOOST_REQUIRE(frozen);
    const auto& reference = value->rvalue();
    auto escaped = value->get(); escaped.address()[2].position = 999;
    BOOST_CHECK_EQUAL(value->get().address()[2].position, 5);
    publish(6);
    BOOST_CHECK_EQUAL(value->get().address()[2].position, 8);
    BOOST_CHECK_EQUAL(reference.address()[2].position, 8);
    BOOST_CHECK_EQUAL(frozen->get().address()[2].position, 5);
    auto frozen_view = frozen->get(); frozen_view.address()[2].position = 777;
    BOOST_CHECK_EQUAL(frozen->get().address()[2].position, 5);
}
BOOST_AUTO_TEST_CASE(external_member_sources_preserve_disjoint_writers_and_cycle_boundary) {
    BOOST_REQUIRE(connectMembers(output, "x", input, "x"));
    std::string error;
    auto external = PortInputSource::create({&input, "y"}, &error, "network");
    BOOST_REQUIRE_MESSAGE(external, error);
    BOOST_CHECK(!PortInputSource::create({&input, "y"}));
    BOOST_CHECK(!PortInputSource::create({&input, ""}));
    BOOST_CHECK(external->getTypeInfo() == internal::DataSource<double>::GetTypeInfo());
    BOOST_REQUIRE(external->stage(new internal::ValueDataSource<double>(73)));
    BOOST_CHECK(!external->stage(new internal::ValueDataSource<int>(99)));
    publish(4);
    BOOST_CHECK_EQUAL(input.data().y, 0);
    BOOST_REQUIRE(sink.start()); BOOST_REQUIRE(sink.getActivity()->execute());
    BOOST_CHECK_EQUAL(input.data().x, 5); BOOST_CHECK_EQUAL(input.data().y, 73);
    BOOST_CHECK(!external->disconnect());
    BOOST_REQUIRE(external->stage(new internal::ValueDataSource<double>(74)));
    BOOST_CHECK_EQUAL(input.data().y, 73);
    BOOST_REQUIRE(sink.getActivity()->execute());
    BOOST_CHECK_EQUAL(input.data().y, 74);
    BOOST_REQUIRE(sink.stop()); BOOST_REQUIRE(external->disconnect());
    BOOST_CHECK(!external->connected());
    BOOST_CHECK(!external->stage(new internal::ValueDataSource<double>(75)));
    BOOST_CHECK_EQUAL(input.getSourceConnections().size(), 1u);
    BOOST_CHECK_EQUAL(input.data().y, 74);
}
BOOST_AUTO_TEST_CASE(external_sources_reject_wrong_direction_overlap_shape_and_active_setup) {
    BOOST_CHECK(!PortInputSource::create({&output, "y"}));
    auto array = PortInputSource::create({&input, "axes"}); BOOST_REQUIRE(array);
    BOOST_CHECK(!PortInputSource::create({&input, "axes[2].position"}));
    Axis wrong[2]{};
    BOOST_CHECK(!array->stage(new internal::ValueDataSource<types::carray<Axis>>(types::carray<Axis>(wrong, 2))));
    Axis valid[3]{}; valid[1].position = 32;
    BOOST_REQUIRE(array->stage(new internal::ValueDataSource<types::carray<Axis>>(types::carray<Axis>(valid, 3))));
    BOOST_REQUIRE(sink.start());
    BOOST_CHECK(!PortInputSource::create({&input, "y"}));
    BOOST_REQUIRE(sink.getActivity()->execute());
    BOOST_CHECK_EQUAL(input.data().axes[1].position, 32);
    BOOST_REQUIRE(sink.stop()); BOOST_REQUIRE(array->disconnect());
    BOOST_REQUIRE(output.connectTo(&input));
    BOOST_CHECK(!PortInputSource::create({&input, "y"}));
    BOOST_CHECK(!PortInputSource::create({&input, ""}));
}
BOOST_AUTO_TEST_CASE(whole_external_source_stages_and_safely_releases_running_owner) {
    auto external = PortInputSource::create({&input, ""}); BOOST_REQUIRE(external);
    Frame sample; sample.y = 26; sample.axes[0].position = 27;
    BOOST_REQUIRE(external->stage(new internal::ValueDataSource<Frame>(sample)));
    BOOST_CHECK_EQUAL(input.data().y, 0);
    BOOST_REQUIRE(sink.start()); BOOST_REQUIRE(sink.getActivity()->execute());
    BOOST_CHECK_EQUAL(input.data().y, 26);
    BOOST_CHECK_EQUAL(input.data().axes[0].position, 27);
    external.reset();
    BOOST_CHECK(!sink.isRunning());
    BOOST_CHECK(!input.connected());
    BOOST_CHECK_EQUAL(input.data().y, 26);
}
BOOST_AUTO_TEST_CASE(external_source_survives_destination_destruction_without_dangling_images) {
    std::shared_ptr<PortInputSource> external;
    {
        InputPort<Frame> temporary("temporary");
        sink.addPort(temporary);
        external = PortInputSource::create({&temporary, "y"}); BOOST_REQUIRE(external);
        BOOST_REQUIRE(external->stage(new internal::ValueDataSource<double>(42)));
    }
    BOOST_CHECK(!external->connected());
    BOOST_CHECK(!external->stage(new internal::ValueDataSource<double>(43)));
    BOOST_CHECK(external->disconnect());
}
BOOST_AUTO_TEST_CASE(invalid_anti_clone_is_destroyed_when_source_setup_fails) {
    unsigned destroyed = 0;
    InvalidAntiCloneInput invalid(destroyed);
    BOOST_CHECK(!PortInputSource::create({&invalid, ""}));
    BOOST_CHECK_EQUAL(destroyed, 1u);
}
BOOST_AUTO_TEST_CASE(transport_observation_override_is_independent_from_mirror_images) {
    internal::AssignableDataSource<double>::shared_ptr remote = new internal::ValueDataSource<double>(17);
    internal::PortDataAccess::setObservationSource(scalar, remote);
    auto observation = PortObservation::create({&scalar, ""}); BOOST_REQUIRE(observation);
    BOOST_CHECK(observation->available());
    BOOST_CHECK_EQUAL(boost::dynamic_pointer_cast<internal::DataSource<double>>(observation->dataSource())->get(), 17);
    BOOST_CHECK_EQUAL(scalar.data(), 0);
    BOOST_CHECK(!scalar.connected());
}
BOOST_AUTO_TEST_CASE(unavailable_remote_observations_keep_metadata_but_fail_evaluation) {
    auto readable = std::make_shared<bool>(true);
    Frame remote; remote.y = 21;
    internal::PortDataAccess::setObservationSource(output, new RemoteFrame(remote, readable));
    auto observation = PortObservation::create({&output, "y"}); BOOST_REQUIRE(observation);
    auto value = boost::dynamic_pointer_cast<internal::DataSource<double>>(observation->dataSource());
    BOOST_REQUIRE(value); BOOST_CHECK(value->evaluate()); BOOST_CHECK_EQUAL(value->get(), 21);
    *readable = false;
    BOOST_CHECK(!observation->available());
    BOOST_CHECK(!value->evaluate());
    BOOST_CHECK_THROW(value->get(), std::runtime_error);
    BOOST_CHECK_THROW(value->rvalue(), std::runtime_error);
    auto unavailable = observation->snapshot(); BOOST_REQUIRE(unavailable);
    BOOST_CHECK(!unavailable->evaluate());
    auto whole = PortObservation::create({&output, ""}); BOOST_REQUIRE(whole);
    BOOST_REQUIRE(whole->dataSource()->getMember("axes"));
    BOOST_CHECK(!whole->dataSource()->getMember("axes")->evaluate());
    *readable = true;
    BOOST_CHECK(value->evaluate());
    BOOST_CHECK(!unavailable->evaluate());
}
BOOST_AUTO_TEST_SUITE_END()
