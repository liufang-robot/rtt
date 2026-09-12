/***************************************************************************
  tag: Peter Soetens  Thu Oct 22 11:59:08 CEST 2009  InputPort.hpp

                        InputPort.hpp -  description
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


#ifndef ORO_INPUT_PORT_HPP
#define ORO_INPUT_PORT_HPP

#include "base/InputPortInterface.hpp"
#include "base/DataObject.hpp"
#include "internal/Channels.hpp"
#include "internal/DataSources.hpp"
#include "internal/PortSnapshot.hpp"
#include "Logger.hpp"
#include "Service.hpp"
#include "OutputPort.hpp"

namespace RTT {
/** A typed, component-owned cyclic input image. The execution engine refreshes
 * it before updateHook(); data() never consumes a transport sample. */
template<typename T>
class InputPort : public base::InputPortInterface {
    friend class internal::ConnOutputEndpoint<T>;
    friend class internal::PortDataAccess;
    typename internal::ConnOutputEndpoint<T>::shared_ptr endpoint;
    T image_{};
    typename internal::ReferenceDataSource<T>::shared_ptr image_source_;
    boost::shared_ptr<internal::PortSnapshot<T>> snapshot_;

    InputPort(const InputPort&) = delete;
    InputPort& operator=(const InputPort&) = delete;
    bool connectionAdded(base::ChannelElementBase::shared_ptr, const ConnPolicy&) { return true; }
    base::DataSourceBase::shared_ptr imageSource() override { return image_source_; }
    void setImageStatus(FlowStatus value) override {
        if (value == NewData) snapshot_->publish(image_);
        image_status_.store(value, std::memory_order_release);
    }
    FlowStatus refreshImage() override {
        FlowStatus result = receive(image_, false);
        setImageStatus(result);
        return result;
    }
    FlowStatus receive(T& value, bool copy_old_data = true) {
        FlowStatus result = getEndpoint()->getReadEndpoint()->read(value, copy_old_data);
        traceRead(result);
        return result;
    }
    FlowStatus receive(base::DataSourceBase::shared_ptr source, bool copy_old_data) override {
        auto target = boost::dynamic_pointer_cast<internal::AssignableDataSource<T>>(source);
        if (!target) return NoData;
        return receive(target->set(), copy_old_data);
    }
public:
    explicit InputPort(const std::string& name = "unnamed", const ConnPolicy& policy = ConnPolicy())
      : base::InputPortInterface(name, policy), endpoint(new internal::ConnOutputEndpoint<T>(this)),
        image_source_(new internal::ReferenceDataSource<T>(image_)), snapshot_(new internal::PortSnapshot<T>()) {}
    ~InputPort() override { preparePortDestruction(); disconnect(); }

    const T& data() const noexcept { return image_; }
    /** Observe the last prepared image without touching the incoming channel. */
    bool snapshot(T& value) const {
        return snapshot_->copy(value, true);
    }
    /** Initialize default values/capacity while the component is inactive. */
    void setDataSample(const T& value) {
        if (!prepareConnectionChange()) throw std::logic_error("input image is active");
        image_ = value;
        snapshot_->initialize(value);
        setImageStatus(NoData);
    }
    void clear() override {
        if (!prepareConnectionChange()) return;
        getEndpoint()->getReadEndpoint()->clear();
        setImageStatus(NoData);
    }
    void getDataSample(T& value) { value = getEndpoint()->getReadEndpoint()->data_sample(); }
    const types::TypeInfo* getTypeInfo() const override { return internal::DataSourceTypeInfo<T>::getTypeInfo(); }
    base::PortInterface* clone() const override { return new InputPort<T>(getName()); }
    base::PortInterface* antiClone() const override { return new OutputPort<T>(getName()); }
    base::DataSourceBase* getDataSource() override { return new internal::PortSnapshotSource<T>(snapshot_, true); }
    bool createStream(const ConnPolicy& policy) override {
        return prepareConnectionChange() && internal::ConnFactory::createStream(*this, policy);
    }
#ifndef ORO_DISABLE_PORT_DATA_SCRIPTING
    Service* createPortObject() override {
        Service* object = base::InputPortInterface::createPortObject();
        if (object) object->addSynchronousOperation("status", &InputPort::status, this)
            .doc("Observe freshness of the current cyclic input image.");
        return object;
    }
#endif
    internal::ConnOutputEndpoint<T>* getEndpoint() const override { return endpoint.get(); }
    typename base::ChannelElement<T>::shared_ptr getSharedBuffer() const { return endpoint->getSharedBuffer(); }
};
}
#endif
