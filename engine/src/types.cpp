#include "lob/types.hpp"

namespace lob {

const char* to_string(Result r) noexcept {
    switch (r) {
        case Result::Ok: return "Ok";
        case Result::DuplicateOrderId: return "DuplicateOrderId";
        case Result::UnknownOrderId: return "UnknownOrderId";
        case Result::InvalidSide: return "InvalidSide";
        case Result::InvalidPrice: return "InvalidPrice";
        case Result::InvalidQuantity: return "InvalidQuantity";
        case Result::InvalidMessageType: return "InvalidMessageType";
        case Result::PoolExhausted: return "PoolExhausted";
    }
    return "Unknown";
}

}  // namespace lob
