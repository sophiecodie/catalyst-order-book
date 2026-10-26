#pragma once

#include <rigtorp/SPSCQueue.h>

#include <atomic>
#include <cstdint>

#include "lob/book.hpp"
#include "lob/messages.hpp"

namespace lob {

using InboundQueue = rigtorp::SPSCQueue<InboundMsg>;
using OutboundQueue = rigtorp::SPSCQueue<OutboundMsg>;

// Single-threaded matching engine: drains an inbound SPSC ring, applies each
// message to the book, and publishes fills plus one ack per message on an
// outbound SPSC ring. The engine thread is the only consumer of `in` and the
// only producer of `out`; nothing is locked.
class Engine {
public:
    Engine(const BookConfig& config, InboundQueue& in, OutboundQueue& out)
        : publisher_{&out, 0}, book_(config, publisher_), in_(in), out_(out) {}

    // Processes at most one message. Returns false when the ring was empty.
    bool poll() noexcept {
        const InboundMsg* msg = in_.front();
        if (msg == nullptr) return false;
        process(*msg);
        in_.pop();
        return true;
    }

    void run(const std::atomic<bool>& stop) noexcept {
        while (!stop.load(std::memory_order_relaxed)) poll();
        while (poll()) {
        }
    }

    void process(const InboundMsg& msg) noexcept {
        publisher_.timestamp = msg.timestamp;
        const Result result = apply(book_, msg);
        out_.push(OutboundMsg{OutType::Ack, result, {}, 0, msg.id, 0, 0, msg.timestamp});
    }

    const auto& book() const noexcept { return book_; }

private:
    struct Publisher {
        OutboundQueue* out;
        std::uint64_t timestamp;
        void on_fill(const Fill& f) noexcept {
            out->push(OutboundMsg{OutType::Fill, Result::Ok, {}, f.qty, f.taker_id, f.maker_id, f.price, timestamp});
        }
    };

    Publisher publisher_;
    Book<Publisher> book_;
    InboundQueue& in_;
    OutboundQueue& out_;
};

}  // namespace lob
