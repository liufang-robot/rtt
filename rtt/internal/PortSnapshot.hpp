#ifndef RTT_INTERNAL_PORT_SNAPSHOT_HPP
#define RTT_INTERNAL_PORT_SNAPSHOT_HPP

#include "../base/DataObject.hpp"
#include "DataSource.hpp"
#include <atomic>
#include <cstdint>
#include <utility>
#include <mutex>
#include <thread>
#include <boost/shared_ptr.hpp>

namespace RTT { namespace internal {
/** Synchronized committed storage shared by a port and non-consuming observers.
 * Configuration may resize the sample only while its publishing owner is stopped.
 * Observers remain safe during that resize; they retain their previous snapshot. */
template<class T>
struct PortSnapshot {
    typename base::DataObjectInterface<T>::shared_ptr sample{new base::DataObject<T>(T{})};
    std::atomic<std::uint64_t> revision{0};
    std::atomic<bool> available{false};
private:
    mutable std::atomic<unsigned> readers_{0};
    std::atomic<bool> configuring_{false};
    std::mutex configuration_mutex_;
    struct Reader {
        std::atomic<unsigned>& readers;
        explicit Reader(std::atomic<unsigned>& count) : readers(count) { ++readers; }
        ~Reader() { --readers; }
    };
public:
    bool publish(const T& value) {
        if (!sample->Set(value)) return false;
        revision.fetch_add(1, std::memory_order_release);
        available.store(true, std::memory_order_release);
        return true;
    }
    bool copy(T& value, bool include_default = false) const {
        if (configuring_.load()) return false;
        Reader reading(readers_);
        if (configuring_.load()) return false;
        if (!available.load(std::memory_order_acquire)) {
            if (!include_default) return false;
            value = sample->data_sample();
        } else {
            sample->Get(value);
        }
        return true;
    }
    void clear() { available.store(false, std::memory_order_release); }
    void initialize(const T& value) {
        // Only configuration waits. Cyclic publication uses the existing
        // lock-free DataObject, and observers never wait for configuration.
        std::lock_guard<std::mutex> lock(configuration_mutex_);
        configuring_.store(true);
        while (readers_.load()) std::this_thread::yield();
        try {
            sample->data_sample(value);
            clear();
        } catch (...) {
            configuring_.store(false);
            throw;
        }
        configuring_.store(false);
    }
};

template<class T>
class PortSnapshotSource : public DataSource<T> {
    boost::shared_ptr<PortSnapshot<T>> snapshot_;
    bool include_default_;
    mutable T value_{};
    mutable std::uint64_t observed_{0};
public:
    explicit PortSnapshotSource(boost::shared_ptr<PortSnapshot<T>> snapshot, bool include_default = false)
        : snapshot_(std::move(snapshot)), include_default_(include_default) {
        snapshot_->copy(value_, include_default_);
    }
    bool evaluate() const override {
        // Capture the revision before copying: a concurrent publication may
        // be observed twice, but is never skipped by advancing past its sample.
        const auto revision = snapshot_->revision.load(std::memory_order_acquire);
        if (!snapshot_->copy(value_, include_default_)) return false;
        if (!snapshot_->available.load(std::memory_order_acquire)) return false;
        const bool changed = observed_ != revision;
        observed_ = revision;
        return changed;
    }
    typename DataSource<T>::result_t get() const override { return value_; }
    typename DataSource<T>::result_t value() const override { return value_; }
    typename DataSource<T>::const_reference_t rvalue() const override { return value_; }
    void reset() override { observed_ = 0; }
    PortSnapshotSource* clone() const override { return new PortSnapshotSource(snapshot_, include_default_); }
    PortSnapshotSource* copy(std::map<const base::DataSourceBase*, base::DataSourceBase*>&) const override {
        return new PortSnapshotSource(snapshot_, include_default_);
    }
};
}}
#endif
