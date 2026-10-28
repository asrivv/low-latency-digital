#pragma once

#include <cstddef>
#include <cstdint>

#include "lle/common.hpp"

// OUCH 4.2. The [[gnu::packed]] *Wire structs only define the layout: sizes are
// static_asserted and encode/decode use offsetof into a byte buffer.
namespace lle::ouch {

#define LLE_OFF(W, f) offsetof(W, f)

namespace wire {
struct [[gnu::packed]] EnterOrder {
    char type; char token[14]; char side; std::uint32_t shares; char stock[8]; std::uint32_t price;
    std::uint32_t tif; char firm[4]; char display; char capacity; char iso; std::uint32_t min_qty;
    char cross_type; char customer_type;
};
struct [[gnu::packed]] ReplaceOrder {
    char type; char existing[14]; char replacement[14]; std::uint32_t shares; std::uint32_t price;
    std::uint32_t tif; char display; char iso; std::uint32_t min_qty;
};
struct [[gnu::packed]] CancelOrder {
    char type; char token[14]; std::uint32_t shares;
};
struct [[gnu::packed]] SystemEvent {
    char type; std::uint64_t timestamp; char event;
};
struct [[gnu::packed]] Accepted {
    char type; std::uint64_t timestamp; char token[14]; char side; std::uint32_t shares; char stock[8];
    std::uint32_t price; std::uint32_t tif; char firm[4]; char display; std::uint64_t ref; char capacity;
    char iso; std::uint32_t min_qty; char cross_type; char state; char bbo_weight;
};
struct [[gnu::packed]] Replaced {
    char type; std::uint64_t timestamp; char token[14]; char side; std::uint32_t shares; char stock[8];
    std::uint32_t price; std::uint32_t tif; char firm[4]; char display; std::uint64_t ref; char capacity;
    char iso; std::uint32_t min_qty; char cross_type; char state; char previous[14]; char bbo_weight;
};
struct [[gnu::packed]] Canceled {
    char type; std::uint64_t timestamp; char token[14]; std::uint32_t decrement; char reason;
};
struct [[gnu::packed]] Executed {
    char type; std::uint64_t timestamp; char token[14]; std::uint32_t shares; std::uint32_t price;
    char liquidity; std::uint64_t match;
};
struct [[gnu::packed]] Rejected {
    char type; std::uint64_t timestamp; char token[14]; char reason;
};
}  // namespace wire

inline constexpr std::size_t kEnterOrderSize = 49;
inline constexpr std::size_t kReplaceOrderSize = 47;
inline constexpr std::size_t kCancelOrderSize = 19;
inline constexpr std::size_t kSystemEventSize = 10;
inline constexpr std::size_t kAcceptedSize = 66;
inline constexpr std::size_t kReplacedSize = 80;
inline constexpr std::size_t kCanceledSize = 28;
inline constexpr std::size_t kExecutedSize = 40;
inline constexpr std::size_t kRejectedSize = 24;
inline constexpr std::size_t kMaxOutboundSize = kReplacedSize;

static_assert(sizeof(wire::EnterOrder) == kEnterOrderSize);
static_assert(sizeof(wire::ReplaceOrder) == kReplaceOrderSize);
static_assert(sizeof(wire::CancelOrder) == kCancelOrderSize);
static_assert(sizeof(wire::SystemEvent) == kSystemEventSize);
static_assert(sizeof(wire::Accepted) == kAcceptedSize);
static_assert(sizeof(wire::Replaced) == kReplacedSize);
static_assert(sizeof(wire::Canceled) == kCanceledSize);
static_assert(sizeof(wire::Executed) == kExecutedSize);
static_assert(sizeof(wire::Rejected) == kRejectedSize);

inline constexpr std::uint32_t kTifIoc = 0;

struct EnterOrder {
    Token token;
    char side;
    std::uint32_t shares;
    std::uint64_t stock;
    std::uint32_t price;
    std::uint32_t tif;
    std::uint32_t firm;
    char display;
    char capacity;
    char iso;
    std::uint32_t min_qty;
    char cross_type;
    char customer_type;
};

struct ReplaceOrder {
    Token existing;
    Token replacement;
    std::uint32_t shares;
    std::uint32_t price;
    std::uint32_t tif;
    char display;
    char iso;
    std::uint32_t min_qty;
};

struct CancelOrder {
    Token token;
    std::uint32_t shares;
};

struct SystemEvent {
    std::uint64_t timestamp;
    char event;
};

struct Accepted {
    std::uint64_t timestamp;
    Token token;
    char side;
    std::uint32_t shares;
    std::uint64_t stock;
    std::uint32_t price;
    std::uint32_t tif;
    std::uint32_t firm;
    char display;
    std::uint64_t ref;
    char capacity;
    char iso;
    std::uint32_t min_qty;
    char cross_type;
    char state;
    char bbo_weight;
};

struct Replaced {
    std::uint64_t timestamp;
    Token token;
    char side;
    std::uint32_t shares;
    std::uint64_t stock;
    std::uint32_t price;
    std::uint32_t tif;
    std::uint32_t firm;
    char display;
    std::uint64_t ref;
    char capacity;
    char iso;
    std::uint32_t min_qty;
    char cross_type;
    char state;
    Token previous;
    char bbo_weight;
};

struct Canceled {
    std::uint64_t timestamp;
    Token token;
    std::uint32_t decrement;
    char reason;
};

struct Executed {
    std::uint64_t timestamp;
    Token token;
    std::uint32_t shares;
    std::uint32_t price;
    char liquidity;
    std::uint64_t match;
};

struct Rejected {
    std::uint64_t timestamp;
    Token token;
    char reason;
};

inline std::size_t encode(const EnterOrder& m, std::byte* p) noexcept {
    using W = wire::EnterOrder;
    store_char(p, 'O');
    m.token.store(p + LLE_OFF(W, token));
    store_char(p + LLE_OFF(W, side), m.side);
    store_be32(p + LLE_OFF(W, shares), m.shares);
    store_be64(p + LLE_OFF(W, stock), m.stock);
    store_be32(p + LLE_OFF(W, price), m.price);
    store_be32(p + LLE_OFF(W, tif), m.tif);
    store_be32(p + LLE_OFF(W, firm), m.firm);
    store_char(p + LLE_OFF(W, display), m.display);
    store_char(p + LLE_OFF(W, capacity), m.capacity);
    store_char(p + LLE_OFF(W, iso), m.iso);
    store_be32(p + LLE_OFF(W, min_qty), m.min_qty);
    store_char(p + LLE_OFF(W, cross_type), m.cross_type);
    store_char(p + LLE_OFF(W, customer_type), m.customer_type);
    return kEnterOrderSize;
}

inline EnterOrder decode_enter(const std::byte* p) noexcept {
    using W = wire::EnterOrder;
    EnterOrder m;
    m.token.load(p + LLE_OFF(W, token));
    m.side = load_char(p + LLE_OFF(W, side));
    m.shares = load_be32(p + LLE_OFF(W, shares));
    m.stock = load_be64(p + LLE_OFF(W, stock));
    m.price = load_be32(p + LLE_OFF(W, price));
    m.tif = load_be32(p + LLE_OFF(W, tif));
    m.firm = load_be32(p + LLE_OFF(W, firm));
    m.display = load_char(p + LLE_OFF(W, display));
    m.capacity = load_char(p + LLE_OFF(W, capacity));
    m.iso = load_char(p + LLE_OFF(W, iso));
    m.min_qty = load_be32(p + LLE_OFF(W, min_qty));
    m.cross_type = load_char(p + LLE_OFF(W, cross_type));
    m.customer_type = load_char(p + LLE_OFF(W, customer_type));
    return m;
}

inline std::size_t encode(const ReplaceOrder& m, std::byte* p) noexcept {
    using W = wire::ReplaceOrder;
    store_char(p, 'U');
    m.existing.store(p + LLE_OFF(W, existing));
    m.replacement.store(p + LLE_OFF(W, replacement));
    store_be32(p + LLE_OFF(W, shares), m.shares);
    store_be32(p + LLE_OFF(W, price), m.price);
    store_be32(p + LLE_OFF(W, tif), m.tif);
    store_char(p + LLE_OFF(W, display), m.display);
    store_char(p + LLE_OFF(W, iso), m.iso);
    store_be32(p + LLE_OFF(W, min_qty), m.min_qty);
    return kReplaceOrderSize;
}

inline ReplaceOrder decode_replace(const std::byte* p) noexcept {
    using W = wire::ReplaceOrder;
    ReplaceOrder m;
    m.existing.load(p + LLE_OFF(W, existing));
    m.replacement.load(p + LLE_OFF(W, replacement));
    m.shares = load_be32(p + LLE_OFF(W, shares));
    m.price = load_be32(p + LLE_OFF(W, price));
    m.tif = load_be32(p + LLE_OFF(W, tif));
    m.display = load_char(p + LLE_OFF(W, display));
    m.iso = load_char(p + LLE_OFF(W, iso));
    m.min_qty = load_be32(p + LLE_OFF(W, min_qty));
    return m;
}

inline std::size_t encode(const CancelOrder& m, std::byte* p) noexcept {
    using W = wire::CancelOrder;
    store_char(p, 'X');
    m.token.store(p + LLE_OFF(W, token));
    store_be32(p + LLE_OFF(W, shares), m.shares);
    return kCancelOrderSize;
}

inline CancelOrder decode_cancel(const std::byte* p) noexcept {
    using W = wire::CancelOrder;
    CancelOrder m;
    m.token.load(p + LLE_OFF(W, token));
    m.shares = load_be32(p + LLE_OFF(W, shares));
    return m;
}

inline std::size_t encode(const SystemEvent& m, std::byte* p) noexcept {
    using W = wire::SystemEvent;
    store_char(p, 'S');
    store_be64(p + LLE_OFF(W, timestamp), m.timestamp);
    store_char(p + LLE_OFF(W, event), m.event);
    return kSystemEventSize;
}

inline SystemEvent decode_system_event(const std::byte* p) noexcept {
    using W = wire::SystemEvent;
    return {load_be64(p + LLE_OFF(W, timestamp)), load_char(p + LLE_OFF(W, event))};
}

inline std::size_t encode(const Accepted& m, std::byte* p) noexcept {
    using W = wire::Accepted;
    store_char(p, 'A');
    store_be64(p + LLE_OFF(W, timestamp), m.timestamp);
    m.token.store(p + LLE_OFF(W, token));
    store_char(p + LLE_OFF(W, side), m.side);
    store_be32(p + LLE_OFF(W, shares), m.shares);
    store_be64(p + LLE_OFF(W, stock), m.stock);
    store_be32(p + LLE_OFF(W, price), m.price);
    store_be32(p + LLE_OFF(W, tif), m.tif);
    store_be32(p + LLE_OFF(W, firm), m.firm);
    store_char(p + LLE_OFF(W, display), m.display);
    store_be64(p + LLE_OFF(W, ref), m.ref);
    store_char(p + LLE_OFF(W, capacity), m.capacity);
    store_char(p + LLE_OFF(W, iso), m.iso);
    store_be32(p + LLE_OFF(W, min_qty), m.min_qty);
    store_char(p + LLE_OFF(W, cross_type), m.cross_type);
    store_char(p + LLE_OFF(W, state), m.state);
    store_char(p + LLE_OFF(W, bbo_weight), m.bbo_weight);
    return kAcceptedSize;
}

inline Accepted decode_accepted(const std::byte* p) noexcept {
    using W = wire::Accepted;
    Accepted m;
    m.timestamp = load_be64(p + LLE_OFF(W, timestamp));
    m.token.load(p + LLE_OFF(W, token));
    m.side = load_char(p + LLE_OFF(W, side));
    m.shares = load_be32(p + LLE_OFF(W, shares));
    m.stock = load_be64(p + LLE_OFF(W, stock));
    m.price = load_be32(p + LLE_OFF(W, price));
    m.tif = load_be32(p + LLE_OFF(W, tif));
    m.firm = load_be32(p + LLE_OFF(W, firm));
    m.display = load_char(p + LLE_OFF(W, display));
    m.ref = load_be64(p + LLE_OFF(W, ref));
    m.capacity = load_char(p + LLE_OFF(W, capacity));
    m.iso = load_char(p + LLE_OFF(W, iso));
    m.min_qty = load_be32(p + LLE_OFF(W, min_qty));
    m.cross_type = load_char(p + LLE_OFF(W, cross_type));
    m.state = load_char(p + LLE_OFF(W, state));
    m.bbo_weight = load_char(p + LLE_OFF(W, bbo_weight));
    return m;
}

inline std::size_t encode(const Replaced& m, std::byte* p) noexcept {
    using W = wire::Replaced;
    store_char(p, 'U');
    store_be64(p + LLE_OFF(W, timestamp), m.timestamp);
    m.token.store(p + LLE_OFF(W, token));
    store_char(p + LLE_OFF(W, side), m.side);
    store_be32(p + LLE_OFF(W, shares), m.shares);
    store_be64(p + LLE_OFF(W, stock), m.stock);
    store_be32(p + LLE_OFF(W, price), m.price);
    store_be32(p + LLE_OFF(W, tif), m.tif);
    store_be32(p + LLE_OFF(W, firm), m.firm);
    store_char(p + LLE_OFF(W, display), m.display);
    store_be64(p + LLE_OFF(W, ref), m.ref);
    store_char(p + LLE_OFF(W, capacity), m.capacity);
    store_char(p + LLE_OFF(W, iso), m.iso);
    store_be32(p + LLE_OFF(W, min_qty), m.min_qty);
    store_char(p + LLE_OFF(W, cross_type), m.cross_type);
    store_char(p + LLE_OFF(W, state), m.state);
    m.previous.store(p + LLE_OFF(W, previous));
    store_char(p + LLE_OFF(W, bbo_weight), m.bbo_weight);
    return kReplacedSize;
}

inline Replaced decode_replaced(const std::byte* p) noexcept {
    using W = wire::Replaced;
    Replaced m;
    m.timestamp = load_be64(p + LLE_OFF(W, timestamp));
    m.token.load(p + LLE_OFF(W, token));
    m.side = load_char(p + LLE_OFF(W, side));
    m.shares = load_be32(p + LLE_OFF(W, shares));
    m.stock = load_be64(p + LLE_OFF(W, stock));
    m.price = load_be32(p + LLE_OFF(W, price));
    m.tif = load_be32(p + LLE_OFF(W, tif));
    m.firm = load_be32(p + LLE_OFF(W, firm));
    m.display = load_char(p + LLE_OFF(W, display));
    m.ref = load_be64(p + LLE_OFF(W, ref));
    m.capacity = load_char(p + LLE_OFF(W, capacity));
    m.iso = load_char(p + LLE_OFF(W, iso));
    m.min_qty = load_be32(p + LLE_OFF(W, min_qty));
    m.cross_type = load_char(p + LLE_OFF(W, cross_type));
    m.state = load_char(p + LLE_OFF(W, state));
    m.previous.load(p + LLE_OFF(W, previous));
    m.bbo_weight = load_char(p + LLE_OFF(W, bbo_weight));
    return m;
}

inline std::size_t encode(const Canceled& m, std::byte* p) noexcept {
    using W = wire::Canceled;
    store_char(p, 'C');
    store_be64(p + LLE_OFF(W, timestamp), m.timestamp);
    m.token.store(p + LLE_OFF(W, token));
    store_be32(p + LLE_OFF(W, decrement), m.decrement);
    store_char(p + LLE_OFF(W, reason), m.reason);
    return kCanceledSize;
}

inline Canceled decode_canceled(const std::byte* p) noexcept {
    using W = wire::Canceled;
    Canceled m;
    m.timestamp = load_be64(p + LLE_OFF(W, timestamp));
    m.token.load(p + LLE_OFF(W, token));
    m.decrement = load_be32(p + LLE_OFF(W, decrement));
    m.reason = load_char(p + LLE_OFF(W, reason));
    return m;
}

inline std::size_t encode(const Executed& m, std::byte* p) noexcept {
    using W = wire::Executed;
    store_char(p, 'E');
    store_be64(p + LLE_OFF(W, timestamp), m.timestamp);
    m.token.store(p + LLE_OFF(W, token));
    store_be32(p + LLE_OFF(W, shares), m.shares);
    store_be32(p + LLE_OFF(W, price), m.price);
    store_char(p + LLE_OFF(W, liquidity), m.liquidity);
    store_be64(p + LLE_OFF(W, match), m.match);
    return kExecutedSize;
}

inline Executed decode_executed(const std::byte* p) noexcept {
    using W = wire::Executed;
    Executed m;
    m.timestamp = load_be64(p + LLE_OFF(W, timestamp));
    m.token.load(p + LLE_OFF(W, token));
    m.shares = load_be32(p + LLE_OFF(W, shares));
    m.price = load_be32(p + LLE_OFF(W, price));
    m.liquidity = load_char(p + LLE_OFF(W, liquidity));
    m.match = load_be64(p + LLE_OFF(W, match));
    return m;
}

inline std::size_t encode(const Rejected& m, std::byte* p) noexcept {
    using W = wire::Rejected;
    store_char(p, 'J');
    store_be64(p + LLE_OFF(W, timestamp), m.timestamp);
    m.token.store(p + LLE_OFF(W, token));
    store_char(p + LLE_OFF(W, reason), m.reason);
    return kRejectedSize;
}

inline Rejected decode_rejected(const std::byte* p) noexcept {
    using W = wire::Rejected;
    Rejected m;
    m.timestamp = load_be64(p + LLE_OFF(W, timestamp));
    m.token.load(p + LLE_OFF(W, token));
    m.reason = load_char(p + LLE_OFF(W, reason));
    return m;
}

inline std::size_t inbound_size(char type) noexcept {
    switch (type) {
        case 'O': return kEnterOrderSize;
        case 'U': return kReplaceOrderSize;
        case 'X': return kCancelOrderSize;
        default: return 0;
    }
}

inline std::size_t outbound_size(char type) noexcept {
    switch (type) {
        case 'S': return kSystemEventSize;
        case 'A': return kAcceptedSize;
        case 'U': return kReplacedSize;
        case 'C': return kCanceledSize;
        case 'E': return kExecutedSize;
        case 'J': return kRejectedSize;
        default: return 0;
    }
}

#undef LLE_OFF

}  // namespace lle::ouch
