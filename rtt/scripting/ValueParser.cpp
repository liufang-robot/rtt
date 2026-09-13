/***************************************************************************
  tag: Peter Soetens  Mon May 10 19:10:37 CEST 2004  ValueParser.cxx

                        ValueParser.cxx -  description
                           -------------------
    begin                : Mon May 10 2004
    copyright            : (C) 2004 Peter Soetens
    email                : peter.soetens@mech.kuleuven.ac.be

 ***************************************************************************
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU General Public                   *
 *   License as published by the Free Software Foundation;                 *
 *   version 2 of the License.                                             *
 *                                                                         *
 *   As a special exception, you may use this file as part of a free       *
 *   software library without restriction.  Specifically, if other files   *
 *   instantiate templates or use macros or inline functions from this     *
 *   file, or you compile this file and link it with other files to        *
 *   produce an executable, this file does not by itself cause the         *
 *   resulting executable to be covered by the GNU General Public          *
 *   License.  This exception does not however invalidate any other        *
 *   reasons why the executable file might be covered by the GNU General   *
 *   Public License.                                                       *
 *                                                                         *
 *   This library is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU     *
 *   General Public License for more details.                              *
 *                                                                         *
 *   You should have received a copy of the GNU General Public             *
 *   License along with this library; if not, write to the Free Software   *
 *   Foundation, Inc., 59 Temple Place,                                    *
 *   Suite 330, Boston, MA  02111-1307  USA                                *
 *                                                                         *
 ***************************************************************************/

#include "parser-debug.hpp"
#include "parse_exception.hpp"
#include "ValueParser.hpp"
#include "../PortEndpoint.hpp"
#include "../Attribute.hpp"

#include "../TaskContext.hpp"
#include "../Service.hpp"
#include "../types/GlobalsRepository.hpp"

#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>

#include <iostream>
using namespace std;

namespace RTT
{
    using boost::bind;
    using namespace detail;

    ValueParser::ValueParser( TaskContext* tc, CommonParser& cp)
        : commonparser(cp), peerparser(tc,cp), propparser(cp)
  {
#ifndef ORO_DISABLE_PORT_DATA_SCRIPTING
    peerparser.stopAtPort(true);
#endif
    BOOST_SPIRIT_DEBUG_RULE( constant );
    BOOST_SPIRIT_DEBUG_RULE( const_float );
    BOOST_SPIRIT_DEBUG_RULE( const_double );
    BOOST_SPIRIT_DEBUG_RULE( const_int );
    BOOST_SPIRIT_DEBUG_RULE( const_hex );
    BOOST_SPIRIT_DEBUG_RULE( const_uint );
    BOOST_SPIRIT_DEBUG_RULE( const_int64 );
    BOOST_SPIRIT_DEBUG_RULE( const_uint64 );
    BOOST_SPIRIT_DEBUG_RULE( const_char );
    BOOST_SPIRIT_DEBUG_RULE( const_bool );
    BOOST_SPIRIT_DEBUG_RULE( const_string );
    BOOST_SPIRIT_DEBUG_RULE( named_constant );

    // note the order is important: commonparser.identifier throws a
    // useful "cannot use x as identifier" error if it fails, so we
    // must first show all non-identifier rules.
    constant =
        const_float
      | const_double
      | const_hex
      | const_uint64
      | const_int64
      | const_uint
      | const_int
      | const_bool
      | const_char
      | const_string
      | named_constant;

    const_float =
      strict_real_p [
        boost::bind( &ValueParser::seenfloatconstant, this, _1 ) ] >> ch_p('f');

    const_double =
      strict_real_p [
        boost::bind( &ValueParser::seendoubleconstant, this, _1 ) ];

    const_hex = (str_p("0x") | str_p("0X")) >>
      uint_parser<std::uint32_t, 16>() [
        boost::bind( &ValueParser::seenhexconstant, this, _1 ) ];

    const_uint64 =
      uint_parser<std::uint64_t>() [
        boost::bind( &ValueParser::seenuint64constant, this, _1 ) ] >> str_p("ull");

    const_int64 =
      uint_parser<std::int64_t>() [
        boost::bind( &ValueParser::seenint64constant, this, _1 ) ] >> str_p("ll");

    const_uint =
      uint_parser<std::uint32_t>() [
        boost::bind( &ValueParser::seenuintconstant, this, _1 ) ] >> ch_p('u');

    const_int =
      int_parser<std::int32_t>() [
        boost::bind( &ValueParser::seenintconstant, this, _1 ) ];

    const_bool =
      ( keyword_p( "true" ) | keyword_p("false") )[
        boost::bind( &ValueParser::seenboolconstant, this, _1, _2 ) ];

    const_char = (ch_p('\'') >> ch_p('\\') >> ch_p('0') >> ch_p('\''))[boost::bind( &ValueParser::seennull,this)] |
        confix_p( "'", (c_escape_ch_p[ boost::bind( &ValueParser::seencharconstant, this, _1 ) ]) , "'" );

    const_string = lexeme_d[confix_p(
      ch_p( '"' ), *c_escape_ch_p[ boost::bind( &ValueParser::push_str_char, this, _1 ) ], '"' )[ boost::bind( &ValueParser::seenstring, this ) ]];

    named_constant =
        ( keyword_p("done")[boost::bind( &ValueParser::seennamedconstant, this, _1, _2 ) ]
          |
          ( peerparser.locator()[boost::bind( &ValueParser::seenpeer, this) ]
            >> propparser.locator()
            >> commonparser.identifier[boost::bind( &ValueParser::seennamedconstant, this, _1, _2 ) ]) )
        ;
  }

    void ValueParser::seenpeer() {
        // inform propparser of new peer :
        //std::cerr << "ValueParser: seenpeer : "<< peerparser.taskObject()->getName()
        //          <<" has props :" << (peerparser.taskObject()->properties() != 0) << std::endl;
#ifndef ORO_DISABLE_PORT_DATA_SCRIPTING
        if (peerparser.taskObject()->getPort(peerparser.object())) {
            propparser.setPropertyBag(nullptr);
            return;
        }
#endif
        propparser.setPropertyBag( peerparser.taskObject()->properties() );
    }

  void ValueParser::seenboolconstant( iter_t begin, iter_t end )
  {
    std::string value( begin, end );
    assert( value == "true" || value == "false" );
    if ( value == "true" )
      ret =
        new ConstantDataSource<bool>( true );
    else
      ret =
        new ConstantDataSource<bool>( false );
  }

  void ValueParser::seennamedconstant( iter_t begin, iter_t end )
  {
    std::string name( begin, end );
    Service::shared_ptr task = peerparser.taskObject();
    peerparser.reset();
    //std::cerr << "ValueParser: seenvar : "<< name
    //          <<" is bag : " << (propparser.bag() != 0) << " is prop: "<< (propparser.property() != 0) << std::endl;
    // in case our task is a taskcontext:
    if ( task && propparser.bag() && propparser.property() ) {
        // nested property case :
        if ( ! propparser.bag()->find( name ) ) {
            //std::cerr << "In "<<peer->getName() <<" : " << name << " not present"<<std::endl;
            throw parse_exception_semantic_error("Property " + name + " not present in PropertyBag "+propparser.property()->getName()+" in "+ task->getName()+".");
        }
        ret = propparser.bag()->find( name )->getDataSource();
        propparser.reset();
        return;
    }

#ifndef ORO_DISABLE_PORT_DATA_SCRIPTING
    if (task && task->getPort(name)) {
        std::string error;
        auto observation = PortObservation::create(PortEndpoint{task->getPort(name), ""}, &error);
        if (!observation)
            throw parse_exception_semantic_error("Cannot observe port " + name + ": " + error);
        ret = observation->dataSource();
        return;
    }
#endif

    // non-nested property or attribute case :
    if ( task && task->hasAttribute( name ) ) {
      ret = task->getValue(name)->getDataSource();
      return;
    }
    if ( task && task->hasProperty( name ) ) {
        ret = task->properties()->find(name)->getDataSource();
        return;
    }

    // Global Attribute case:
    if ( GlobalsRepository::Instance()->hasAttribute( name ) ) {
        ret = GlobalsRepository::Instance()->getValue(name)->getDataSource();
        return;
    }

    // Global Property case:
    if ( GlobalsRepository::Instance()->hasProperty( name ) ) {
        ret = GlobalsRepository::Instance()->properties()->find(name)->getDataSource();
        return;
    }

    throw_(begin, "Value " + name + " not defined in "+ task->getName()+".");
  }

    void ValueParser::seennull()
    {
        ret = new ConstantDataSource<char>( '\0' );
    }

    void ValueParser::seencharconstant( iter_t c )
    {
        ret = new ConstantDataSource<char>( *c );
    }

    void ValueParser::seenhexconstant( std::uint32_t i )
    {
      ret = new ConstantDataSource<std::uint32_t>( i );
    }

  void ValueParser::seenintconstant( std::int32_t i )
  {
    ret = new ConstantDataSource<std::int32_t>( i );
  }

  void ValueParser::seenuintconstant( std::uint32_t i )
  {
    ret = new ConstantDataSource<std::uint32_t>( i );
  }

  void ValueParser::seenint64constant( std::int64_t i )
  {
    ret = new ConstantDataSource<std::int64_t>( i );
  }

  void ValueParser::seenuint64constant( std::uint64_t i )
  {
    ret = new ConstantDataSource<std::uint64_t>( i );
  }

  void ValueParser::seenfloatconstant( double i )
  {
    ret = new ConstantDataSource<float>( float(i) );
  }

  void ValueParser::seendoubleconstant( double i )
  {
    ret = new ConstantDataSource<double>( i );
  }

  ValueParser::~ValueParser()
  {
    clear();
  }

  void ValueParser::clear()
  {
      propparser.reset();
  }

  rule_t& ValueParser::parser()
  {
    return constant;
  }

  void ValueParser::push_str_char( char c )
  {
    mcurstring += c;
  }

  void ValueParser::seenstring()
  {
    // due to our config parse rule, the '"' terminating a
    // string will be in mcurstring, and we don't want it, so we
    // remove it..
    mcurstring.erase( mcurstring.end() - 1 );
    ret = new ConstantDataSource<std::string>( mcurstring );
    //deleter.reset( ret );
    mcurstring.clear();
  }
}
