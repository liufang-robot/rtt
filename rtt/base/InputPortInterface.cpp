/***************************************************************************
  tag: Peter Soetens  Thu Oct 22 11:59:07 CEST 2009  InputPortInterface.cpp

                        InputPortInterface.cpp -  description
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
#include "InputPortInterface.hpp"
#include "OutputPortInterface.hpp"
#include "DataFlowInterface.hpp"
#include "../internal/ConnInputEndPoint.hpp"
#include "../internal/ConnFactory.hpp"
#include "../internal/CyclicDataFlow.hpp"
#include "../TaskContext.hpp"
#include "../Logger.hpp"
#include <algorithm>
#include <exception>
#include <set>
#include <stdexcept>
#include <tuple>
#include <../os/traces.h>

using namespace RTT;
using namespace RTT::detail;
using namespace std;

InputPortInterface::InputPortInterface(std::string const& name, ConnPolicy const& default_policy)
: PortInterface(name)
  , default_policy( default_policy )
#ifdef ORO_SIGNALLING_PORTS
  , new_data_on_port_event(0)
#else
 , msignal_interface(false)
#endif
{}

InputPortInterface::~InputPortInterface()
{
    cmanager.disconnect();
#ifdef ORO_SIGNALLING_PORTS
    if ( new_data_on_port_event) {
        delete new_data_on_port_event;
    }
#endif
}

ConnPolicy InputPortInterface::getDefaultPolicy() const
{ return default_policy; }

namespace {
// Shared channels may have several producers. getInput() alone would select
// only the last signalling branch and would report a misleading source.
void sharedSources(ChannelElementBase::shared_ptr channel, InputPortInterface::SourceConnections& result)
{
    std::vector<ChannelElementBase::shared_ptr> pending(1, channel);
    std::set<ChannelElementBase*> visited;
    const size_t before = result.size();
    while (!pending.empty()) {
        ChannelElementBase::shared_ptr current = pending.back();
        pending.pop_back();
        if (!current || !visited.insert(current.get()).second) continue;
        if (const OutputPortInterface* output = dynamic_cast<const OutputPortInterface*>(current->getPort())) {
            result.push_back(InputPortInterface::SourceConnection{output->getFullName(), "", ""});
            continue;
        }
        if (MultipleInputsChannelElementBase* multiple = dynamic_cast<MultipleInputsChannelElementBase*>(current.get())) {
            const MultipleInputsChannelElementBase::Inputs inputs = multiple->getInputs();
            pending.insert(pending.end(), inputs.begin(), inputs.end());
            if (!inputs.empty()) continue;
        } else if (ChannelElementBase::shared_ptr input = current->getInput()) {
            pending.push_back(input);
            continue;
        }
        result.push_back(InputPortInterface::SourceConnection{"", "", ""});
    }
    if (result.size() == before) result.push_back(InputPortInterface::SourceConnection{"", "", ""});
}
}

InputPortInterface::SourceConnections InputPortInterface::getSourceConnections() const
{
    SourceConnections result;
    if (iface && iface->getOwner())
        result = iface->getOwner()->cyclicDataFlow().sources(*this);

    // Retain the input lock while inspecting physical descriptors. Graph changes
    // and endpoint destruction still require the caller's topology serialization.
    internal::PortConnectionLock lock(const_cast<InputPortInterface*>(this));
    const internal::ConnectionManager::Connections connections = cmanager.getConnections();
    for (const auto& connection : connections) {
        const internal::ConnID* id = boost::get<0>(connection).get();
        const internal::LocalConnID* local = dynamic_cast<const internal::LocalConnID*>(id);
        const OutputPortInterface* output = local ? dynamic_cast<const OutputPortInterface*>(local->ptr) : 0;
        if (output) result.push_back(SourceConnection{output->getFullName(), "", ""});
        else if (const internal::SharedConnID* shared = dynamic_cast<const internal::SharedConnID*>(id))
            sharedSources(shared->connection, result);
        else result.push_back(SourceConnection{"", "", ""});
    }
    std::sort(result.begin(), result.end(), [](const SourceConnection& a, const SourceConnection& b) {
        return std::tie(a.destinationMember, a.sourcePort, a.sourceMember) <
               std::tie(b.destinationMember, b.sourcePort, b.sourceMember);
    });
    return result;
}

#ifdef ORO_SIGNALLING_PORTS
InputPortInterface::NewDataOnPortEvent* InputPortInterface::getNewDataOnPortEvent()
{
    if (!new_data_on_port_event)
        new_data_on_port_event = new NewDataOnPortEvent();
    return new_data_on_port_event;
}
#endif

bool InputPortInterface::connectTo(PortInterface* other, ConnPolicy const& policy)
{
    OutputPortInterface* output = dynamic_cast<OutputPortInterface*>(other);
    if (! output) {
        Logger::log().logf(Logger::Error, "InputPortInterface",
                           "InputPort %s could not connect to %s: not an Output port.",
                           getName().c_str(), other->getName().c_str());
        return false;
    }
    return output->createConnection(*this, policy);
}

bool InputPortInterface::connectTo(PortInterface* other)
{
    return connectTo(other, default_policy);
}

bool InputPortInterface::addConnection(ConnID* cid, ChannelElementBase::shared_ptr channel, const ConnPolicy& policy)
{
    if (!prepareConnectionChange()) return false;
    return cmanager.addConnection( cid, channel, policy);
}

#ifndef ORO_SIGNALLING_PORTS
void InputPortInterface::signal()
{
    if (iface && msignal_interface)
        iface->dataOnPort(this);
}

void InputPortInterface::signalInterface(bool true_false)
{
    msignal_interface = true_false;
}
#endif

FlowStatus InputPortInterface::receive(DataSourceBase::shared_ptr, bool)
{ throw std::runtime_error("calling default InputPortInterface::receive(datasource) implementation"); }

/** Returns true if this port is connected */
bool InputPortInterface::connected() const
{
    return getEndpoint()->connected() || hasMemberConnections();
}

void InputPortInterface::traceRead([[maybe_unused]] RTT::FlowStatus status)
{
    tracepoint(orocos_rtt, InputPort_read, status, getFullName().c_str());
}

void InputPortInterface::disconnect()
{
    if (!prepareConnectionChange()) return;
    disconnectMemberConnections();
    cmanager.disconnect();
}

bool InputPortInterface::disconnect(PortInterface* port)
{
    if (!prepareConnectionChange() || (port && !port->prepareConnectionChange())) return false;
    const bool mapped = disconnectMemberConnections(port);
    return cmanager.disconnect(port) || mapped;
}

bool InputPortInterface::createConnection( internal::SharedConnectionBase::shared_ptr shared_connection, ConnPolicy const& policy )
{
    if (hasMemberConnections() || !prepareConnectionChange()) return false;
    return internal::ConnFactory::createSharedConnection(0, this, shared_connection, policy);
}

base::ChannelElementBase::shared_ptr InputPortInterface::buildRemoteChannelOutput(
                base::OutputPortInterface&,
                types::TypeInfo const*,
                base::InputPortInterface&, const ConnPolicy&)
{
    return base::ChannelElementBase::shared_ptr();
}
