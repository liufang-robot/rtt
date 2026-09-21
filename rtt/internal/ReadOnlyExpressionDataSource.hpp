#ifndef RTT_INTERNAL_READ_ONLY_EXPRESSION_DATA_SOURCE_HPP
#define RTT_INTERNAL_READ_ONLY_EXPRESSION_DATA_SOURCE_HPP
#include "ObservationPath.hpp"
#include "PartDataSource.hpp"
#include <memory>
namespace RTT { namespace internal {
template<class T> struct ObservationCache {
    T cached{};
    void store(const T& value) { cached = value; }
    const T& read() const { return cached; }
};
/** carray is a non-owning handle: own both the immutable sample and a reusable
 * presentation buffer so even its mutable address() cannot alter later reads. */
template<class T> struct ObservationCache<types::carray<T>> {
    std::unique_ptr<CopyConstructedBuffer<T>> cached;
    mutable std::unique_ptr<CopyConstructedBuffer<T>> presentation;
    mutable types::carray<T> view;
    static void update(CopyConstructedBuffer<T>& buffer, const T* source) {
        if constexpr (std::is_copy_assignable<T>::value) buffer.copyAssign(source);
        else buffer.reconstruct(source);
    }
    void store(types::carray<T> value) {
        if (!cached || cached->size() != value.count()) {
            cached.reset(new CopyConstructedBuffer<T>(value.address(), value.count()));
            presentation.reset(new CopyConstructedBuffer<T>(value.address(), value.count()));
        } else update(*cached, value.address());
    }
    const types::carray<T>& read() const {
        if (cached) {
            update(*presentation, cached->data());
            view.init(presentation->data(), presentation->size());
        }
        return view;
    }
};
template<class T> class ReadOnlyExpressionDataSource : public DataSource<T>, public ObservationExpression {
    ObservationPath::shared_ptr path;
    bool live;
    mutable bool present = false;
    mutable ObservationCache<T> cache;
    bool refresh() const {
        auto selected = path->resolve(&present);
        auto typed = dynamic_cast<DataSource<T>*>(selected.get());
        if (!typed) return false;
        cache.store(typed->get());
        return present;
    }
public:
    ReadOnlyExpressionDataSource(ObservationPath::shared_ptr selection, bool refreshing)
        : path(std::move(selection)), live(refreshing) { refresh(); }
    bool evaluate() const override { return live ? refresh() : present; }
    typename DataSource<T>::result_t get() const override {
        if (!evaluate()) throw ObservationUnavailable();
        return cache.read();
    }
    typename DataSource<T>::result_t value() const override {
        if (!present) throw ObservationUnavailable();
        return cache.read();
    }
    typename DataSource<T>::const_reference_t rvalue() const override {
        if (!evaluate()) throw ObservationUnavailable();
        return cache.read();
    }
    base::DataSourceBase::shared_ptr frozen() const override {
        return live ? new ReadOnlyExpressionDataSource(path, false) : clone();
    }
    base::DataSourceBase::shared_ptr getMember(const std::string& name) override {
        return live ? path->member(name) : base::DataSourceBase::getMember(name);
    }
    base::DataSourceBase::shared_ptr getMember(base::DataSourceBase::shared_ptr index,
                                             base::DataSourceBase::shared_ptr offset) override {
        return live && !offset ? path->member(index) : base::DataSourceBase::getMember(index, offset);
    }
    ReadOnlyExpressionDataSource* clone() const override {
        auto result = new ReadOnlyExpressionDataSource(path, live);
        if (!live) { result->cache.store(cache.read()); result->present = present; }
        return result;
    }
    ReadOnlyExpressionDataSource* copy(std::map<const base::DataSourceBase*, base::DataSourceBase*>& replacements) const override {
        if (replacements[this]) return static_cast<ReadOnlyExpressionDataSource*>(replacements[this]);
        auto result = new ReadOnlyExpressionDataSource(path->copy(replacements), live);
        if (!live) { result->cache.store(cache.read()); result->present = present; }
        replacements[this] = result;
        return result;
    }
};
}}
#endif
