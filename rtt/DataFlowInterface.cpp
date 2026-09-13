/***************************************************************************
  tag: FMTC  Tue Mar 11 21:49:27 CET 2008  DataFlowInterface.cpp

                        DataFlowInterface.cpp -  description
                           -------------------
    begin                : Tue March 11 2008
    copyright            : (C) 2008 FMTC
    email                : peter.soetens@fmtc.be

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
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU General Public             *
 *   License along with this library; if not, write to the Free Software   *
 *   Foundation, Inc., 59 Temple Place,                                    *
 *   Suite 330, Boston, MA  02111-1307  USA                                *
 *                                                                         *
 ***************************************************************************/


#include "DataFlowInterface.hpp"
#include <stdexcept>
#include "Logger.hpp"
#include "Service.hpp"
#include "TaskContext.hpp"

namespace RTT
{
    using namespace detail;

    DataFlowInterface::DataFlowInterface(Service* parent /* = 0 */)
        : mservice(parent)
    {}

    DataFlowInterface::~DataFlowInterface() {
    }

    TaskContext* DataFlowInterface::getOwner() const {
        return mservice ? mservice->getOwner() : 0;
    }

    PortInterface& DataFlowInterface::addPort(PortInterface& port) {
        return addLocalPort(port);
    }

    PortInterface& DataFlowInterface::addLocalPort(PortInterface& port) {
        if ((getOwner() && getOwner()->base::TaskCore::isRunning()) || !port.prepareConnectionChange())
            throw std::runtime_error("Cannot change ports of a running component");
        if (getOwner()) getOwner()->invalidateConnections();
        for ( Ports::iterator it(mports.begin());
              it != mports.end();
              ++it)
            if ( (*it)->getName() == port.getName() ) {
                Logger::log().logf(Logger::Warning, "DataFlowInterface",
                                   "'addPort' %s: name already in use. Disconnecting and replacing previous port with new one.",
                                   port.getName().c_str());
                removeLocalPort( port.getName() );
                break;
            }

        mports.push_back( &port );
        port.setInterface( this );
        return port;
    }

    void DataFlowInterface::removePort(const std::string& name) {
        removeLocalPort(name);
    }

    void DataFlowInterface::removeLocalPort(const std::string& name) {
        if (getOwner() && getOwner()->base::TaskCore::isRunning())
            throw std::runtime_error("Cannot remove a port of a running component");
        if (getOwner()) getOwner()->invalidateConnections();
        for ( Ports::iterator it(mports.begin());
              it != mports.end();
              ++it)
            if ( (*it)->getName() == name ) {
                if (!(*it)->connectionChangeAllowed())
                    throw std::runtime_error("Cannot remove a port while a connected component is running");
                (*it)->disconnect(); // remove all connections.
                (*it)->setInterface(0);
                mports.erase(it);
                return;
            }
    }

    DataFlowInterface::Ports DataFlowInterface::getPorts() const {
        return mports;
    }

    DataFlowInterface::PortNames DataFlowInterface::getPortNames() const {
        std::vector<std::string> res;
        for ( Ports::const_iterator it(mports.begin());
              it != mports.end();
              ++it)
            res.push_back( (*it)->getName() );
        return res;
    }

    PortInterface* DataFlowInterface::getPort(const std::string& name) const {
        for ( Ports::const_iterator it(mports.begin());
              it != mports.end();
              ++it)
            if ( (*it)->getName() == name )
                return *it;
        return 0;
    }

    std::string DataFlowInterface::getPortDescription(const std::string& name) const {
        for ( Ports::const_iterator it(mports.begin());
              it != mports.end();
              ++it)
            if ( (*it)->getName() == name )
                return (*it)->getDescription();
        return "";
    }

    bool DataFlowInterface::setPortDescription(const std::string& name, const std::string description) {
        auto* port = getPort(name);
        if (!port) return false;
        port->doc(description);
        return true;
    }

    void DataFlowInterface::clear()
    {
        if (mports.empty()) return;
        if (getOwner() && getOwner()->base::TaskCore::isRunning())
            throw std::runtime_error("Cannot clear ports of a running component");
        if (getOwner()) getOwner()->invalidateConnections();
        while (!mports.empty()) removePort(mports.back()->getName());
    }

    bool DataFlowInterface::chkPtr(const std::string & where, const std::string & name, const void *ptr)
    {
        if ( ptr == 0) {
            Logger::log().logf(Logger::Error, "DataFlowInterface",
                               "You tried to add a null pointer in '%s' for the object '%s'. Fix your code !",
                               where.c_str(), name.c_str());
            return false;
        }
        return true;
    }

}
