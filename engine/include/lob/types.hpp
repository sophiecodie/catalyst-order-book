#pragma once

#include <concepts>
#include <cstdint>

namespace lob {

using Price = std::int64_t;     // ticks
using Qty = std::uint32_t;      // lots
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy = 'B', Sell = 'S' };

enum class Result : std::uint8_t {
    Ok,
    DuplicateOrderId,
    UnknownOrderId,
    InvalidSide,
    InvalidPrice,
    InvalidQuantity,
    InvalidMessageType,
    PoolExhausted,
};

const char* to_string(Result r) noexcept;

struct Fill {
    OrderId maker_id;
    OrderId taker_id;
    Price price;  // always the resting maker's price
    Qty qty;
};

// qty == 0 means that side of the book is empty.
struct TopOfBook {
    Price price;
    std::uint64_t qty;
};

// Fills are reported through a statically dispatched listener: the book is
// templated on it, so on_fill inlines and there is no virtual call.
template <class L>
concept FillListener = requires(L& listener, const Fill& fill) {
    { listener.on_fill(fill) } -> std::same_as<void>;
};

struct NullListener {
    void on_fill(const Fill&) noexcept {}
};

}  // namespace lob
