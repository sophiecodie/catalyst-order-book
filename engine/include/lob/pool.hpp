#pragma once

#include <cstdint>
#include <memory>

namespace lob {

// Fixed-capacity object pool addressed by 32-bit handles. Handle 0 is a
// reserved null, so zero-initialised links mean "none". All storage is
// allocated (and touched) in the constructor; acquire/release never allocate.
template <class T>
class ObjectPool {
public:
    using Handle = std::uint32_t;
    static constexpr Handle kNull = 0;

    explicit ObjectPool(std::uint32_t capacity)
        : capacity_(capacity),
          slots_(new T[capacity + 1]()),
          free_(new Handle[capacity]) {
        reset();
    }

    // Returns kNull when the pool is exhausted.
    Handle acquire() noexcept {
        if (free_top_ == 0) [[unlikely]] return kNull;
        return free_[--free_top_];
    }
    void release(Handle h) noexcept { free_[free_top_++] = h; }

    void reset() noexcept {
        // Hand out low handles first so a fresh book walks memory in order.
        free_top_ = capacity_;
        for (std::uint32_t i = 0; i < capacity_; ++i) free_[i] = capacity_ - i;
    }

    T& operator[](Handle h) noexcept { return slots_[h]; }
    const T& operator[](Handle h) const noexcept { return slots_[h]; }
    Handle handle_of(const T* p) const noexcept { return static_cast<Handle>(p - slots_.get()); }

    bool full() const noexcept { return free_top_ == 0; }
    std::uint32_t in_use() const noexcept { return capacity_ - free_top_; }
    std::uint32_t capacity() const noexcept { return capacity_; }

private:
    std::uint32_t capacity_;
    std::uint32_t free_top_ = 0;
    std::unique_ptr<T[]> slots_;
    std::unique_ptr<Handle[]> free_;
};

}  // namespace lob
