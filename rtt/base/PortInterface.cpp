/***************************************************************************
  tag: Peter Soetens  Thu Oct 22 11:59:07 CEST 2009  PortInterface.cpp

                        PortInterface.cpp -  description
                           -------------------
    begin                : Thu October 22 2009
    copyright            : (C) 2009 Sylvain Joyeux
    email                : sylvain.joyeux@m4x.org

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


#include "PortInterface.hpp"
#include "../Service.hpp"
#include "../OperationCaller.hpp"
#include "../internal/ConnFactory.hpp"
#include "../TaskContext.hpp"
#include <cstring>
#include <algorithm>
#include <set>
#include <stdexcept>
#include "../internal/CyclicDataFlow.hpp"
#include "InputPortInterface.hpp"
#include "OutputPortInterface.hpp"

using namespace RTT;
using namespace RTT::detail;
using namespace std;

PortInterface::PortInterface(const std::string& name)
    : name(name), fullName(name), iface(0), connection_lock(), cmanager(this) {}

PortInterface::~PortInterface() {
    // Typed implementations already synchronized before releasing their images.
    // This fallback only drops metadata: virtual channel access is no longer valid.
    while (!cyclicDependencies.empty()) cyclicDependencies.back()->forget(*this);
}

bool PortInterface::setName(const std::string& name) {
    if ( !connected() && prepareConnectionChange() ) {
        this->name = name;
        updateFullName();
        return true;
    }
    return false;
}

void PortInterface::updateFullName() {
    DataFlowInterface* dataflow = getInterface();
    fullName = getName();
    if (dataflow) {
        Service* service = dataflow->getServiceInterface();
        std::set<Service*> visited;
        while (service && visited.insert(service).second) {
            fullName = service->getName() + "." + fullName;
            Service::shared_ptr parent = service->getParent();
            service = parent.get();
        }
        if (!dataflow->getServiceInterface() && dataflow->getOwner())
            fullName = dataflow->getOwner()->getName() + "." + fullName;
    }
}

PortInterface& PortInterface::doc(const std::string& desc) {
    mdesc = desc;
    if (iface)
        iface->setPortDescription(name, desc);
    return *this;
}

bool PortInterface::connectedTo(PortInterface* port) {
    if (cmanager.connectedTo(port)) return true;
    for (size_t i = 0; i < cyclicDependencies.size(); ++i)
        if (cyclicDependencies[i]->contains(*this, port)) return true;
    return false;
}

bool PortInterface::isLocal() const
{ return serverProtocol() == 0; }
int PortInterface::serverProtocol() const
{ return 0; }

ConnID* PortInterface::getPortID() const
{ return new LocalConnID(this); }

Service* PortInterface::createPortObject()
{
#ifndef ORO_EMBEDDED
    Service* to = new Service( this->getName(), iface->getOwner() );
    to->addSynchronousOperation( "name",&PortInterface::getName, this).doc(
            "Returns the port name.");
    to->addSynchronousOperation("connected", &PortInterface::connected, this).doc("Check if this port is connected and ready for use.");

    typedef void (PortInterface::*disconnect_all)();
    to->addSynchronousOperation("disconnect", static_cast<disconnect_all>(&PortInterface::disconnect), this).doc("Disconnects this port from any connection it is part of.");
    return to;
#else
    return 0;
#endif
}

bool PortInterface::removeConnection(ConnID* conn)
{
    return cmanager.removeConnection(conn);
}

void PortInterface::setInterface(DataFlowInterface* dfi) {
    if (iface != dfi) {
        if (!prepareConnectionChange() || (dfi && dfi->getOwner() && dfi->getOwner()->base::TaskCore::isRunning()))
            throw std::runtime_error("Cannot move a port while an affected component is running");
        while (!cyclicDependencies.empty()) cyclicDependencies.back()->forget(*this);
    }
    iface = dfi;
    updateFullName();
}

DataFlowInterface* PortInterface::getInterface() const
{
    return iface;
}

internal::SharedConnectionBase::shared_ptr PortInterface::getSharedConnection() const
{
    return cmanager.getSharedConnection();
}

void PortInterface::addCyclicDependency(internal::CyclicDataFlow* plan) {
    if (std::find(cyclicDependencies.begin(), cyclicDependencies.end(), plan) == cyclicDependencies.end())
        cyclicDependencies.push_back(plan);
}
void PortInterface::removeCyclicDependency(internal::CyclicDataFlow* plan) {
    cyclicDependencies.erase(std::remove(cyclicDependencies.begin(), cyclicDependencies.end(), plan), cyclicDependencies.end());
}
bool PortInterface::connectionChangeAllowed() const {
    if (iface && iface->getOwner() && (iface->getOwner()->base::TaskCore::isRunning() ||
        iface->getOwner()->base::TaskCore::getTargetState() >= base::TaskCore::Running)) return false;
    for (size_t i = 0; i < cyclicDependencies.size(); ++i)
        if (cyclicDependencies[i]->owner().base::TaskCore::isRunning() ||
            cyclicDependencies[i]->owner().base::TaskCore::getTargetState() >= base::TaskCore::Running ||
            !cyclicDependencies[i]->connectionChangeAllowed(*this)) return false;
    internal::ConnectionManager::Connections connections = cmanager.getConnections();
    for (internal::ConnectionManager::Connections::const_iterator it = connections.begin(); it != connections.end(); ++it) {
        internal::LocalConnID* id = dynamic_cast<internal::LocalConnID*>(boost::get<0>(*it).get());
        TaskContext* peer = id && id->ptr && id->ptr->getInterface() ? id->ptr->getInterface()->getOwner() : 0;
        if (peer && (peer->base::TaskCore::isRunning() || peer->base::TaskCore::getTargetState() >= base::TaskCore::Running)) return false;
    }
    return true;
}
bool PortInterface::prepareConnectionChange() {
    if (!connectionChangeAllowed()) return false;
    if (iface && iface->getOwner()) iface->getOwner()->invalidateConnections();
    for (size_t i = 0; i < cyclicDependencies.size(); ++i) cyclicDependencies[i]->invalidate();
    internal::ConnectionManager::Connections connections = cmanager.getConnections();
    for (internal::ConnectionManager::Connections::const_iterator it = connections.begin(); it != connections.end(); ++it) {
        internal::LocalConnID* id = dynamic_cast<internal::LocalConnID*>(boost::get<0>(*it).get());
        if (id && id->ptr && id->ptr->getInterface() && id->ptr->getInterface()->getOwner())
            id->ptr->getInterface()->getOwner()->invalidateConnections();
    }
    return true;
}
bool PortInterface::hasMemberConnections() const {
    for (size_t i = 0; i < cyclicDependencies.size(); ++i)
        if (cyclicDependencies[i]->contains(*this)) return true;
    return false;
}
bool PortInterface::disconnectMemberConnections(PortInterface* other) {
    bool removed = false;
    for (size_t i = 0; i < cyclicDependencies.size(); ++i)
        removed = cyclicDependencies[i]->disconnect(*this, other) || removed;
    return removed;
}
void PortInterface::preparePortDestruction() {
    if (iface && iface->getOwner() && iface->getOwner()->base::TaskCore::isRunning()) iface->getOwner()->stop();
    internal::ConnectionManager::Connections connections = cmanager.getConnections();
    for (internal::ConnectionManager::Connections::const_iterator it = connections.begin(); it != connections.end(); ++it) {
        internal::LocalConnID* id = dynamic_cast<internal::LocalConnID*>(boost::get<0>(*it).get());
        TaskContext* peer = id && id->ptr && id->ptr->getInterface() ? id->ptr->getInterface()->getOwner() : 0;
        if (peer && peer->base::TaskCore::isRunning()) peer->stop();
    }
    std::vector<internal::CyclicDataFlow*> plans = cyclicDependencies;
    for (size_t i = 0; i < plans.size(); ++i) {
        if (plans[i]->owner().base::TaskCore::isRunning()) plans[i]->owner().stop();
        plans[i]->forget(*this);
    }
    if (iface) iface->removeLocalPort(name);
}

bool PortInterface::validateWholeConnection(PortInterface& other) const {
    const OutputPortInterface* source = dynamic_cast<const OutputPortInterface*>(this);
    InputPortInterface* destination = dynamic_cast<InputPortInterface*>(&other);
    return source && destination && internal::CyclicDataFlow::validateWhole(
        *const_cast<OutputPortInterface*>(source), *destination);
}
