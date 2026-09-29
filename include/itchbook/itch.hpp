// Zero-copy decoder for Nasdaq TotalView-ITCH 5.0.
//
// Implemented from "Nasdaq TotalView-ITCH 5.0" (revision log up to
// February 19, 2026), which includes Operational Halt ('h') and Direct Listing
// with Capital Raise ('O'). Every message struct is a view over the raw bytes:
// it holds one pointer and decodes a field only when its accessor is called.
// All integers are big-endian; timestamps are 6-byte nanoseconds since
// midnight; Price(4) fields are u32 with 4 implied decimals and Price(8) fields
// are u64 with 8 implied decimals.
//
// Dispatch is a switch on the type byte that calls handler.on(Msg) for the
// message types the handler accepts (detected with a requires-expression), so
// the hot path has no virtual calls and unhandled types compile to nothing.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace itchbook::itch {

// ---------------------------------------------------------------- decoding

[[nodiscard]] inline std::uint16_t be16(const char* p) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap16(v);
    return v;
}
[[nodiscard]] inline std::uint32_t be32(const char* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap32(v);
    return v;
}
[[nodiscard]] inline std::uint64_t be64(const char* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap64(v);
    return v;
}
// 6-byte big-endian integer (the ITCH timestamp).
[[nodiscard]] inline std::uint64_t be48(const char* p) noexcept {
    return (std::uint64_t{be16(p)} << 32) | be32(p + 2);
}

// Right-trims the space padding from an Alpha field.
[[nodiscard]] inline std::string_view trim_alpha(std::string_view s) noexcept {
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

// ---------------------------------------------------------------- messages

// Fields shared by every message: type (0), stock locate (1), tracking
// number (3), timestamp (5).
struct MessageView {
    const char* p;
    [[nodiscard]] char type() const noexcept { return p[0]; }
    [[nodiscard]] std::uint16_t stock_locate() const noexcept { return be16(p + 1); }
    [[nodiscard]] std::uint16_t tracking_number() const noexcept { return be16(p + 3); }
    [[nodiscard]] std::uint64_t timestamp() const noexcept { return be48(p + 5); }

protected:
    [[nodiscard]] std::string_view alpha(std::size_t off, std::size_t len) const noexcept {
        return {p + off, len};
    }
};

#define ITCHBOOK_MSG(Name, TypeChar, Size) \
    struct Name : MessageView {            \
        static constexpr char kType = TypeChar; \
        static constexpr std::size_t kSize = Size;

// 1.1 System Event
ITCHBOOK_MSG(SystemEvent, 'S', 12)
    [[nodiscard]] char event_code() const noexcept { return p[11]; }
};

// 1.2.1 Stock Directory
ITCHBOOK_MSG(StockDirectory, 'R', 39)
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(11, 8); }
    [[nodiscard]] char market_category() const noexcept { return p[19]; }
    [[nodiscard]] char financial_status_indicator() const noexcept { return p[20]; }
    [[nodiscard]] std::uint32_t round_lot_size() const noexcept { return be32(p + 21); }
    [[nodiscard]] char round_lots_only() const noexcept { return p[25]; }
    [[nodiscard]] char issue_classification() const noexcept { return p[26]; }
    [[nodiscard]] std::string_view issue_sub_type() const noexcept { return alpha(27, 2); }
    [[nodiscard]] char authenticity() const noexcept { return p[29]; }
    [[nodiscard]] char short_sale_threshold_indicator() const noexcept { return p[30]; }
    [[nodiscard]] char ipo_flag() const noexcept { return p[31]; }
    [[nodiscard]] char luld_reference_price_tier() const noexcept { return p[32]; }
    [[nodiscard]] char etp_flag() const noexcept { return p[33]; }
    [[nodiscard]] std::uint32_t etp_leverage_factor() const noexcept { return be32(p + 34); }
    [[nodiscard]] char inverse_indicator() const noexcept { return p[38]; }
};

// 1.2.2 Stock Trading Action
ITCHBOOK_MSG(StockTradingAction, 'H', 25)
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(11, 8); }
    [[nodiscard]] char trading_state() const noexcept { return p[19]; }
    [[nodiscard]] char reserved() const noexcept { return p[20]; }
    [[nodiscard]] std::string_view reason() const noexcept { return alpha(21, 4); }
};

// 1.2.3 Reg SHO Short Sale Price Test Restricted Indicator
ITCHBOOK_MSG(RegShoRestriction, 'Y', 20)
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(11, 8); }
    [[nodiscard]] char reg_sho_action() const noexcept { return p[19]; }
};

// 1.2.4 Market Participant Position
ITCHBOOK_MSG(MarketParticipantPosition, 'L', 26)
    [[nodiscard]] std::string_view mpid() const noexcept { return alpha(11, 4); }
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(15, 8); }
    [[nodiscard]] char primary_market_maker() const noexcept { return p[23]; }
    [[nodiscard]] char market_maker_mode() const noexcept { return p[24]; }
    [[nodiscard]] char market_participant_state() const noexcept { return p[25]; }
};

// 1.2.5.1 MWCB Decline Level (Price(8) fields)
ITCHBOOK_MSG(MwcbDeclineLevel, 'V', 35)
    [[nodiscard]] std::uint64_t level1() const noexcept { return be64(p + 11); }
    [[nodiscard]] std::uint64_t level2() const noexcept { return be64(p + 19); }
    [[nodiscard]] std::uint64_t level3() const noexcept { return be64(p + 27); }
};

// 1.2.5.2 MWCB Status
ITCHBOOK_MSG(MwcbStatus, 'W', 12)
    [[nodiscard]] char breached_level() const noexcept { return p[11]; }
};

// 1.2.6 IPO Quoting Period Update
ITCHBOOK_MSG(IpoQuotingPeriodUpdate, 'K', 28)
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(11, 8); }
    [[nodiscard]] std::uint32_t ipo_quotation_release_time() const noexcept { return be32(p + 19); }
    [[nodiscard]] char ipo_quotation_release_qualifier() const noexcept { return p[23]; }
    [[nodiscard]] std::uint32_t ipo_price() const noexcept { return be32(p + 24); }
};

// 1.2.7 LULD Auction Collar
ITCHBOOK_MSG(LuldAuctionCollar, 'J', 35)
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(11, 8); }
    [[nodiscard]] std::uint32_t auction_collar_reference_price() const noexcept { return be32(p + 19); }
    [[nodiscard]] std::uint32_t upper_auction_collar_price() const noexcept { return be32(p + 23); }
    [[nodiscard]] std::uint32_t lower_auction_collar_price() const noexcept { return be32(p + 27); }
    [[nodiscard]] std::uint32_t auction_collar_extension() const noexcept { return be32(p + 31); }
};

// 1.2.8 Operational Halt
ITCHBOOK_MSG(OperationalHalt, 'h', 21)
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(11, 8); }
    [[nodiscard]] char market_code() const noexcept { return p[19]; }
    [[nodiscard]] char operational_halt_action() const noexcept { return p[20]; }
};

// 1.3.1 Add Order, no MPID attribution
ITCHBOOK_MSG(AddOrder, 'A', 36)
    [[nodiscard]] std::uint64_t order_reference_number() const noexcept { return be64(p + 11); }
    [[nodiscard]] char buy_sell_indicator() const noexcept { return p[19]; }
    [[nodiscard]] std::uint32_t shares() const noexcept { return be32(p + 20); }
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(24, 8); }
    [[nodiscard]] std::uint32_t price() const noexcept { return be32(p + 32); }
};

// 1.3.2 Add Order with MPID attribution
ITCHBOOK_MSG(AddOrderMpid, 'F', 40)
    [[nodiscard]] std::uint64_t order_reference_number() const noexcept { return be64(p + 11); }
    [[nodiscard]] char buy_sell_indicator() const noexcept { return p[19]; }
    [[nodiscard]] std::uint32_t shares() const noexcept { return be32(p + 20); }
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(24, 8); }
    [[nodiscard]] std::uint32_t price() const noexcept { return be32(p + 32); }
    [[nodiscard]] std::string_view attribution() const noexcept { return alpha(36, 4); }
};

// 1.4.1 Order Executed
ITCHBOOK_MSG(OrderExecuted, 'E', 31)
    [[nodiscard]] std::uint64_t order_reference_number() const noexcept { return be64(p + 11); }
    [[nodiscard]] std::uint32_t executed_shares() const noexcept { return be32(p + 19); }
    [[nodiscard]] std::uint64_t match_number() const noexcept { return be64(p + 23); }
};

// 1.4.2 Order Executed With Price
ITCHBOOK_MSG(OrderExecutedWithPrice, 'C', 36)
    [[nodiscard]] std::uint64_t order_reference_number() const noexcept { return be64(p + 11); }
    [[nodiscard]] std::uint32_t executed_shares() const noexcept { return be32(p + 19); }
    [[nodiscard]] std::uint64_t match_number() const noexcept { return be64(p + 23); }
    [[nodiscard]] char printable() const noexcept { return p[31]; }
    [[nodiscard]] std::uint32_t execution_price() const noexcept { return be32(p + 32); }
};

// 1.4.3 Order Cancel (partial)
ITCHBOOK_MSG(OrderCancel, 'X', 23)
    [[nodiscard]] std::uint64_t order_reference_number() const noexcept { return be64(p + 11); }
    [[nodiscard]] std::uint32_t cancelled_shares() const noexcept { return be32(p + 19); }
};

// 1.4.4 Order Delete
ITCHBOOK_MSG(OrderDelete, 'D', 19)
    [[nodiscard]] std::uint64_t order_reference_number() const noexcept { return be64(p + 11); }
};

// 1.4.5 Order Replace
ITCHBOOK_MSG(OrderReplace, 'U', 35)
    [[nodiscard]] std::uint64_t original_order_reference_number() const noexcept { return be64(p + 11); }
    [[nodiscard]] std::uint64_t new_order_reference_number() const noexcept { return be64(p + 19); }
    [[nodiscard]] std::uint32_t shares() const noexcept { return be32(p + 27); }
    [[nodiscard]] std::uint32_t price() const noexcept { return be32(p + 31); }
};

// 1.5.1 Trade (non-cross), executions of non-displayed orders
ITCHBOOK_MSG(Trade, 'P', 44)
    [[nodiscard]] std::uint64_t order_reference_number() const noexcept { return be64(p + 11); }
    [[nodiscard]] char buy_sell_indicator() const noexcept { return p[19]; }
    [[nodiscard]] std::uint32_t shares() const noexcept { return be32(p + 20); }
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(24, 8); }
    [[nodiscard]] std::uint32_t price() const noexcept { return be32(p + 32); }
    [[nodiscard]] std::uint64_t match_number() const noexcept { return be64(p + 36); }
};

// 1.5.2 Cross Trade (shares is 8 bytes here)
ITCHBOOK_MSG(CrossTrade, 'Q', 40)
    [[nodiscard]] std::uint64_t shares() const noexcept { return be64(p + 11); }
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(19, 8); }
    [[nodiscard]] std::uint32_t cross_price() const noexcept { return be32(p + 27); }
    [[nodiscard]] std::uint64_t match_number() const noexcept { return be64(p + 31); }
    [[nodiscard]] char cross_type() const noexcept { return p[39]; }
};

// 1.5.3 Broken Trade / Order Execution
ITCHBOOK_MSG(BrokenTrade, 'B', 19)
    [[nodiscard]] std::uint64_t match_number() const noexcept { return be64(p + 11); }
};

// 1.6 Net Order Imbalance Indicator
ITCHBOOK_MSG(Noii, 'I', 50)
    [[nodiscard]] std::uint64_t paired_shares() const noexcept { return be64(p + 11); }
    [[nodiscard]] std::uint64_t imbalance_shares() const noexcept { return be64(p + 19); }
    [[nodiscard]] char imbalance_direction() const noexcept { return p[27]; }
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(28, 8); }
    [[nodiscard]] std::uint32_t far_price() const noexcept { return be32(p + 36); }
    [[nodiscard]] std::uint32_t near_price() const noexcept { return be32(p + 40); }
    [[nodiscard]] std::uint32_t current_reference_price() const noexcept { return be32(p + 44); }
    [[nodiscard]] char cross_type() const noexcept { return p[48]; }
    [[nodiscard]] char price_variation_indicator() const noexcept { return p[49]; }
};

// 1.7 Retail Price Improvement Indicator
ITCHBOOK_MSG(Rpii, 'N', 20)
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(11, 8); }
    [[nodiscard]] char interest_flag() const noexcept { return p[19]; }
};

// 1.8 Direct Listing with Capital Raise Price Discovery
ITCHBOOK_MSG(Dlcr, 'O', 48)
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(11, 8); }
    [[nodiscard]] char open_eligibility_status() const noexcept { return p[19]; }
    [[nodiscard]] std::uint32_t minimum_allowable_price() const noexcept { return be32(p + 20); }
    [[nodiscard]] std::uint32_t maximum_allowable_price() const noexcept { return be32(p + 24); }
    [[nodiscard]] std::uint32_t near_execution_price() const noexcept { return be32(p + 28); }
    [[nodiscard]] std::uint64_t near_execution_time() const noexcept { return be64(p + 32); }
    [[nodiscard]] std::uint32_t lower_price_range_collar() const noexcept { return be32(p + 40); }
    [[nodiscard]] std::uint32_t upper_price_range_collar() const noexcept { return be32(p + 44); }
};

#undef ITCHBOOK_MSG

// Expected payload size for a type byte, 0 if the type is not in the spec.
[[nodiscard]] constexpr std::size_t message_size(char type) noexcept {
    switch (type) {
        case SystemEvent::kType: return SystemEvent::kSize;
        case StockDirectory::kType: return StockDirectory::kSize;
        case StockTradingAction::kType: return StockTradingAction::kSize;
        case RegShoRestriction::kType: return RegShoRestriction::kSize;
        case MarketParticipantPosition::kType: return MarketParticipantPosition::kSize;
        case MwcbDeclineLevel::kType: return MwcbDeclineLevel::kSize;
        case MwcbStatus::kType: return MwcbStatus::kSize;
        case IpoQuotingPeriodUpdate::kType: return IpoQuotingPeriodUpdate::kSize;
        case LuldAuctionCollar::kType: return LuldAuctionCollar::kSize;
        case OperationalHalt::kType: return OperationalHalt::kSize;
        case AddOrder::kType: return AddOrder::kSize;
        case AddOrderMpid::kType: return AddOrderMpid::kSize;
        case OrderExecuted::kType: return OrderExecuted::kSize;
        case OrderExecutedWithPrice::kType: return OrderExecutedWithPrice::kSize;
        case OrderCancel::kType: return OrderCancel::kSize;
        case OrderDelete::kType: return OrderDelete::kSize;
        case OrderReplace::kType: return OrderReplace::kSize;
        case Trade::kType: return Trade::kSize;
        case CrossTrade::kType: return CrossTrade::kSize;
        case BrokenTrade::kType: return BrokenTrade::kSize;
        case Noii::kType: return Noii::kSize;
        case Rpii::kType: return Rpii::kSize;
        case Dlcr::kType: return Dlcr::kSize;
        default: return 0;
    }
}

// ---------------------------------------------------------------- dispatch

enum class Status : std::uint8_t { ok, unknown_type, bad_length };

namespace detail {
template <class M, class H>
[[gnu::always_inline]] inline Status deliver(const char* p, std::size_t len, H& h) {
    if (len != M::kSize) [[unlikely]] {
        if constexpr (requires { h.on_bad_length(p, len); }) h.on_bad_length(p, len);
        return Status::bad_length;
    }
    if constexpr (requires { h.on(M{{p}}); }) h.on(M{{p}});
    return Status::ok;
}
}  // namespace detail

// Decodes one message payload (without the 2-byte length prefix) and calls
// h.on(Msg) if the handler has an overload for that message type.
template <class H>
[[gnu::always_inline]] inline Status dispatch(const char* p, std::size_t len, H& h) {
    if (len == 0) [[unlikely]] {
        if constexpr (requires { h.on_bad_length(p, len); }) h.on_bad_length(p, len);
        return Status::bad_length;
    }
    using namespace detail;
    switch (p[0]) {
        // Order book messages first: they are about 95% of a trading day.
        case AddOrder::kType: return deliver<AddOrder>(p, len, h);
        case OrderDelete::kType: return deliver<OrderDelete>(p, len, h);
        case OrderReplace::kType: return deliver<OrderReplace>(p, len, h);
        case OrderExecuted::kType: return deliver<OrderExecuted>(p, len, h);
        case OrderCancel::kType: return deliver<OrderCancel>(p, len, h);
        case AddOrderMpid::kType: return deliver<AddOrderMpid>(p, len, h);
        case OrderExecutedWithPrice::kType: return deliver<OrderExecutedWithPrice>(p, len, h);
        case Trade::kType: return deliver<Trade>(p, len, h);
        case Noii::kType: return deliver<Noii>(p, len, h);
        case CrossTrade::kType: return deliver<CrossTrade>(p, len, h);
        case BrokenTrade::kType: return deliver<BrokenTrade>(p, len, h);
        case SystemEvent::kType: return deliver<SystemEvent>(p, len, h);
        case StockDirectory::kType: return deliver<StockDirectory>(p, len, h);
        case StockTradingAction::kType: return deliver<StockTradingAction>(p, len, h);
        case RegShoRestriction::kType: return deliver<RegShoRestriction>(p, len, h);
        case MarketParticipantPosition::kType: return deliver<MarketParticipantPosition>(p, len, h);
        case MwcbDeclineLevel::kType: return deliver<MwcbDeclineLevel>(p, len, h);
        case MwcbStatus::kType: return deliver<MwcbStatus>(p, len, h);
        case IpoQuotingPeriodUpdate::kType: return deliver<IpoQuotingPeriodUpdate>(p, len, h);
        case LuldAuctionCollar::kType: return deliver<LuldAuctionCollar>(p, len, h);
        case OperationalHalt::kType: return deliver<OperationalHalt>(p, len, h);
        case Rpii::kType: return deliver<Rpii>(p, len, h);
        case Dlcr::kType: return deliver<Dlcr>(p, len, h);
        default:
            if constexpr (requires { h.on_unknown(p, len); }) h.on_unknown(p, len);
            return Status::unknown_type;
    }
}

// Walks a buffer of length-prefixed messages (2-byte big-endian length, then
// the payload), dispatching each complete one. Returns the number of bytes
// consumed; a trailing partial message is left for the caller to carry over.
template <class H>
std::size_t dispatch_buffer(const char* buf, std::size_t n, H& h) {
    std::size_t off = 0;
    while (n - off >= 2) {
        const std::size_t len = be16(buf + off);
        if (n - off - 2 < len) break;
        dispatch(buf + off + 2, len, h);
        off += 2 + len;
    }
    return off;
}

}  // namespace itchbook::itch
