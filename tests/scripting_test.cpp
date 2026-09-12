/***************************************************************************
  tag: The SourceWorks  Tue Sep 7 00:54:57 CEST 2010  scripting_test.cpp

                        scripting_test.cpp -  description
                           -------------------
    begin                : Tue September 07 2010
    copyright            : (C) 2010 The SourceWorks
    email                : peter@thesourceworks.com

 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "unit.hpp"

#include "operations_fixture.hpp"
#include "datasource_fixture.hpp"
#include <scripting/Scripting.hpp>
#include <scripting/ScriptingService.hpp>
#include <extras/SequentialActivity.hpp>
#include <plugin/PluginLoader.hpp>
#include <scripting/Parser.hpp>
#include <scripting/CommonParser.hpp>
#include <scripting/ExpressionParser.hpp>
#include <scripting/CallFunction.hpp>
#include <scripting/CommandNOP.hpp>
#include <scripting/FunctionGraph.hpp>
#include <internal/GlobalService.hpp>
#include <types/StructTypeInfo.hpp>
#include <types/CArrayTypeInfo.hpp>


using namespace std;
using namespace boost;
using namespace RTT;
using namespace RTT::detail;

#include <boost/shared_ptr.hpp>

// note: Does not preserve newlines. Add them explicitly with \n or add semicolons after each line.
#define MULTILINE_STRING(...) #__VA_ARGS__

class ReadOnlyCArrayOperationProvider
{
public:
    BType getBatch() const { return BType(true); }
};

// Registers the fixture into the 'registry'
BOOST_FIXTURE_TEST_SUITE(  ScriptingTestSuite,  OperationsFixture )

BOOST_AUTO_TEST_CASE(TestScriptingStatusOperationsUseCanonicalTypes)
{
    PluginLoader::Instance()->loadService("scripting",tc);
    Service::shared_ptr service = tc->provides("scripting");
    BOOST_REQUIRE(service);

    OperationInterfacePart* program_status =
        service->getOperation("getProgramStatus");
    OperationInterfacePart* state_machine_status =
        service->getOperation("getStateMachineStatus");
    BOOST_REQUIRE(program_status);
    BOOST_REQUIRE(state_machine_status);
    BOOST_REQUIRE(program_status->getArgumentType(0));
    BOOST_REQUIRE(state_machine_status->getArgumentType(0));
    BOOST_CHECK_EQUAL(program_status->getArgumentType(0)->getTypeName(),
                      "Int32");
    BOOST_CHECK_EQUAL(state_machine_status->getArgumentType(0)->getTypeName(),
                      "Int32");
}

//! Tests the scripting service's functions
BOOST_AUTO_TEST_CASE(TestGetProvider)
{
    //ScriptingService* sa = new ScriptingService( tc ); // done by TC or plugin.

    PluginLoader::Instance()->loadService("scripting",tc);

    boost::shared_ptr<Scripting> sc = tc->getProvider<Scripting>("scripting");
    BOOST_REQUIRE( sc );
    BOOST_CHECK ( sc->ready() );
    BOOST_REQUIRE( sc->loadProgramText( MULTILINE_STRING(
        program Foo {
            do test.assert(true);
            set ret = 10.0;
        })));
    BOOST_CHECK( sc->hasProgram("Foo") );
    BOOST_REQUIRE( sc->startProgram("Foo") );
    BOOST_CHECK( sc->isProgramRunning("Foo") );

    // executes our script in the EE:
    while(sc->isProgramRunning("Foo")) {
        tc->trigger();
        usleep(100);
    }

    // test results:
    BOOST_CHECK( sc->isProgramRunning("Foo") == false );
    BOOST_CHECK( sc->inProgramError("Foo") == false );
    BOOST_CHECK( ret == 10.0 );

}

BOOST_AUTO_TEST_CASE(TestScriptingParser)
{
    PluginLoader::Instance()->loadService("scripting",tc);

    boost::shared_ptr<Scripting> sc = tc->getProvider<Scripting>("scripting");
    BOOST_REQUIRE( sc );
    BOOST_CHECK ( sc->ready() );

    // test plain statements:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        ;;test.increase()\n
        ;;;\n
        test.increase())));  // trailing newline is optional
    BOOST_CHECK_EQUAL( i, 1);

    // test variable decls:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        var Int32 i = 0;
        var Int32 y, z = 10;
        test.i = z;
        )));
    BOOST_CHECK_EQUAL( i, 10);

    // test if statement:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        var Int32 x = 1, y = 2;
        if 3 == 8 then
            test.i = x;
        else
            test.i = y;
        )));
    BOOST_CHECK_EQUAL( i, 2);

    // test while statement:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        var Int32 x = 1, y = 2;
        while x != y {
            test.i = 3;
            x = y;
        })));
    BOOST_CHECK_EQUAL( i, 3);

    // test while name clash:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        var Int32 whilex, whiley\n
        whilex = 1;
        whiley = 2\n
        while whilex != whiley {
            test.i = 3;
            whilex = whiley;
        })));
    BOOST_CHECK_EQUAL( i, 3);

    // test for statement:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        var Int32 x = 10, y = 20;
        for( x = 0; x != y; x = x + 1) {
            test.i = x;
        })));
    BOOST_CHECK_EQUAL( i, 19);

    // test for name clash:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        var Int32 forx, fory\n
        forx = 10; fory = 20;
        for( forx = 0; forx != fory; forx = forx + 1) {
            test.i = forx;
        })));
    BOOST_CHECK_EQUAL( i, 19);

    // test function +  a statement that uses that function:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        export function adder(Int32 a, Int32 b) {
            test.i = a + b;
        }\n
        adder(5,6)\n
        )));
    BOOST_CHECK_EQUAL( i, 11);
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        export Void adder2(Int32 a, Int32 b) {
            test.i = a + b;
        }\n
        adder2(7,8)\n
        )));
    BOOST_CHECK_EQUAL( i, 15);
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        export Int32 adder3(Int32 a, Int32 b) {
            return a + b;
        }\n
        test.i = adder3(6,10)\n
        )));
    BOOST_CHECK_EQUAL( i, 16);

    // test program +  a statement that starts that program and waits for the result.
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        program rt_script {
            test.i = 3-9;
        }\n
        rt_script.start();;;;
        while( rt_script.isRunning() ) {
            trigger();
            yield;
        })));
    BOOST_CHECK_EQUAL( sc->getProgramStatus("rt_script"), ProgramInterface::Status::stopped );
    BOOST_CHECK_EQUAL( i, -6);

    // test state machine +  a statement that starts that SM and waits for the result.
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        StateMachine RTState {
            initial state init {
                entry {
                    test.i = 0;
                }
                transitions {
                    select fini;
                }
            }

            final state fini {
                entry {
                   test.i = test.i - 2;
                }
            }
        }
        RootMachine RTState rt_state;
        rt_state.activate();
        rt_state.start();
        while( !rt_state.inFinalState() ) {
            trigger();
            yield; // ...has no effect here other than incrementing the step counter! See ScriptParser::seenstatement().
        })));
    BOOST_CHECK_EQUAL( sc->getStateMachineState("rt_state"), "fini" );
    BOOST_CHECK_EQUAL( i, -2);
}

BOOST_AUTO_TEST_CASE(TestExpressionParserRejectsTrailingInput)
{
    Parser parser(caller->engine());
    i = 0;

    try {
        DataSourceBase::shared_ptr result =
            parser.parseExpression("test.increase()test.increase()", tc);
        result->evaluate();
        BOOST_FAIL("The parser accepted two adjacent calls as one expression");
    } catch (const parse_exception&) {
    }

    BOOST_CHECK_EQUAL(i, 0);
}

BOOST_AUTO_TEST_CASE(TestScriptingServiceRejectsAdjacentCalls)
{
    PluginLoader::Instance()->loadService("scripting", tc);
    boost::shared_ptr<Scripting> scripting =
        tc->getProvider<Scripting>("scripting");
    BOOST_REQUIRE(scripting);

    i = 0;
    BOOST_CHECK(!scripting->eval("test.increase()test.increase()"));
    BOOST_CHECK_EQUAL(i, 0);
}

BOOST_AUTO_TEST_CASE(TestScriptingServiceReportsProgramLoadFailure)
{
    PluginLoader::Instance()->loadService("scripting", tc);
    ScriptingService::shared_ptr scripting =
        boost::dynamic_pointer_cast<ScriptingService>(
            tc->provides("scripting"));
    BOOST_REQUIRE(scripting);

    const std::string program = "program DuplicateProgram {}";
    BOOST_REQUIRE(scripting->eval(program));

    Parser parser(caller->engine());
    BOOST_CHECK_THROW(
        parser.runScript(program, tc, scripting.get(), "duplicate-program"),
        file_parse_exception);
}

BOOST_AUTO_TEST_CASE(TestScriptingServiceReportsOperationFailure)
{
    PluginLoader::Instance()->loadService("scripting", tc);
    ScriptingService::shared_ptr scripting =
        boost::dynamic_pointer_cast<ScriptingService>(
            tc->provides("scripting"));
    BOOST_REQUIRE(scripting);

    Parser parser(caller->engine());
    BOOST_CHECK_THROW(
        parser.runScript("test.fail()", tc, scripting.get(), "failing-operation"),
        file_parse_exception);
    BOOST_CHECK(!scripting->eval("test.fail()"));
}

BOOST_AUTO_TEST_CASE(TestExpressionParserAcceptsStatementTerminator)
{
    Parser parser(caller->engine());
    i = 0;

    DataSourceBase::shared_ptr result =
        parser.parseExpression("test.increase();", tc);
    BOOST_REQUIRE(result);
    result->evaluate();

    BOOST_CHECK_EQUAL(i, 1);
}

BOOST_AUTO_TEST_CASE(TestSingleInputParsersRequireCompleteInput)
{
    Parser parser(caller->engine());

    BOOST_CHECK_THROW(parser.parseCondition("", tc), parse_exception);
    BOOST_CHECK_THROW(parser.parseCondition("true false", tc), parse_exception);

    const std::string partial_name = "partial_parser_value";
    BOOST_REQUIRE(!tc->provides()->getValue(partial_name));
    BOOST_CHECK_THROW(
        parser.parseValueStatement(
            "var Int32 partial_parser_value = 1 trailing", tc),
        parse_exception);
    BOOST_CHECK(!tc->provides()->getValue(partial_name));

    const std::string invalid_name = "invalid_parser_value";
    BOOST_REQUIRE(!tc->provides()->getValue(invalid_name));
    BOOST_CHECK_THROW(
        parser.parseValueStatement(
            "var Int32 invalid_parser_value = \"not an integer\"", tc),
        parse_exception);
    BOOST_CHECK(!tc->provides()->getValue(invalid_name));

    const std::string complete_name = "complete_parser_value";
    BOOST_REQUIRE(!tc->provides()->getValue(complete_name));
    BOOST_REQUIRE(
        parser.parseValueStatement(
            "var Int32 complete_parser_value = 1;", tc));
    BOOST_CHECK(tc->provides()->getValue(complete_name));
    tc->provides()->removeValue(complete_name);
}

BOOST_AUTO_TEST_CASE(TestParserRejectsOversizedInputBeforeExecution)
{
    Parser parser(caller->engine());
    std::string oversized_input = "test.increase()";
    oversized_input.resize(Parser::MaxInputSize + 1U, ' ');

    i = 0;
    BOOST_CHECK_THROW(
        parser.parseExpression(oversized_input, tc),
        parse_exception);
    BOOST_CHECK_EQUAL(i, 0);

    PluginLoader::Instance()->loadService("scripting", tc);
    boost::shared_ptr<Scripting> scripting =
        tc->getProvider<Scripting>("scripting");
    BOOST_REQUIRE(scripting);
    BOOST_CHECK(!scripting->eval(oversized_input));
    BOOST_CHECK_EQUAL(i, 0);

    BOOST_REQUIRE(scripting->eval("test.increase()"));
    BOOST_CHECK_EQUAL(i, 1);
}

BOOST_AUTO_TEST_CASE(TestParserRejectsExcessiveNestingAndRecovers)
{
    Parser parser(caller->engine());
    const std::vector<std::pair<char, char> > delimiters = {
        std::make_pair('(', ')'),
        std::make_pair('[', ']'),
        std::make_pair('{', '}')
    };

    i = 0;
    for (const std::pair<char, char>& delimiter : delimiters) {
        std::string nested(Parser::MaxNestingDepth + 1U, delimiter.first);
        nested += "test.increase()";
        nested.append(Parser::MaxNestingDepth + 1U, delimiter.second);

        try {
            parser.parseExpression(nested, tc);
            BOOST_FAIL("The parser accepted excessive nesting");
        } catch (const parse_exception& error) {
            BOOST_CHECK(
                error.what().find("nesting depth limit") !=
                std::string::npos);
        }
    }
    BOOST_CHECK_EQUAL(i, 0);

    DataSourceBase::shared_ptr result =
        parser.parseExpression("test.increase()", tc);
    BOOST_REQUIRE(result);
    result->evaluate();
    BOOST_CHECK_EQUAL(i, 1);
}

BOOST_AUTO_TEST_CASE(TestParserNestingGuardIgnoresLiteralsAndComments)
{
    Parser parser(caller->engine());
    const std::string delimiters(Parser::MaxNestingDepth + 1U, '(');

    DataSourceBase::shared_ptr literal =
        parser.parseExpression("\"" + delimiters + "\"", tc);
    BOOST_REQUIRE(literal);

    i = 0;
    const std::vector<std::string> scripts = {
        "# " + delimiters + "\ntest.increase()",
        "// " + delimiters + "\ntest.increase()",
        "/* " + delimiters + " */\ntest.increase()"
    };
    for (const std::string& script : scripts)
        parser.runScript(script, tc, 0, "delimiter-comments");
    BOOST_CHECK_EQUAL(i, 3);
}

BOOST_AUTO_TEST_CASE(TestCallResultIndexing)
{
    Parser parser(caller->engine());

    DataSourceBase::shared_ptr result;
    try {
        result = parser.parseExpression("test.getState(2)[0]", tc);
    } catch (const parse_exception& error) {
        BOOST_FAIL(error.what());
    }
    BOOST_REQUIRE(result);
    DataSource<double>::shared_ptr value =
        dynamic_cast<DataSource<double>*>(result.get());
    BOOST_REQUIRE(value);
    BOOST_CHECK_EQUAL(value->get(), 2.0);
}

BOOST_AUTO_TEST_CASE(TestCallResultCArrayIndexingIsReadOnly)
{
    if (!Types()->type("cints")) {
        Types()->addType(new CArrayTypeInfo<carray<int> >("cints"));
    }
    if (!Types()->type("BType")) {
        Types()->addType(new StructTypeInfo<BType>("BType"));
    }

    ReadOnlyCArrayOperationProvider provider;
    tc->provides("test")->addOperation(
        "getBatch", &ReadOnlyCArrayOperationProvider::getBatch, &provider);

    Parser parser(caller->engine());
    DataSourceBase::shared_ptr result;
    try {
        result = parser.parseExpression("test.getBatch().ai[3]", tc);
    } catch (const parse_exception& error) {
        BOOST_FAIL(error.what());
    }

    BOOST_REQUIRE(result);
    DataSource<int>::shared_ptr value =
        DataSource<int>::narrow(result.get());
    BOOST_REQUIRE(value);
    BOOST_CHECK_EQUAL(value->get(), 99);
    BOOST_CHECK(!result->isAssignable());
}

BOOST_AUTO_TEST_CASE(TestDirectStructIndexingIsRejectedWithoutAborting)
{
    if (!Types()->type("cints")) {
        Types()->addType(new CArrayTypeInfo<carray<int> >("cints"));
    }
    if (!Types()->type("BType")) {
        Types()->addType(new StructTypeInfo<BType>("BType"));
    }

    ReadOnlyCArrayOperationProvider provider;
    tc->provides("test")->addOperation(
        "getBatch", &ReadOnlyCArrayOperationProvider::getBatch, &provider);

    Parser parser(caller->engine());
    try {
        parser.parseExpression("test.getBatch()[0]", tc);
        BOOST_FAIL("Direct struct indexing was accepted");
    } catch (const parse_exception_fatal_semantic_error& error) {
        BOOST_CHECK_NE(
            std::string(error.what()).find("Illegal use of []"),
            std::string::npos);
    }

    DataSourceBase::shared_ptr valid =
        parser.parseExpression("test.getBatch().ai[3]", tc);
    BOOST_REQUIRE(valid);
    DataSource<int>::shared_ptr value = DataSource<int>::narrow(valid.get());
    BOOST_REQUIRE(value);
    BOOST_CHECK_EQUAL(value->get(), 99);
}

BOOST_AUTO_TEST_CASE(TestExpressionParserRejectsMalformedCallIndexes)
{
    Parser parser(caller->engine());
    const std::vector<std::string> invalid_expressions = {
        "test.getState(2)[",
        "test.getState(2)[]",
        "test.getState(2)[0",
        "test.getState(2)[0][0]",
        "test.getState(2)[0].missing",
        "test.getState(2)[0]trailing",
        "test.getState(2)[\"invalid\"]"
    };

    for (const std::string& expression : invalid_expressions) {
        BOOST_TEST_CONTEXT(expression) {
            BOOST_CHECK_THROW(
                parser.parseExpression(expression, tc),
                parse_exception);
        }
    }
}

BOOST_AUTO_TEST_CASE(TestExpressionParserRejectsMissingResult)
{
    CommonParser common_parser;
    ExpressionParser parser(tc, caller->engine(), common_parser);

    BOOST_CHECK_THROW(parser.getResult(), parse_exception);
    BOOST_CHECK_THROW(parser.getCmdResult(), parse_exception);
    BOOST_CHECK_THROW(parser.getHandle(), parse_exception);
    BOOST_CHECK_THROW(parser.dropResult(), parse_exception);
}

BOOST_AUTO_TEST_CASE(TestParserRejectsNullContext)
{
    Parser parser(caller->engine());

    BOOST_CHECK_THROW(
        parser.runScript("", 0, 0, "null-context"),
        parse_exception);
    BOOST_CHECK_THROW(parser.parseFunction("", 0), parse_exception);
    BOOST_CHECK_THROW(parser.parseProgram("", 0), parse_exception);
    BOOST_CHECK_THROW(parser.parseStateMachine("", 0), parse_exception);
    BOOST_CHECK_THROW(parser.parseCondition("true", 0), parse_exception);
    BOOST_CHECK_THROW(parser.parseExpression("1", 0), parse_exception);
    BOOST_CHECK_THROW(parser.parseValueChange("1", 0), parse_exception);
    BOOST_CHECK_THROW(
        parser.parseValueStatement("var Int32 value = 1", 0),
        parse_exception);
}

BOOST_AUTO_TEST_CASE(TestStateMachineParserRollsBackEarlierRoots)
{
    Parser parser(caller->engine());
    const std::string service_name = "rollback_machine";
    const std::string script = MULTILINE_STRING(
        StateMachine ValidMachine {
            initial state ready {}
        }
        RootMachine ValidMachine rollback_machine;
        StateMachine InvalidMachine {
    );

    BOOST_REQUIRE(!tc->provides()->hasService(service_name));
    BOOST_CHECK_THROW(
        parser.parseStateMachine(script, tc),
        file_parse_exception);
    BOOST_CHECK(!tc->provides()->hasService(service_name));
}

BOOST_AUTO_TEST_CASE(TestScriptingFunction)
{
    PluginLoader::Instance()->loadService("scripting",tc);

    boost::shared_ptr<Scripting> sc = tc->getProvider<Scripting>("scripting");
    BOOST_REQUIRE( sc );
    BOOST_CHECK ( sc->ready() );

    // set test counter to zero:
    i = 0;

    // define a function (added to scripting interface):
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        Void func1(Void) {
            test.increase();
        })));
    BOOST_CHECK_EQUAL( i, 0);
    BOOST_CHECK( tc->provides("scripting")->hasMember("func1"));

    // export a function:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        export Void efunc1(Void) {
            test.increase();
        })));
    BOOST_CHECK_EQUAL( i, 0);
    BOOST_CHECK( tc->provides()->hasMember("efunc1"));

    // local function:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        Void lfunc1(Void) {
            test.increase();
        })));
    BOOST_CHECK_EQUAL( i, 0);
    BOOST_CHECK( tc->provides("scripting")->hasMember("lfunc1"));

    // global function:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        global Void gfunc1(Void) {
            test.increase();
        })));
    BOOST_CHECK_EQUAL( i, 0);
    BOOST_CHECK( GlobalService::Instance()->provides()->hasMember("gfunc1"));

    // nested function call:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        Void func2(Void) {
            func1();
        })));
    BOOST_CHECK_EQUAL( i, 0);
    BOOST_CHECK( tc->provides("scripting")->hasMember("func2"));

    // nested exported function call:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        Void efunc2(Void) {
            efunc1();
        })));
    BOOST_CHECK_EQUAL( i, 0);
    BOOST_CHECK( tc->provides("scripting")->hasMember("efunc2"));

    // nested global function call:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        Void gfunc2(Void) {
            gfunc1();
        })));
    BOOST_CHECK_EQUAL( i, 0);
    BOOST_CHECK( tc->provides("scripting")->hasMember("gfunc2"));

    // nested local function call:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        Void lfunc2(Void) {
            lfunc1();
        })));
    BOOST_CHECK_EQUAL( i, 0);
    BOOST_CHECK( tc->provides("scripting")->hasMember("lfunc2"));

    // invoke a function:
    BOOST_REQUIRE( sc->eval("func1()") );
    BOOST_CHECK_EQUAL( i, 1);

    // invoke an exported function:
    BOOST_REQUIRE( sc->eval("efunc1()") );
    BOOST_CHECK_EQUAL( i, 2);

    // invoke a global function:
    BOOST_REQUIRE( sc->eval("gfunc1()") );
    BOOST_CHECK_EQUAL( i, 3);

    // invoke a local function:
    BOOST_REQUIRE( sc->eval("lfunc1()") );
    BOOST_CHECK_EQUAL( i, 4);

    // invoke a function with a nested function call:
    BOOST_REQUIRE( sc->eval("func2()") );
    BOOST_CHECK_EQUAL( i, 5);

    // invoke a function with a nested exported function call:
    BOOST_REQUIRE( sc->eval("efunc2()") );
    BOOST_CHECK_EQUAL( i, 6);

    // invoke a function with a nested global function call:
    BOOST_REQUIRE( sc->eval("gfunc2()") );
    BOOST_CHECK_EQUAL( i, 7);

    // invoke a function with a nested local function call:
    BOOST_REQUIRE( sc->eval("lfunc2()") );
    BOOST_CHECK_EQUAL( i, 8);

    // call a function:
    BOOST_CHECK( !sc->eval("call func1()") );
    BOOST_CHECK_EQUAL( i, 8);

    // call an exported function:
    BOOST_CHECK( !sc->eval("call efunc1()") );
    BOOST_CHECK_EQUAL( i, 8);

    // RE-define a function (added to scripting interface):
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        Void func1(Void) {
            test.increase();
            test.increase();
        })));
    BOOST_CHECK_EQUAL( i, 8);
    BOOST_CHECK( tc->provides("scripting")->hasMember("func1"));

    BOOST_REQUIRE( sc->eval("func1()") );
    BOOST_CHECK_EQUAL( i, 10);

    // RE-export a function:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        export Void efunc1(Void) {
            test.increase();
            test.increase();
        })));
    BOOST_CHECK_EQUAL( i, 10);
    BOOST_CHECK( tc->provides()->hasMember("efunc1"));

    BOOST_REQUIRE( sc->eval("efunc1()") );
    BOOST_CHECK_EQUAL( i, 12);

    // RE-global a function:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        global Void gfunc1(Void) {
            test.increase();
            test.increase();
        })));
    BOOST_CHECK_EQUAL( i, 12);
    BOOST_CHECK( GlobalService::Instance()->provides()->hasMember("gfunc1"));

    BOOST_REQUIRE( sc->eval("gfunc1()") );
    BOOST_CHECK_EQUAL( i, 14);
}

BOOST_AUTO_TEST_CASE(TestFunctionYieldBeforeEnqueueReturns,
                     *boost::unit_test::timeout(2))
{
    // Complete the callback before process() returns, deterministically
    // reproducing an executor that outruns the calling thread.
    struct ImmediateEngine : ExecutionEngine {
        ImmediateEngine() : ExecutionEngine(nullptr), continuations(0) {}
        int continuations;
        bool process(base::DisposableInterface* message) override {
            message->executeAndDispose();
            return true;
        }
        bool runFunction(base::ExecutableInterface* function) override {
            ++continuations;
            const bool again = function->execute();
            if (!again) function->unloaded();
            return !again;
        }
    } engine;
    struct YieldOnce : scripting::FunctionGraph {
        YieldOnce() : FunctionGraph("yield_once", true), executions(0) { finish(); }
        int executions;
        bool execute() override {
            if (++executions == 1) return true;
            stop();
            return false;
        }
    };
    boost::shared_ptr<YieldOnce> function(new YieldOnce);
    scripting::CallFunction call(new scripting::CommandNOP, function, &engine, nullptr);

    BOOST_REQUIRE(call.execute());
    BOOST_CHECK_EQUAL(function->executions, 2);
    BOOST_CHECK_EQUAL(engine.continuations, 1);
    BOOST_CHECK(function->isStopped());
}

BOOST_AUTO_TEST_CASE(TestScriptingFunctionWithYield)
{
    PluginLoader::Instance()->loadService("scripting",tc);

    // We need a periodic activity for this test case so that yielded functions
    // will be executed again while we are waiting.
    tc->setPeriod(0.1);

    boost::shared_ptr<Scripting> sc = tc->getProvider<Scripting>("scripting");
    BOOST_REQUIRE( sc );
    BOOST_CHECK ( sc->ready() );

    // set test counter to zero:
    i = 0;

    // define a function that yields:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
          Void func1(Void) {
              test.printNumber("[ENTER func1()] CycleCounter = ", CycleCounter);
              test.increase();
              yield;
              test.increase();
              test.printNumber("[EXIT func1()] CycleCounter = ", CycleCounter);
          })));
    BOOST_CHECK_EQUAL( i, 0);
    BOOST_CHECK( tc->provides("scripting")->hasMember("func1"));

    // define a function that calls func1, yields and calls func1 again:
    BOOST_REQUIRE( sc->eval( MULTILINE_STRING(
        Void func2(Void) {
            test.printNumber("[ENTER func2()] CycleCounter = ", CycleCounter);
            func1();
            yield;
            func1();
            test.printNumber("[EXIT func2()] CycleCounter = ", CycleCounter);
        })));
    BOOST_CHECK_EQUAL( i, 0);
    BOOST_CHECK( tc->provides("scripting")->hasMember("func2"));

    // invoke func1()
    BOOST_REQUIRE( sc->eval("func1()") );
    BOOST_CHECK_EQUAL( i, 2);

    // invoke func2()
    BOOST_REQUIRE( sc->eval("func2()") );
    BOOST_CHECK_EQUAL( i, 6);
}

BOOST_AUTO_TEST_SUITE_END()
