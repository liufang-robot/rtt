/***************************************************************************
  tag: Peter Soetens  Thu Oct 22 11:59:08 CEST 2009  OutputPort.hpp

                        OutputPort.hpp -  description
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


#ifndef ORO_OUTPUT_PORT_HPP
#define ORO_OUTPUT_PORT_HPP

#include "base/OutputPortInterface.hpp"
#include "base/DataObject.hpp"
#include "internal/PortSnapshot.hpp"
#include "internal/DataSources.hpp"
#include "internal/Channels.hpp"
#include "internal/ConnFactory.hpp"
#include "Service.hpp"
#include <atomic>
#include <stdexcept>
#include "InputPort.hpp"

namespace RTT {
/** A typed cyclic output image. Components modify data(); only the runtime
 * publishes a completed image. Observers use synchronized committed snapshots. */
template<typename T>
class OutputPort : public base::OutputPortInterface {
    friend class internal::ConnInputEndpoint<T>;
    friend class internal::PortDataAccess;
    typename internal::ConnInputEndpoint<T>::shared_ptr endpoint;
    T image_{};
    typename internal::ReferenceDataSource<T>::shared_ptr image_source_;
    boost::shared_ptr<internal::PortSnapshot<T>> committed_;

    OutputPort(const OutputPort&) = delete;
    OutputPort& operator=(const OutputPort&) = delete;
    bool connectionAdded(base::ChannelElementBase::shared_ptr channel, const ConnPolicy& policy) override {
        auto typed = channel->template narrow<T>();
        T sample{};
        if (!snapshot(sample)) sample = image_;
        if (!typed || typed->data_sample(sample, false) == NotConnected) return false;
        return !policy.init || !committed_->available.load(std::memory_order_acquire) || typed->write(sample) != NotConnected;
    }
    base::DataSourceBase::shared_ptr imageSource() override { return image_source_; }
    WriteStatus commitImage() override { return publish(image_); }
    WriteStatus publish(const T& value) {
        // Slow observers may occupy every bounded snapshot slot. Their cache
        // can retain an older commit, but must never suppress channel delivery.
        const bool retained = committed_->publish(value);
        if (!connected()) return retained ? NotConnected : WriteFailure;
        traceWrite();
        return getEndpoint()->getWriteEndpoint()->write(value);
    }
    WriteStatus publish(base::DataSourceBase::shared_ptr source) override {
        auto value = boost::dynamic_pointer_cast<internal::DataSource<T>>(source);
        if (!value) return WriteFailure;
        value->evaluate();
        return publish(value->rvalue());
    }
public:
    explicit OutputPort(const std::string& name = "unnamed")
      : base::OutputPortInterface(name), endpoint(new internal::ConnInputEndpoint<T>(this)),
        image_source_(new internal::ReferenceDataSource<T>(image_)), committed_(new internal::PortSnapshot<T>()) {}
    ~OutputPort() override { preparePortDestruction(); disconnect(); }

    T& data() noexcept { return image_; }
    const T& data() const noexcept { return image_; }
    T snapshot() const { T value{}; snapshot(value); return value; }
    bool snapshot(T& value) const {
        return committed_->copy(value);
    }
    base::DataSourceBase::shared_ptr getDataSource() const override {
        return new internal::PortSnapshotSource<T>(committed_);
    }
    /** Prepare defaults and transport capacity while inactive; does not publish. */
    void setDataSample(const T& value) {
        if (!prepareConnectionChange()) throw std::logic_error("output image is active");
        image_ = value;
        committed_->initialize(value);
        if (connected()) getEndpoint()->getWriteEndpoint()->data_sample(value, true);
    }
    void clear() {
        if (!prepareConnectionChange()) return;
        committed_->clear();
        getEndpoint()->getWriteEndpoint()->clear();
        auto shared = cmanager.getSharedConnection();
        if (shared) shared->clear();
    }
    const types::TypeInfo* getTypeInfo() const override { return internal::DataSourceTypeInfo<T>::getTypeInfo(); }
    base::PortInterface* clone() const override { return new OutputPort<T>(getName()); }
    base::PortInterface* antiClone() const override { return new InputPort<T>(getName()); }
    using base::OutputPortInterface::createConnection;
    bool createConnection(base::InputPortInterface& input, const ConnPolicy& policy) override {
        if (input.hasMemberConnections() || !validateWholeConnection(input)) return false;
        if (!prepareConnectionChange() || !input.prepareConnectionChange()) return false;
        return internal::ConnFactory::createConnection(*this, input, policy);
    }
    bool createStream(const ConnPolicy& policy) override {
        return prepareConnectionChange() && internal::ConnFactory::createStream(*this, policy);
    }
#ifndef ORO_DISABLE_PORT_DATA_SCRIPTING
    Service* createPortObject() override {
        Service* object = base::OutputPortInterface::createPortObject();
        if (object) object->addSynchronousOperation("snapshot", static_cast<T (OutputPort::*)() const>(&OutputPort::snapshot), this)
            .doc("Observe the last committed cyclic output value.");
        return object;
    }
#endif
    internal::ConnInputEndpoint<T>* getEndpoint() const override { return endpoint.get(); }
    typename base::ChannelElement<T>::shared_ptr getSharedBuffer() const { return endpoint->getSharedBuffer(); }
};
}
#endif
