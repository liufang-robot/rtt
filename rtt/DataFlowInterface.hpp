/***************************************************************************
  tag: Peter Soetens  Thu Mar 2 08:30:18 CET 2006  DataFlowInterface.hpp

                        DataFlowInterface.hpp -  description
                           -------------------
    begin                : Thu March 02 2006
    copyright            : (C) 2006 Peter Soetens
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


#ifndef ORO_EXECUTION_DATA_FLOW_INTERFACE_HPP
#define ORO_EXECUTION_DATA_FLOW_INTERFACE_HPP

#include <vector>
#include <string>
#include "base/InputPortInterface.hpp"
#include "base/OutputPortInterface.hpp"
#include "rtt-fwd.hpp"

namespace RTT
{

    /**
     * The Interface of a TaskContext which exposes its data-flow ports.
     * @ingroup Ports
     * @ingroup Services
     * @ingroup RTTComponentInterface
     */
    class RTT_API DataFlowInterface
    {
    public:
        /**
         * A sequence of pointers to ports.
         */
        typedef std::vector<base::PortInterface*> Ports;

        /**
         * A sequence of names of ports.
         */
        typedef std::vector<std::string> PortNames;

        /**
         * Construct the DataFlow interface of a Service.
         * @param parent Owning service used to register ports in the component cycle.
         */
        DataFlowInterface(Service* parent = 0 );

        ~DataFlowInterface();

        /**
         * Name and register a cyclic Port in this interface.
         * @param name The name to give to the port.
         * @param port The port to add.
         */
        base::PortInterface& addPort(const std::string& name, base::PortInterface& port) {
            if ( !chkPtr("addPort", name, &port) ) return port;
            port.setName(name);
            return addPort(port);
        }

        /**
         * Register a cyclic Port. An existing same-name port is replaced;
         * real services are independent and are preserved.
         * @param port The port to add.
         * @return \a port
         */
        base::PortInterface& addPort(base::PortInterface& port);

        /**
         * Remove a Port and its connections from this interface.
         * @param name The port to remove.
         */
        void removePort(const std::string& name);

        /**
         * Get all ports of this interface.
         * @return A sequence of pointers to ports.
         */
        Ports getPorts() const;

        /**
         * Get all port names of this interface.
         * @return A sequence of strings containing the port names.
         * @deprecated by getNames()
         */
        PortNames getPortNames() const;

        /**
         * Get an added port.
         * @param name The port name
         * @return a pointer to a port or null if it does not exist.
         */
        base::PortInterface* getPort(const std::string& name) const;

        /**
         * Get the description of an added Port.
         *
         * @param name The port name
         *
         * @return The description or "" if it does not exist.
         */
        std::string getPortDescription(const std::string& name) const;

        /**
         * Set the documentation stored in an added port.
         * @return true if the port exists.
         */
        bool setPortDescription(const std::string& name, const std::string description);

        /**
         * Returns the component this interface belongs to.
         */
        TaskContext* getOwner() const;
        Service* getServiceInterface() const { return mservice; }

        /**
         * Returns the service this interface belongs to.
         * The returned service is a service living in the component
         * returned by getOwner() or in one of its sub-services.
         */
        Service* getService() const { return mservice; }

        /**
         * Registration alias for addPort(); every registered port participates
         * in its owner's cyclic I/O, and neither registration creates a service.
         */
        base::PortInterface& addLocalPort(base::PortInterface& port);

        /**
         * Removal alias for removePort().
         */
        void removeLocalPort(const std::string& name);


        /**
         * Get a port of a specific type.
         */
        template< class Type>
        Type* getPortType(const std::string& name) {
            return dynamic_cast<Type*>( this->getPort(name) );
        }

        /**
         * Remove all registered ports and their connections.
         */
        void clear();

    protected:

        bool chkPtr(const std::string &where, const std::string& name, const void* ptr);
        /**
         * All our ports.
         */
        Ports mports;
        /**
         * The parent Service. May be null in exceptional cases.
         */
        Service* mservice;

    };

}

#endif
