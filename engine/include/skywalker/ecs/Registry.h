#pragma once
// A compact sparse-set ECS registry (in the spirit of EnTT).
//
//   * Entities are (index, generation) pairs: stale handles are detected safely.
//   * Each component type lives in its own densely packed pool, so iterating a
//     component is a linear walk over contiguous memory (cache friendly).
//   * Add/remove are O(1) via swap-and-pop.
//
// The registry is intentionally low level and knows nothing about names, JSON or
// agents; `sky::Scene` layers stable IDs, hierarchy and reflection on top.

#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace sky::ecs {

struct Entity {
    static constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();
    uint32_t index = kInvalidIndex;
    uint32_t generation = 0;

    bool isNull() const { return index == kInvalidIndex; }
    bool operator==(const Entity&) const = default;
};

namespace detail {
inline uint32_t nextTypeId() {
    static uint32_t counter = 0;
    return counter++;
}
}  // namespace detail

/// Process-wide dense id per component type.
template <typename T>
uint32_t typeId() {
    static const uint32_t id = detail::nextTypeId();
    return id;
}

class PoolBase {
public:
    virtual ~PoolBase() = default;
    virtual void remove(uint32_t index) = 0;
    virtual bool has(uint32_t index) const = 0;
    virtual size_t size() const = 0;
    virtual void clear() = 0;
};

template <typename T>
class Pool final : public PoolBase {
public:
    static constexpr uint32_t kAbsent = std::numeric_limits<uint32_t>::max();

    template <typename... Args>
    T& emplace(uint32_t index, Args&&... args) {
        if (index >= sparse_.size()) sparse_.resize(index + 1, kAbsent);
        if (sparse_[index] != kAbsent) {
            dense_[sparse_[index]] = T{std::forward<Args>(args)...};
            return dense_[sparse_[index]];
        }
        sparse_[index] = static_cast<uint32_t>(dense_.size());
        owners_.push_back(index);
        dense_.push_back(T{std::forward<Args>(args)...});
        return dense_.back();
    }

    T* get(uint32_t index) {
        if (index >= sparse_.size() || sparse_[index] == kAbsent) return nullptr;
        return &dense_[sparse_[index]];
    }
    const T* get(uint32_t index) const { return const_cast<Pool*>(this)->get(index); }

    bool has(uint32_t index) const override { return index < sparse_.size() && sparse_[index] != kAbsent; }

    void remove(uint32_t index) override {
        if (!has(index)) return;
        uint32_t pos = sparse_[index];
        uint32_t last = static_cast<uint32_t>(dense_.size() - 1);
        if (pos != last) {
            dense_[pos] = std::move(dense_[last]);
            owners_[pos] = owners_[last];
            sparse_[owners_[pos]] = pos;
        }
        dense_.pop_back();
        owners_.pop_back();
        sparse_[index] = kAbsent;
    }

    size_t size() const override { return dense_.size(); }
    void clear() override {
        sparse_.clear();
        owners_.clear();
        dense_.clear();
    }

    const std::vector<uint32_t>& owners() const { return owners_; }
    std::vector<T>& data() { return dense_; }

private:
    std::vector<uint32_t> sparse_;  // entity index -> dense position
    std::vector<uint32_t> owners_;  // dense position -> entity index
    std::vector<T> dense_;
};

class Registry {
public:
    Entity create() {
        if (!freeList_.empty()) {
            uint32_t index = freeList_.back();
            freeList_.pop_back();
            alive_[index] = true;
            ++aliveCount_;
            return {index, generations_[index]};
        }
        auto index = static_cast<uint32_t>(generations_.size());
        generations_.push_back(0);
        alive_.push_back(true);
        ++aliveCount_;
        return {index, 0};
    }

    void destroy(Entity e) {
        if (!valid(e)) return;
        for (auto& pool : pools_) {
            if (pool) pool->remove(e.index);
        }
        alive_[e.index] = false;
        ++generations_[e.index];  // invalidates outstanding handles
        freeList_.push_back(e.index);
        --aliveCount_;
    }

    bool valid(Entity e) const {
        return e.index < generations_.size() && alive_[e.index] && generations_[e.index] == e.generation;
    }

    size_t alive() const { return aliveCount_; }

    template <typename T, typename... Args>
    T& emplace(Entity e, Args&&... args) {
        assert(valid(e));
        return pool<T>().emplace(e.index, std::forward<Args>(args)...);
    }

    template <typename T>
    T* get(Entity e) {
        if (!valid(e)) return nullptr;
        auto* p = findPool<T>();
        return p ? p->get(e.index) : nullptr;
    }

    template <typename T>
    const T* get(Entity e) const {
        return const_cast<Registry*>(this)->get<T>(e);
    }

    template <typename T>
    bool has(Entity e) const {
        if (!valid(e)) return false;
        auto* p = const_cast<Registry*>(this)->findPool<T>();
        return p && p->has(e.index);
    }

    template <typename T>
    void remove(Entity e) {
        if (!valid(e)) return;
        if (auto* p = findPool<T>()) p->remove(e.index);
    }

    /// Calls fn(Entity, First&, Rest&...) for every entity owning all listed components.
    /// Iterates the pool of `First`; put the rarest component first for best speed.
    template <typename First, typename... Rest, typename Fn>
    void each(Fn&& fn) {
        auto* first = findPool<First>();
        if (!first) return;
        const auto& owners = first->owners();
        for (size_t i = 0; i < owners.size(); ++i) {
            uint32_t index = owners[i];
            Entity e{index, generations_[index]};
            if constexpr (sizeof...(Rest) == 0) {
                fn(e, first->data()[i]);
            } else {
                if ((hasIndex<Rest>(index) && ...)) fn(e, first->data()[i], *findPool<Rest>()->get(index)...);
            }
        }
    }

    template <typename T>
    size_t count() {
        auto* p = findPool<T>();
        return p ? p->size() : 0;
    }

    void clear() {
        for (auto& pool : pools_) {
            if (pool) pool->clear();
        }
        generations_.clear();
        alive_.clear();
        freeList_.clear();
        aliveCount_ = 0;
    }

private:
    template <typename T>
    Pool<T>& pool() {
        uint32_t id = typeId<T>();
        if (id >= pools_.size()) pools_.resize(id + 1);
        if (!pools_[id]) pools_[id] = std::make_unique<Pool<T>>();
        return static_cast<Pool<T>&>(*pools_[id]);
    }

    template <typename T>
    Pool<T>* findPool() {
        uint32_t id = typeId<T>();
        if (id >= pools_.size() || !pools_[id]) return nullptr;
        return static_cast<Pool<T>*>(pools_[id].get());
    }

    template <typename T>
    bool hasIndex(uint32_t index) {
        auto* p = findPool<T>();
        return p && p->has(index);
    }

    std::vector<std::unique_ptr<PoolBase>> pools_;
    std::vector<uint32_t> generations_;
    std::vector<bool> alive_;
    std::vector<uint32_t> freeList_;
    size_t aliveCount_ = 0;
};

}  // namespace sky::ecs
