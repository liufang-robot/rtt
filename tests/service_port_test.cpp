#include <rtt/internal/PortDataAccess.hpp>

#include <TaskContext.hpp>
#include <InputPort.hpp>
#include <OutputPort.hpp>
#include <Service.hpp>
#include <ServiceRequester.hpp>

#include "unit.hpp"
#include "operations_fixture.hpp"

struct ServicePortFixture {};

// Registers the suite into the 'registry'
BOOST_FIXTURE_TEST_SUITE(  ServicePortTestSuite,  ServicePortFixture )

/**
 * This test suite tests using ports in services
 */

class TestService : public Service {
public:
    InputPort<int> ip;
    InputPort<int> ip2;
    OutputPort<int> op;
    TestService(TaskContext* owner = 0) : Service("portservice", owner)
    {
        addPort("ip",ip).doc("ip");
        addPort("ip",ip2).doc("ip"); // overrides ip
        addPort("op",op).doc("op");
    }
};

class TestEventService : public Service {
public:
    InputPort<int> ip;
    InputPort<int> ip2;
    OutputPort<int> op;
    TestEventService(TaskContext* owner = 0) : Service("portservice", owner)
    {
        addEventPort("ip",ip).doc("ip");
        addEventPort("ip",ip2).doc("ip"); // overrides ip
        addPort("op",op).doc("op");
    }
};

BOOST_AUTO_TEST_CASE(testAddPort)
{
    TestService* ts = new TestService();
    Service::shared_ptr s( ts );
    TaskContext tc("tc");

    tc.provides()->addService( s );

    // check that last port is the real thing:
    BOOST_CHECK( tc.provides("portservice")->getPort("ip") == &ts->ip2 );

    BOOST_CHECK( tc.provides("portservice")->getPort("op") == &ts->op );
}

BOOST_AUTO_TEST_CASE(testAddPortWithOwner)
{
    TaskContext tc("tc");
    TestService* ts = new TestService( &tc );
    Service::shared_ptr s( ts );

    tc.provides()->addService( s );

    // check that last port is the real thing:
    BOOST_CHECK( tc.provides("portservice")->getPort("ip") == &ts->ip2 );

    BOOST_CHECK( tc.provides("portservice")->getPort("op") == &ts->op );
}


BOOST_AUTO_TEST_CASE(testAddEventPort)
{
    TestEventService* ts = new TestEventService();
    Service::shared_ptr s( ts );
    TaskContext tc("tc");

    tc.provides()->addService( s );

    // check that last port is the real thing:
    BOOST_CHECK( tc.provides("portservice")->getPort("ip") == &ts->ip2 );

    BOOST_CHECK( tc.provides("portservice")->getPort("op") == &ts->op );
}

BOOST_AUTO_TEST_CASE(testAddEventPortWithOwner)
{
    TaskContext tc("tc");
    TestEventService* ts = new TestEventService(&tc);
    Service::shared_ptr s( ts );

    tc.provides()->addService( s );

    // check that last port is the real thing:
    BOOST_CHECK( tc.provides("portservice")->getPort("ip") == &ts->ip2 );

    BOOST_CHECK( tc.provides("portservice")->getPort("op") == &ts->op );
}


#ifndef ORO_DISABLE_PORT_DATA_SCRIPTING

BOOST_AUTO_TEST_CASE(testUsePort)
{
    TestService* ts = new TestService();
    // should work because TestService already adds itself to children... but this memleaks
    Service::shared_ptr s1 = ts->provides();
    // should work as well because TestService inherits from enable_shared_from_raw :
    Service::shared_ptr s( ts );
    TaskContext tc("tc");

    tc.provides()->addService( s );

    ts->ip2.connectTo( &ts->op );

    BOOST_REQUIRE(tc.provides("portservice")->provides("op")->hasOperation("snapshot"));
    BOOST_CHECK(!tc.provides("portservice")->provides("op")->hasOperation("write"));
    BOOST_CHECK(!tc.provides("portservice")->provides("ip")->hasOperation("read"));
    OperationCaller<int()> snapshot = tc.provides("portservice")->provides("op")->getOperation("snapshot");
    OperationCaller<FlowStatus()> status = tc.provides("portservice")->provides("ip")->getOperation("status");
    BOOST_REQUIRE(snapshot.ready());
    BOOST_REQUIRE(status.ready());
    ts->op.data() = 3;
    BOOST_CHECK_EQUAL(snapshot(), 0);
    BOOST_REQUIRE_EQUAL(internal::PortDataAccess::commit(ts->op), WriteSuccess);
    BOOST_CHECK_EQUAL(snapshot(), 3);
    BOOST_CHECK_EQUAL(status(), NoData);
    BOOST_REQUIRE_EQUAL(internal::PortDataAccess::refresh(ts->ip2), NewData);
    BOOST_CHECK_EQUAL(ts->ip2.data(), 3);
    BOOST_CHECK_EQUAL(status(), NewData);
}

BOOST_AUTO_TEST_CASE(testUsePortWithOwner)
{
    TaskContext tc("tc");
    TestService* ts = new TestService(&tc);
    Service::shared_ptr s( ts );

    tc.provides()->addService( s );

    ts->ip2.connectTo( &ts->op );

    BOOST_REQUIRE(tc.provides("portservice")->provides("op")->hasOperation("snapshot"));
    BOOST_CHECK(!tc.provides("portservice")->provides("op")->hasOperation("write"));
    BOOST_CHECK(!tc.provides("portservice")->provides("ip")->hasOperation("read"));
    OperationCaller<int()> snapshot = tc.provides("portservice")->provides("op")->getOperation("snapshot");
    OperationCaller<FlowStatus()> status = tc.provides("portservice")->provides("ip")->getOperation("status");
    BOOST_REQUIRE(snapshot.ready());
    BOOST_REQUIRE(status.ready());
    ts->op.data() = 3;
    BOOST_CHECK_EQUAL(snapshot(), 0);
    BOOST_REQUIRE_EQUAL(internal::PortDataAccess::commit(ts->op), WriteSuccess);
    BOOST_CHECK_EQUAL(snapshot(), 3);
    BOOST_CHECK_EQUAL(status(), NoData);
    BOOST_REQUIRE_EQUAL(internal::PortDataAccess::refresh(ts->ip2), NewData);
    BOOST_CHECK_EQUAL(ts->ip2.data(), 3);
    BOOST_CHECK_EQUAL(status(), NewData);
}

#endif

BOOST_AUTO_TEST_SUITE_END()
