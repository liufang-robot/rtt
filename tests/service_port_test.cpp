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

    BOOST_CHECK(!tc.provides("portservice")->hasService("op"));
    BOOST_CHECK(!tc.provides("portservice")->hasService("ip"));
    ts->op.data() = 3;
    BOOST_CHECK_EQUAL(ts->op.snapshot(), 0);
    BOOST_REQUIRE_EQUAL(internal::PortDataAccess::commit(ts->op), WriteSuccess);
    BOOST_CHECK_EQUAL(ts->op.snapshot(), 3);
    BOOST_CHECK_EQUAL(ts->ip2.status(), NoData);
    BOOST_REQUIRE_EQUAL(internal::PortDataAccess::refresh(ts->ip2), NewData);
    BOOST_CHECK_EQUAL(ts->ip2.data(), 3);
    BOOST_CHECK_EQUAL(ts->ip2.status(), NewData);
}

BOOST_AUTO_TEST_CASE(testUsePortWithOwner)
{
    TaskContext tc("tc");
    TestService* ts = new TestService(&tc);
    Service::shared_ptr s( ts );

    tc.provides()->addService( s );

    ts->ip2.connectTo( &ts->op );

    BOOST_CHECK(!tc.provides("portservice")->hasService("op"));
    BOOST_CHECK(!tc.provides("portservice")->hasService("ip"));
    ts->op.data() = 3;
    BOOST_CHECK_EQUAL(ts->op.snapshot(), 0);
    BOOST_REQUIRE_EQUAL(internal::PortDataAccess::commit(ts->op), WriteSuccess);
    BOOST_CHECK_EQUAL(ts->op.snapshot(), 3);
    BOOST_CHECK_EQUAL(ts->ip2.status(), NoData);
    BOOST_REQUIRE_EQUAL(internal::PortDataAccess::refresh(ts->ip2), NewData);
    BOOST_CHECK_EQUAL(ts->ip2.data(), 3);
    BOOST_CHECK_EQUAL(ts->ip2.status(), NewData);
}

#endif

BOOST_AUTO_TEST_SUITE_END()
