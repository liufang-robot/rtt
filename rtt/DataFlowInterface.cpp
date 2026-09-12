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
        if ((getOwner() && getOwner()->base::TaskCore::isRunning()) || !port.prepareConnectionChange())
            throw std::runtime_error("Cannot change ports of a running component");
        if (getOwner()) getOwner()->invalidateConnections();
        if ( !chkPtr("addPort", "PortInterface", &port) ) return port;
        this->addLocalPort(port);
        Service::shared_ptr mservice_ref;
        if (mservice && mservice->hasService( port.getName()) ) {
            // Since there is at least one child service, mservice is ref counted. The danger here is that mservice is destructed during removeService()
            // for this reason, we take a ref to mservice until we leave addPort.
            mservice_ref = mservice->provides(); // uses shared_from_this()
            Logger::log().logf(Logger::Warning, "DataFlowInterface",
                               "'addPort' %s: name already in use as Service. Replacing previous service with new one.",
                               port.getName().c_str());
            mservice->removeService(port.getName());
        }

        if (!mservice) {
            Logger::log().logf(Logger::Warning, "DataFlowInterface",
                               "'addPort' %s: DataFlowInterface not given to parent. Not adding Service.",
                               port.getName().c_str());
            return port;
        }
        Service::shared_ptr ms( this->createPortObject( port.getName()) );
        if ( ms )
            mservice->addService( ms );
        // END NOTE.
        return port;
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

    InputPortInterface& DataFlowInterface::addEventPort(InputPortInterface& port, SlotFunction callback) {
        if ((getOwner() && getOwner()->base::TaskCore::isRunning()) || !port.prepareConnectionChange())
            throw std::runtime_error("Cannot change ports of a running component");
        if (getOwner()) getOwner()->invalidateConnections();
        if ( !chkPtr("addEventPort", "PortInterface", &port) ) return port;
        this->addLocalEventPort(port, callback);
        Service::shared_ptr mservice_ref;
        if (mservice && mservice->hasService( port.getName()) ) {
            // Since there is at least one child service, mservice is ref counted. The danger here is that mservice is destructed during removeService()
            // for this reason, we take a ref to mservice until we leave addPort.
            mservice_ref = mservice->provides(); // uses shared_from_this()
            Logger::log().logf(Logger::Warning, "DataFlowInterface",
                               "'addPort' %s: name already in use as Service. Replacing previous service with new one.",
                               port.getName().c_str());
            mservice->removeService(port.getName());
        }

        if (!mservice) {
            Logger::log().logf(Logger::Warning, "DataFlowInterface",
                               "'addPort' %s: DataFlowInterface not given to parent. Not adding Service.",
                               port.getName().c_str());
            return port;
        }
        Service::shared_ptr ms( this->createPortObject( port.getName()) );
        if ( ms )
            mservice->addService( ms );
        return port;
    }

#ifdef ORO_SIGNALLING_PORTS
    void DataFlowInterface::setupHandles() {
        for_each(handles.begin(), handles.end(), boost::bind(&Handle::connect, _1));
    }

    void DataFlowInterface::cleanupHandles() {
        for_each(handles.begin(), handles.end(), boost::bind(&Handle::disconnect, _1));
    }
#else
    void DataFlowInterface::dataOnPort(base::PortInterface* port)
    {
        if ( mservice && mservice->getOwner() )
            mservice->getOwner()->dataOnPort(port);
    }
#endif

    InputPortInterface& DataFlowInterface::addLocalEventPort(InputPortInterface& port, SlotFunction callback) {
        this->addLocalPort(port);

        if (mservice == 0 || mservice->getOwner() == 0) {
            Logger::log().logf(Logger::Error, "DataFlowInterface",
                               "addLocalEventPort %s: DataFlowInterface not part of a TaskContext. Will not trigger any TaskContext nor register callback.",
                               port.getName().c_str());
            return port;
        }

#ifdef ORO_SIGNALLING_PORTS
        // setup synchronous callback, only purpose is to register that port fired and trigger the TC's engine.
        Handle h = port.getNewDataOnPortEvent()->connect(boost::bind(&TaskContext::dataOnPort, mservice->getOwner(), _1) );
        if (h) {
            Logger::log().logf(Logger::Info, "DataFlowInterface",
                               "%s will be triggered when new data is available on InputPort %s",
                               mservice->getName().c_str(), port.getName().c_str());
            handles.push_back(h);
        } else {
            Logger::log().logf(Logger::Error, "DataFlowInterface",
                               "%s can't connect to event of InputPort %s",
                               mservice->getName().c_str(), port.getName().c_str());
            return port;
        }
#endif
        if (callback)
            mservice->getOwner()->setDataOnPortCallback(&port,callback); // the handle will be deleted when the port is removed.
        else
            mservice->getOwner()->setDataOnPortCallback(&port,boost::bind(&TaskCore::trigger, mservice->getOwner()) ); // default schedules an updateHook()

#ifndef ORO_SIGNALLING_PORTS
        port.signalInterface(true);
#endif
        return port;
    }

    void DataFlowInterface::removePort(const std::string& name) {
        if (getOwner() && getOwner()->base::TaskCore::isRunning())
            throw std::runtime_error("Cannot remove a port of a running component");
        if (getOwner()) getOwner()->invalidateConnections();
        for ( Ports::iterator it(mports.begin());
              it != mports.end();
              ++it)
            if ( (*it)->getName() == name ) {
                if (!(*it)->connectionChangeAllowed())
                    throw std::runtime_error("Cannot remove a port while a connected component is running");
                (*it)->disconnect(); // remove all connections and callbacks.
                Service::shared_ptr mservice_ref;
                if (mservice && mservice->hasService(name) ) {
                    // Since there is at least one child service, mservice is ref counted. The danger here is that mservice is destructed during removeService()
                    // for this reason, we take a ref to mservice until we leave removePort.
                    mservice_ref = mservice->provides(); // uses shared_from_this()
                    mservice->removeService( name );
                    if (mservice->getOwner())
                        mservice->getOwner()->removeDataOnPortCallback( *it );
                }
                (*it)->setInterface(0);
                mports.erase(it);
                return;
            }
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
                (*it)->disconnect(); // remove all connections and callbacks.
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
        Service::shared_ptr srv = mservice->getService(name);
        if (srv) {
            srv->doc(description);
            return true;
        }
        return false;
    }

    Service* DataFlowInterface::createPortObject(const std::string& name) {
        PortInterface* p = this->getPort(name);
        if ( !p )
            return 0;
        Service* to = p->createPortObject();
        if (to) {
            std::string d = this->getPortDescription(name);
            if ( !d.empty() )
                to->doc( d );
            else
                to->doc("No description set for this Port. Use .doc() to document it.");
        }
        return to;
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
