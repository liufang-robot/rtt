/***************************************************************************
  tag: Peter Soetens  Thu Oct 22 11:59:07 CEST 2009  InputPortInterface.hpp

                        InputPortInterface.hpp -  description
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


#ifndef ORO_INPUT_PORT_INTERFACE_HPP
#define ORO_INPUT_PORT_INTERFACE_HPP

#include <string>
#include <atomic>
#include <vector>
#include "PortInterface.hpp"
#include "ChannelElement.hpp"
#include "../internal/rtt-internal-fwd.hpp"
#include "../internal/ConnectionManager.hpp"
#ifdef ORO_SIGNALLING_PORTS
#include "../internal/Signal.hpp"
#endif
#include "../base/DataSourceBase.hpp"

namespace RTT { namespace internal { class PortDataAccess; } }

namespace RTT
{ namespace base {


    /**
     * The base class of the InputPort. It contains the connection management code, which is
     * independent of the actual data being transmitted.
     */
    class RTT_API InputPortInterface : public PortInterface
    {
#ifdef ORO_SIGNALLING_PORTS
    public:
        typedef internal::Signal<void(PortInterface*)> NewDataOnPortEvent;
        typedef NewDataOnPortEvent::SlotFunction SlotFunction;
#endif

    private:
        friend class internal::PortDataAccess;
        virtual DataSourceBase::shared_ptr imageSource() { return {}; }
        virtual FlowStatus refreshImage() { return NoData; }
        virtual FlowStatus receive(DataSourceBase::shared_ptr source, bool copy_old_data);
        virtual void setImageStatus(FlowStatus value) { image_status_.store(value); }
    protected:
        std::atomic<FlowStatus> image_status_{NoData};
        ConnPolicy        default_policy;
#ifdef ORO_SIGNALLING_PORTS
        NewDataOnPortEvent* new_data_on_port_event;
#else
        bool msignal_interface;
        /**
         * The ConnOutputEndpoint signals that new data is available
         */
        void signal();
#endif

        void traceRead(RTT::FlowStatus status);
        InputPortInterface(const InputPortInterface& orig);
    public:

        /** One logical writer of this input, copied for inspection. */
        struct SourceConnection {
            std::string sourcePort;       //!< Qualified registered output name; empty if unavailable.
            std::string sourceMember;     //!< Canonical selector; empty means the whole output.
            std::string destinationMember; //!< Canonical selector; empty means this whole input.
        };
        typedef std::vector<SourceConnection> SourceConnections;

        /**
         * Describe current whole-port and member connections without reading data.
         * Entries are sorted by destination selector, source port and source selector.
         * An opaque transport connection has an empty sourcePort, rather than an
         * invented local endpoint. Returned strings remain valid after disconnect.
         *
         * This allocates inspection data and is not a realtime operation. It may
         * run alongside component cycles with a frozen graph. As with port/service
         * traversal, callers must serialize it with topology changes and destruction.
         */
        SourceConnections getSourceConnections() const;

        InputPortInterface(std::string const& name, ConnPolicy const& default_policy = ConnPolicy());

        virtual ~InputPortInterface();

        /** Clears the connection. After call to read() will return false after
         * clear() has been called
         */
        virtual void clear() = 0;

        ConnPolicy getDefaultPolicy() const;

        virtual bool addConnection(internal::ConnID* port_id, ChannelElementBase::shared_ptr channel, ConnPolicy const& policy);

        /** Returns a DataSourceBase interface to read this port. The returned
         * data source is always a new object.
         */
        virtual DataSourceBase* getDataSource() = 0;

        /** Freshness of the image prepared for this cycle; no channel access. */
        FlowStatus status() const noexcept { return image_status_.load(std::memory_order_acquire); }

        /** Removes any connection that either go to or come from this port
         *  *and* removes all callbacks and cleans up the NewDataOnPortEvent.
         */
        virtual void disconnect();

        /** Removes the channel that connects this port to \c port.
         *  All other ports or callbacks remain unaffected.
         */
        virtual bool disconnect(PortInterface* port);


        /** Returns true if this port is connected */
        virtual bool connected() const;

#ifdef ORO_SIGNALLING_PORTS
        /** Returns the event object that gets emitted when new data is
         * available for this port. It gets deleted when the port is deleted.
         */
        NewDataOnPortEvent* getNewDataOnPortEvent();
#else
        /** When called with \b true, will signal the DataFlowInterface when
         * new data is available.
         */
        void signalInterface(bool true_false);
#endif

        virtual bool connectTo(PortInterface* other, ConnPolicy const& policy);

        virtual bool connectTo(PortInterface* other);

        /**
         * Connects the port to an existing shared connection instance.
         */
        virtual bool createConnection( internal::SharedConnectionBase::shared_ptr shared_connection, ConnPolicy const& policy = ConnPolicy() );

        /** This method is analoguous to the static ConnFactory::buildChannelOutput.
         * It is provided for remote connection building: for these connections,
         * no template can be used and therefore the connection setup should be
         * done based on the types::TypeInfo object
         */
        virtual base::ChannelElementBase::shared_ptr buildRemoteChannelOutput(
                base::OutputPortInterface& output_port,
                types::TypeInfo const* type_info,
                base::InputPortInterface& input, const ConnPolicy& policy);
    };

}}

#endif
