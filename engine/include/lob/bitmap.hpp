#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace lob {

// Two-level occupancy bitmap over price levels. Bit i of words_ is set when
// level i is non-empty; bit w of summary_ is set when words_[w] is non-zero.
// Finding the next non-empty level is one ctz/clz on the current word, then a
// short scan of the summary (num_levels / 4096 words) and one more ctz/clz.
class LevelBitmap {
public:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    explicit LevelBitmap(std::size_t bits)
        : num_words_((bits + 63) / 64),
          num_summary_((num_words_ + 63) / 64),
          words_(new std::uint64_t[num_words_]()),
          summary_(new std::uint64_t[num_summary_]()) {}

    bool test(std::size_t i) const noexcept { return (words_[i >> 6] >> (i & 63)) & 1; }

    void set(std::size_t i) noexcept {
        words_[i >> 6] |= bit(i);
        summary_[i >> 12] |= bit(i >> 6);
    }

    void clear(std::size_t i) noexcept {
        const std::size_t w = i >> 6;
        if ((words_[w] &= ~bit(i)) == 0) summary_[w >> 6] &= ~bit(w);
    }

    // Lowest set index >= i, or npos.
    std::size_t next_up(std::size_t i) const noexcept {
        std::size_t w = i >> 6;
        if (const std::uint64_t m = words_[w] & at_or_above(i)) return (w << 6) | low(m);
        if (++w >= num_words_) [[unlikely]] return npos;
        std::size_t s = w >> 6;
        std::uint64_t m = summary_[s] & at_or_above(w);
        while (!m) {
            if (++s >= num_summary_) return npos;
            m = summary_[s];
        }
        w = (s << 6) | low(m);
        return (w << 6) | low(words_[w]);
    }

    // Highest set index <= i, or npos.
    std::size_t next_down(std::size_t i) const noexcept {
        std::size_t w = i >> 6;
        if (const std::uint64_t m = words_[w] & at_or_below(i)) return (w << 6) | high(m);
        if (w-- == 0) [[unlikely]] return npos;
        std::size_t s = w >> 6;
        std::uint64_t m = summary_[s] & at_or_below(w);
        while (!m) {
            if (s-- == 0) return npos;
            m = summary_[s];
        }
        w = (s << 6) | high(m);
        return (w << 6) | high(words_[w]);
    }

    void reset() noexcept {
        for (std::size_t s = 0; s < num_summary_; ++s) {
            for (std::uint64_t m = summary_[s]; m; m &= m - 1) words_[(s << 6) | low(m)] = 0;
            summary_[s] = 0;
        }
    }

private:
    static std::uint64_t bit(std::size_t i) noexcept { return std::uint64_t{1} << (i & 63); }
    static std::uint64_t at_or_above(std::size_t i) noexcept { return ~std::uint64_t{0} << (i & 63); }
    static std::uint64_t at_or_below(std::size_t i) noexcept { return ~std::uint64_t{0} >> (63 - (i & 63)); }
    static std::size_t low(std::uint64_t m) noexcept { return static_cast<std::size_t>(std::countr_zero(m)); }
    static std::size_t high(std::uint64_t m) noexcept { return 63 - static_cast<std::size_t>(std::countl_zero(m)); }

    std::size_t num_words_;
    std::size_t num_summary_;
    std::unique_ptr<std::uint64_t[]> words_;
    std::unique_ptr<std::uint64_t[]> summary_;
};

}  // namespace lob
