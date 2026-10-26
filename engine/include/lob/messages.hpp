#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "lob/types.hpp"

namespace lob {

// Fixed-size binary wire messages, ITCH-style: one byte of message type, then
// little-endian fields at fixed offsets. Both structs are trivially copyable,
// so a future feed parser can memcpy (or reinterpret) straight off the wire.

enum class MsgType : std::uint8_t {
    AddLimit = 'A',
    AddMarket = 'M',
    Cancel = 'X',
    Modify = 'U',  // reduce remaining quantity to `qty`
};

struct InboundMsg {
    MsgType type;            // 0
    Side side;               // 1   (AddLimit, AddMarket)
    std::uint8_t reserved[2];
    Qty qty;                 // 4   (AddLimit, AddMarket, Modify)
    OrderId id;              // 8
    Price price;             // 16  (AddLimit)
    std::uint64_t timestamp; // 24  sender's clock, echoed in every reply
};
static_assert(sizeof(InboundMsg) == 32);
static_assert(offsetof(InboundMsg, qty) == 4);
static_assert(offsetof(InboundMsg, id) == 8);
static_assert(offsetof(InboundMsg, price) == 16);
static_assert(offsetof(InboundMsg, timestamp) == 24);
static_assert(std::is_trivially_copyable_v<InboundMsg> && std::is_standard_layout_v<InboundMsg>);

enum class OutType : std::uint8_t {
    Ack = 'K',   // one per inbound message, after its fills
    Fill = 'F',
};

struct OutboundMsg {
    OutType type;            // 0
    Result result;           // 1   (Ack)
    std::uint8_t reserved[2];
    Qty qty;                 // 4   (Fill)
    OrderId id;              // 8   Ack: the order; Fill: the taker
    OrderId maker_id;        // 16  (Fill)
    Price price;             // 24  (Fill)
    std::uint64_t timestamp; // 32  copied from the inbound message
};
static_assert(sizeof(OutboundMsg) == 40);
static_assert(std::is_trivially_copyable_v<OutboundMsg> && std::is_standard_layout_v<OutboundMsg>);

inline InboundMsg make_add_limit(OrderId id, Side side, Price price, Qty qty) noexcept {
    return {MsgType::AddLimit, side, {}, qty, id, price, 0};
}
inline InboundMsg make_add_market(OrderId id, Side side, Qty qty) noexcept {
    return {MsgType::AddMarket, side, {}, qty, id, 0, 0};
}
inline InboundMsg make_cancel(OrderId id) noexcept { return {MsgType::Cancel, Side::Buy, {}, 0, id, 0, 0}; }
inline InboundMsg make_modify(OrderId id, Qty qty) noexcept { return {MsgType::Modify, Side::Buy, {}, qty, id, 0, 0}; }

// Applies one inbound message to anything with the Book interface.
template <class BookT>
Result apply(BookT& book, const InboundMsg& msg) noexcept {
    switch (msg.type) {
        case MsgType::AddLimit: return book.add_limit(msg.id, msg.side, msg.price, msg.qty);
        case MsgType::AddMarket: return book.add_market(msg.id, msg.side, msg.qty);
        case MsgType::Cancel: return book.cancel(msg.id);
        case MsgType::Modify: return book.modify(msg.id, msg.qty);
    }
    return Result::InvalidMessageType;
}

}  // namespace lob
