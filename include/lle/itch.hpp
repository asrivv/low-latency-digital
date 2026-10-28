#pragma once

#include <cstddef>
#include <cstdint>

#include "lle/common.hpp"

// ITCH 5.0 subset. As with OUCH, the packed *Wire structs only pin the layout.
namespace lle::itch {

#define LLE_OFF(W, f) offsetof(W, f)

namespace wire {
struct [[gnu::packed]] Header {
    char type; std::uint16_t locate; std::uint16_t tracking; char timestamp[6];
};
struct [[gnu::packed]] SystemEvent {
    Header h; char event;
};
struct [[gnu::packed]] StockDirectory {
    Header h; char stock[8]; char market_category; char financial_status; std::uint32_t round_lot;
    char round_lots_only; char issue_classification; char issue_subtype[2]; char authenticity;
    char short_sale_threshold; char ipo_flag; char luld_tier; char etp_flag; std::uint32_t etp_leverage;
    char inverse;
};
struct [[gnu::packed]] AddOrder {
    Header h; std::uint64_t ref; char side; std::uint32_t shares; char stock[8]; std::uint32_t price;
};
struct [[gnu::packed]] OrderExecuted {
    Header h; std::uint64_t ref; std::uint32_t shares; std::uint64_t match;
};
struct [[gnu::packed]] OrderCancel {
    Header h; std::uint64_t ref; std::uint32_t shares;
};
struct [[gnu::packed]] OrderDelete {
    Header h; std::uint64_t ref;
};
struct [[gnu::packed]] OrderReplace {
    Header h; std::uint64_t orig_ref; std::uint64_t new_ref; std::uint32_t shares; std::uint32_t price;
};
}  // namespace wire

inline constexpr std::size_t kHeaderSize = 11;
inline constexpr std::size_t kSystemEventSize = 12;
inline constexpr std::size_t kStockDirectorySize = 39;
inline constexpr std::size_t kAddOrderSize = 36;
inline constexpr std::size_t kOrderExecutedSize = 31;
inline constexpr std::size_t kOrderCancelSize = 23;
inline constexpr std::size_t kOrderDeleteSize = 19;
inline constexpr std::size_t kOrderReplaceSize = 35;
inline constexpr std::size_t kMaxMessageSize = kStockDirectorySize;

static_assert(sizeof(wire::Header) == kHeaderSize);
static_assert(sizeof(wire::SystemEvent) == kSystemEventSize);
static_assert(sizeof(wire::StockDirectory) == kStockDirectorySize);
static_assert(sizeof(wire::AddOrder) == kAddOrderSize);
static_assert(sizeof(wire::OrderExecuted) == kOrderExecutedSize);
static_assert(sizeof(wire::OrderCancel) == kOrderCancelSize);
static_assert(sizeof(wire::OrderDelete) == kOrderDeleteSize);
static_assert(sizeof(wire::OrderReplace) == kOrderReplaceSize);

struct Header {
    char type;
    std::uint16_t locate;
    std::uint16_t tracking;
    std::uint64_t timestamp;
};

struct SystemEvent {
    std::uint64_t timestamp;
    char event;
};

struct StockDirectory {
    std::uint16_t locate;
    std::uint64_t timestamp;
    std::uint64_t stock;
    char market_category = 'Q';
    char financial_status = 'N';
    std::uint32_t round_lot = 100;
    char round_lots_only = 'N';
    char issue_classification = 'C';
    char issue_subtype[2] = {'Z', ' '};
    char authenticity = 'P';
    char short_sale_threshold = 'N';
    char ipo_flag = 'N';
    char luld_tier = '1';
    char etp_flag = 'N';
    std::uint32_t etp_leverage = 0;
    char inverse = 'N';
};

struct AddOrder {
    std::uint16_t locate;
    std::uint64_t timestamp;
    std::uint64_t ref;
    char side;
    std::uint32_t shares;
    std::uint64_t stock;
    std::uint32_t price;
};

struct OrderExecuted {
    std::uint16_t locate;
    std::uint64_t timestamp;
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint64_t match;
};

struct OrderCancel {
    std::uint16_t locate;
    std::uint64_t timestamp;
    std::uint64_t ref;
    std::uint32_t shares;
};

struct OrderDelete {
    std::uint16_t locate;
    std::uint64_t timestamp;
    std::uint64_t ref;
};

struct OrderReplace {
    std::uint16_t locate;
    std::uint64_t timestamp;
    std::uint64_t orig_ref;
    std::uint64_t new_ref;
    std::uint32_t shares;
    std::uint32_t price;
};

// Tracking number is always 0.
inline void encode_header(std::byte* p, char type, std::uint16_t locate, std::uint64_t ts) noexcept {
    using W = wire::Header;
    store_char(p, type);
    store_be16(p + LLE_OFF(W, locate), locate);
    store_be16(p + LLE_OFF(W, tracking), 0);
    store_be48(p + LLE_OFF(W, timestamp), ts);
}

inline Header decode_header(const std::byte* p) noexcept {
    using W = wire::Header;
    return {load_char(p), load_be16(p + LLE_OFF(W, locate)), load_be16(p + LLE_OFF(W, tracking)),
            load_be48(p + LLE_OFF(W, timestamp))};
}

inline std::size_t encode(const SystemEvent& m, std::byte* p) noexcept {
    encode_header(p, 'S', 0, m.timestamp);
    store_char(p + LLE_OFF(wire::SystemEvent, event), m.event);
    return kSystemEventSize;
}

inline SystemEvent decode_system_event(const std::byte* p) noexcept {
    return {decode_header(p).timestamp, load_char(p + LLE_OFF(wire::SystemEvent, event))};
}

inline std::size_t encode(const StockDirectory& m, std::byte* p) noexcept {
    using W = wire::StockDirectory;
    encode_header(p, 'R', m.locate, m.timestamp);
    store_be64(p + LLE_OFF(W, stock), m.stock);
    store_char(p + LLE_OFF(W, market_category), m.market_category);
    store_char(p + LLE_OFF(W, financial_status), m.financial_status);
    store_be32(p + LLE_OFF(W, round_lot), m.round_lot);
    store_char(p + LLE_OFF(W, round_lots_only), m.round_lots_only);
    store_char(p + LLE_OFF(W, issue_classification), m.issue_classification);
    store_char(p + LLE_OFF(W, issue_subtype), m.issue_subtype[0]);
    store_char(p + LLE_OFF(W, issue_subtype) + 1, m.issue_subtype[1]);
    store_char(p + LLE_OFF(W, authenticity), m.authenticity);
    store_char(p + LLE_OFF(W, short_sale_threshold), m.short_sale_threshold);
    store_char(p + LLE_OFF(W, ipo_flag), m.ipo_flag);
    store_char(p + LLE_OFF(W, luld_tier), m.luld_tier);
    store_char(p + LLE_OFF(W, etp_flag), m.etp_flag);
    store_be32(p + LLE_OFF(W, etp_leverage), m.etp_leverage);
    store_char(p + LLE_OFF(W, inverse), m.inverse);
    return kStockDirectorySize;
}

inline StockDirectory decode_stock_directory(const std::byte* p) noexcept {
    using W = wire::StockDirectory;
    Header h = decode_header(p);
    StockDirectory m;
    m.locate = h.locate;
    m.timestamp = h.timestamp;
    m.stock = load_be64(p + LLE_OFF(W, stock));
    m.market_category = load_char(p + LLE_OFF(W, market_category));
    m.financial_status = load_char(p + LLE_OFF(W, financial_status));
    m.round_lot = load_be32(p + LLE_OFF(W, round_lot));
    m.round_lots_only = load_char(p + LLE_OFF(W, round_lots_only));
    m.issue_classification = load_char(p + LLE_OFF(W, issue_classification));
    m.issue_subtype[0] = load_char(p + LLE_OFF(W, issue_subtype));
    m.issue_subtype[1] = load_char(p + LLE_OFF(W, issue_subtype) + 1);
    m.authenticity = load_char(p + LLE_OFF(W, authenticity));
    m.short_sale_threshold = load_char(p + LLE_OFF(W, short_sale_threshold));
    m.ipo_flag = load_char(p + LLE_OFF(W, ipo_flag));
    m.luld_tier = load_char(p + LLE_OFF(W, luld_tier));
    m.etp_flag = load_char(p + LLE_OFF(W, etp_flag));
    m.etp_leverage = load_be32(p + LLE_OFF(W, etp_leverage));
    m.inverse = load_char(p + LLE_OFF(W, inverse));
    return m;
}

inline std::size_t encode(const AddOrder& m, std::byte* p) noexcept {
    using W = wire::AddOrder;
    encode_header(p, 'A', m.locate, m.timestamp);
    store_be64(p + LLE_OFF(W, ref), m.ref);
    store_char(p + LLE_OFF(W, side), m.side);
    store_be32(p + LLE_OFF(W, shares), m.shares);
    store_be64(p + LLE_OFF(W, stock), m.stock);
    store_be32(p + LLE_OFF(W, price), m.price);
    return kAddOrderSize;
}

inline AddOrder decode_add(const std::byte* p) noexcept {
    using W = wire::AddOrder;
    Header h = decode_header(p);
    return {h.locate,
            h.timestamp,
            load_be64(p + LLE_OFF(W, ref)),
            load_char(p + LLE_OFF(W, side)),
            load_be32(p + LLE_OFF(W, shares)),
            load_be64(p + LLE_OFF(W, stock)),
            load_be32(p + LLE_OFF(W, price))};
}

inline std::size_t encode(const OrderExecuted& m, std::byte* p) noexcept {
    using W = wire::OrderExecuted;
    encode_header(p, 'E', m.locate, m.timestamp);
    store_be64(p + LLE_OFF(W, ref), m.ref);
    store_be32(p + LLE_OFF(W, shares), m.shares);
    store_be64(p + LLE_OFF(W, match), m.match);
    return kOrderExecutedSize;
}

inline OrderExecuted decode_executed(const std::byte* p) noexcept {
    using W = wire::OrderExecuted;
    Header h = decode_header(p);
    return {h.locate, h.timestamp, load_be64(p + LLE_OFF(W, ref)), load_be32(p + LLE_OFF(W, shares)),
            load_be64(p + LLE_OFF(W, match))};
}

inline std::size_t encode(const OrderCancel& m, std::byte* p) noexcept {
    using W = wire::OrderCancel;
    encode_header(p, 'X', m.locate, m.timestamp);
    store_be64(p + LLE_OFF(W, ref), m.ref);
    store_be32(p + LLE_OFF(W, shares), m.shares);
    return kOrderCancelSize;
}

inline OrderCancel decode_cancel(const std::byte* p) noexcept {
    using W = wire::OrderCancel;
    Header h = decode_header(p);
    return {h.locate, h.timestamp, load_be64(p + LLE_OFF(W, ref)), load_be32(p + LLE_OFF(W, shares))};
}

inline std::size_t encode(const OrderDelete& m, std::byte* p) noexcept {
    encode_header(p, 'D', m.locate, m.timestamp);
    store_be64(p + LLE_OFF(wire::OrderDelete, ref), m.ref);
    return kOrderDeleteSize;
}

inline OrderDelete decode_delete(const std::byte* p) noexcept {
    Header h = decode_header(p);
    return {h.locate, h.timestamp, load_be64(p + LLE_OFF(wire::OrderDelete, ref))};
}

inline std::size_t encode(const OrderReplace& m, std::byte* p) noexcept {
    using W = wire::OrderReplace;
    encode_header(p, 'U', m.locate, m.timestamp);
    store_be64(p + LLE_OFF(W, orig_ref), m.orig_ref);
    store_be64(p + LLE_OFF(W, new_ref), m.new_ref);
    store_be32(p + LLE_OFF(W, shares), m.shares);
    store_be32(p + LLE_OFF(W, price), m.price);
    return kOrderReplaceSize;
}

inline OrderReplace decode_replace(const std::byte* p) noexcept {
    using W = wire::OrderReplace;
    Header h = decode_header(p);
    return {h.locate,
            h.timestamp,
            load_be64(p + LLE_OFF(W, orig_ref)),
            load_be64(p + LLE_OFF(W, new_ref)),
            load_be32(p + LLE_OFF(W, shares)),
            load_be32(p + LLE_OFF(W, price))};
}

inline std::size_t message_size(char type) noexcept {
    switch (type) {
        case 'S': return kSystemEventSize;
        case 'R': return kStockDirectorySize;
        case 'A': return kAddOrderSize;
        case 'E': return kOrderExecutedSize;
        case 'X': return kOrderCancelSize;
        case 'D': return kOrderDeleteSize;
        case 'U': return kOrderReplaceSize;
        default: return 0;
    }
}

#undef LLE_OFF

}  // namespace lle::itch
