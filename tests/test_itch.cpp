// Decoder tests. Every buffer is built by hand from the offsets and lengths in
// the ITCH 5.0 spec tables (written here as literals, independently of the
// constants in itch.hpp), using a byte-by-byte big-endian writer that does not
// share code with the decoder. Field values use distinct bytes so that a wrong
// offset, a wrong width or a byte-order slip changes the decoded value.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "itchbook/itch.hpp"

using namespace itchbook::itch;

namespace {

struct Buf {
    std::vector<char> b;
    explicit Buf(std::size_t n) : b(n, '\0') {}
    // Writes value big-endian, most significant byte first, one byte at a time.
    Buf& be(std::size_t off, std::size_t width, std::uint64_t v) {
        REQUIRE(off + width <= b.size());
        for (std::size_t i = 0; i < width; ++i)
            b[off + i] = static_cast<char>((v >> (8 * (width - 1 - i))) & 0xFF);
        return *this;
    }
    Buf& ch(std::size_t off, char c) {
        REQUIRE(off < b.size());
        b[off] = c;
        return *this;
    }
    Buf& str(std::size_t off, std::string_view s) {
        REQUIRE(off + s.size() <= b.size());
        for (std::size_t i = 0; i < s.size(); ++i) b[off + i] = s[i];
        return *this;
    }
    // The common header: type, stock locate, tracking number, timestamp.
    Buf& header(char type, std::uint16_t locate, std::uint16_t tracking, std::uint64_t ts) {
        ch(0, type);
        be(1, 2, locate);
        be(3, 2, tracking);
        be(5, 6, ts);
        return *this;
    }
    const char* data() const { return b.data(); }
    std::size_t size() const { return b.size(); }
};

// Handler that records which message type it received and keeps a copy of
// the bytes so the test can decode through the typed view it was handed.
struct Capture {
    char got = 0;
    int calls = 0;
    int bad = 0;
    int unknown = 0;
    std::vector<char> seen;
    template <class M>
    void on(const M& m) {
        got = M::kType;
        ++calls;
        seen.assign(m.p, m.p + M::kSize);
    }
    void on_bad_length(const char*, std::size_t) { ++bad; }
    void on_unknown(const char*, std::size_t) { ++unknown; }
};

struct TypeLog {
    std::vector<char> types;
    template <class M>
    void on(const M&) { types.push_back(M::kType); }
};

constexpr std::uint16_t kLoc = 0x1234;
constexpr std::uint16_t kTrack = 0xABCD;
constexpr std::uint64_t kTs = 0x0102030405A6ull;  // 6 bytes, all distinct

template <class M>
M view_of(const Buf& buf) {
    Capture c;
    REQUIRE(dispatch(buf.data(), buf.size(), c) == Status::ok);
    REQUIRE(c.calls == 1);
    REQUIRE(c.got == M::kType);
    return M{{buf.data()}};
}

void check_header(const MessageView& m, char type) {
    CHECK(m.type() == type);
    CHECK(m.stock_locate() == kLoc);
    CHECK(m.tracking_number() == kTrack);
    CHECK(m.timestamp() == kTs);
}

}  // namespace

TEST_CASE("big-endian primitives") {
    const char b[8] = {'\x01', '\x02', '\x03', '\x04', '\x05', '\x06', '\x07', '\x08'};
    CHECK(be16(b) == 0x0102u);
    CHECK(be32(b) == 0x01020304u);
    CHECK(be48(b) == 0x010203040506ull);
    CHECK(be64(b) == 0x0102030405060708ull);
    const char hi[6] = {'\xFF', '\xFE', '\xFD', '\xFC', '\xFB', '\xFA'};
    CHECK(be48(hi) == 0xFFFEFDFCFBFAull);
    CHECK(be16(hi) == 0xFFFEu);
}

TEST_CASE("timestamp: 6 bytes, nanoseconds since midnight") {
    // 15:59:59.999999999 = 57599999999999 ns = 0x3462_2BD1_7FFF
    Buf b(12);
    b.header('S', 0, 0, 57599999999999ull).ch(11, 'M');
    const auto m = view_of<SystemEvent>(b);
    CHECK(m.timestamp() == 57599999999999ull);
}

TEST_CASE("literal bytes: Add Order exactly as on the wire") {
    // Hand-assembled 36-byte 'A' message:
    // locate 0x000D, tracking 0x0002, ts 0x00_00_1C_5A_0E_3F, ref 0x...0B80,
    // 'S', shares 0x00000064 (100), "MSFT    ", price 0x0017D798 (1562520 = 156.2520)
    const unsigned char raw[36] = {'A',  0x00, 0x0D, 0x00, 0x02, 0x00, 0x00, 0x1C, 0x5A, 0x0E, 0x3F, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x0B, 0x80, 'S',  0x00, 0x00, 0x00, 0x64,
                                   'M',  'S',  'F',  'T',  ' ',  ' ',  ' ',  ' ',  0x00, 0x17, 0xD7, 0x98};
    const char* p = reinterpret_cast<const char*>(raw);
    Capture c;
    REQUIRE(dispatch(p, 36, c) == Status::ok);
    REQUIRE(c.got == 'A');
    const AddOrder m{{p}};
    CHECK(m.stock_locate() == 13);
    CHECK(m.tracking_number() == 2);
    CHECK(m.timestamp() == 0x1C5A0E3Full);
    CHECK(m.order_reference_number() == 0x0B80u);
    CHECK(m.buy_sell_indicator() == 'S');
    CHECK(m.shares() == 100u);
    CHECK(m.stock() == "MSFT    ");
    CHECK(trim_alpha(m.stock()) == "MSFT");
    CHECK(m.price() == 1562520u);
}

TEST_CASE("1.1 System Event 'S' (12)") {
    Buf b(12);
    b.header('S', kLoc, kTrack, kTs).ch(11, 'Q');
    const auto m = view_of<SystemEvent>(b);
    check_header(m, 'S');
    CHECK(m.event_code() == 'Q');
}

TEST_CASE("1.2.1 Stock Directory 'R' (39)") {
    Buf b(39);
    b.header('R', kLoc, kTrack, kTs)
        .str(11, "BRK A   ")
        .ch(19, 'N')
        .ch(20, 'D')
        .be(21, 4, 0x00000064)
        .ch(25, 'Y')
        .ch(26, 'C')
        .str(27, "Z1")
        .ch(29, 'P')
        .ch(30, 'N')
        .ch(31, 'Z')
        .ch(32, '1')
        .ch(33, 'Y')
        .be(34, 4, 0x01020304)
        .ch(38, 'Y');
    const auto m = view_of<StockDirectory>(b);
    check_header(m, 'R');
    CHECK(m.stock() == "BRK A   ");
    CHECK(m.market_category() == 'N');
    CHECK(m.financial_status_indicator() == 'D');
    CHECK(m.round_lot_size() == 100u);
    CHECK(m.round_lots_only() == 'Y');
    CHECK(m.issue_classification() == 'C');
    CHECK(m.issue_sub_type() == "Z1");
    CHECK(m.authenticity() == 'P');
    CHECK(m.short_sale_threshold_indicator() == 'N');
    CHECK(m.ipo_flag() == 'Z');
    CHECK(m.luld_reference_price_tier() == '1');
    CHECK(m.etp_flag() == 'Y');
    CHECK(m.etp_leverage_factor() == 0x01020304u);
    CHECK(m.inverse_indicator() == 'Y');
}

TEST_CASE("1.2.2 Stock Trading Action 'H' (25)") {
    Buf b(25);
    b.header('H', kLoc, kTrack, kTs).str(11, "AAPL    ").ch(19, 'T').ch(20, ' ').str(21, "LUDP");
    const auto m = view_of<StockTradingAction>(b);
    check_header(m, 'H');
    CHECK(m.stock() == "AAPL    ");
    CHECK(m.trading_state() == 'T');
    CHECK(m.reserved() == ' ');
    CHECK(m.reason() == "LUDP");
}

TEST_CASE("1.2.3 Reg SHO 'Y' (20)") {
    Buf b(20);
    b.header('Y', kLoc, kTrack, kTs).str(11, "ZVZZT   ").ch(19, '2');
    const auto m = view_of<RegShoRestriction>(b);
    check_header(m, 'Y');
    CHECK(m.stock() == "ZVZZT   ");
    CHECK(m.reg_sho_action() == '2');
}

TEST_CASE("1.2.4 Market Participant Position 'L' (26)") {
    Buf b(26);
    b.header('L', kLoc, kTrack, kTs).str(11, "GSCO").str(15, "QQQ     ").ch(23, 'Y').ch(24, 'P').ch(25, 'E');
    const auto m = view_of<MarketParticipantPosition>(b);
    check_header(m, 'L');
    CHECK(m.mpid() == "GSCO");
    CHECK(m.stock() == "QQQ     ");
    CHECK(m.primary_market_maker() == 'Y');
    CHECK(m.market_maker_mode() == 'P');
    CHECK(m.market_participant_state() == 'E');
}

TEST_CASE("1.2.5.1 MWCB Decline Level 'V' (35), Price(8)") {
    Buf b(35);
    b.header('V', 0, kTrack, kTs)
        .be(11, 8, 0x0102030405060708ull)
        .be(19, 8, 0x1112131415161718ull)
        .be(27, 8, 0x2122232425262728ull);
    const auto m = view_of<MwcbDeclineLevel>(b);
    CHECK(m.stock_locate() == 0);
    CHECK(m.timestamp() == kTs);
    CHECK(m.level1() == 0x0102030405060708ull);
    CHECK(m.level2() == 0x1112131415161718ull);
    CHECK(m.level3() == 0x2122232425262728ull);
}

TEST_CASE("1.2.5.2 MWCB Status 'W' (12)") {
    Buf b(12);
    b.header('W', 0, kTrack, kTs).ch(11, '2');
    const auto m = view_of<MwcbStatus>(b);
    CHECK(m.breached_level() == '2');
}

TEST_CASE("1.2.6 IPO Quoting Period Update 'K' (28)") {
    Buf b(28);
    b.header('K', kLoc, kTrack, kTs).str(11, "NEWCO   ").be(19, 4, 34200).ch(23, 'A').be(24, 4, 0x000F4240);
    const auto m = view_of<IpoQuotingPeriodUpdate>(b);
    check_header(m, 'K');
    CHECK(m.stock() == "NEWCO   ");
    CHECK(m.ipo_quotation_release_time() == 34200u);
    CHECK(m.ipo_quotation_release_qualifier() == 'A');
    CHECK(m.ipo_price() == 1000000u);
}

TEST_CASE("1.2.7 LULD Auction Collar 'J' (35)") {
    Buf b(35);
    b.header('J', kLoc, kTrack, kTs)
        .str(11, "TSLA    ")
        .be(19, 4, 0x01020304)
        .be(23, 4, 0x05060708)
        .be(27, 4, 0x090A0B0C)
        .be(31, 4, 3);
    const auto m = view_of<LuldAuctionCollar>(b);
    check_header(m, 'J');
    CHECK(m.stock() == "TSLA    ");
    CHECK(m.auction_collar_reference_price() == 0x01020304u);
    CHECK(m.upper_auction_collar_price() == 0x05060708u);
    CHECK(m.lower_auction_collar_price() == 0x090A0B0Cu);
    CHECK(m.auction_collar_extension() == 3u);
}

TEST_CASE("1.2.8 Operational Halt 'h' (21)") {
    Buf b(21);
    b.header('h', kLoc, kTrack, kTs).str(11, "IBM     ").ch(19, 'X').ch(20, 'H');
    const auto m = view_of<OperationalHalt>(b);
    check_header(m, 'h');
    CHECK(m.stock() == "IBM     ");
    CHECK(m.market_code() == 'X');
    CHECK(m.operational_halt_action() == 'H');
}

TEST_CASE("1.3.1 Add Order 'A' (36)") {
    Buf b(36);
    b.header('A', kLoc, kTrack, kTs)
        .be(11, 8, 0x0102030405060708ull)
        .ch(19, 'B')
        .be(20, 4, 0x0A0B0C0D)
        .str(24, "AAPL    ")
        .be(32, 4, 0x77359400);  // 200,000.0000, the documented maximum Price(4)
    const auto m = view_of<AddOrder>(b);
    check_header(m, 'A');
    CHECK(m.order_reference_number() == 0x0102030405060708ull);
    CHECK(m.buy_sell_indicator() == 'B');
    CHECK(m.shares() == 0x0A0B0C0Du);
    CHECK(m.stock() == "AAPL    ");
    CHECK(m.price() == 2000000000u);
}

TEST_CASE("1.3.2 Add Order with MPID 'F' (40)") {
    Buf b(40);
    b.header('F', kLoc, kTrack, kTs)
        .be(11, 8, 0x1122334455667788ull)
        .ch(19, 'S')
        .be(20, 4, 300)
        .str(24, "SPY     ")
        .be(32, 4, 3215000)
        .str(36, "UBSS");
    const auto m = view_of<AddOrderMpid>(b);
    check_header(m, 'F');
    CHECK(m.order_reference_number() == 0x1122334455667788ull);
    CHECK(m.buy_sell_indicator() == 'S');
    CHECK(m.shares() == 300u);
    CHECK(m.stock() == "SPY     ");
    CHECK(m.price() == 3215000u);
    CHECK(m.attribution() == "UBSS");
}

TEST_CASE("1.4.1 Order Executed 'E' (31)") {
    Buf b(31);
    b.header('E', kLoc, kTrack, kTs).be(11, 8, 0xA1A2A3A4A5A6A7A8ull).be(19, 4, 0x00010203).be(23, 8, 0xB1B2B3B4B5B6B7B8ull);
    const auto m = view_of<OrderExecuted>(b);
    check_header(m, 'E');
    CHECK(m.order_reference_number() == 0xA1A2A3A4A5A6A7A8ull);
    CHECK(m.executed_shares() == 0x00010203u);
    CHECK(m.match_number() == 0xB1B2B3B4B5B6B7B8ull);
}

TEST_CASE("1.4.2 Order Executed With Price 'C' (36)") {
    Buf b(36);
    b.header('C', kLoc, kTrack, kTs)
        .be(11, 8, 42)
        .be(19, 4, 0x01000002)
        .be(23, 8, 0x0000000000ABCDEFull)
        .ch(31, 'N')
        .be(32, 4, 0x00BC614E);
    const auto m = view_of<OrderExecutedWithPrice>(b);
    check_header(m, 'C');
    CHECK(m.order_reference_number() == 42u);
    CHECK(m.executed_shares() == 0x01000002u);
    CHECK(m.match_number() == 0xABCDEFu);
    CHECK(m.printable() == 'N');
    CHECK(m.execution_price() == 12345678u);
}

TEST_CASE("1.4.3 Order Cancel 'X' (23)") {
    Buf b(23);
    b.header('X', kLoc, kTrack, kTs).be(11, 8, 0x0807060504030201ull).be(19, 4, 0x11223344);
    const auto m = view_of<OrderCancel>(b);
    check_header(m, 'X');
    CHECK(m.order_reference_number() == 0x0807060504030201ull);
    CHECK(m.cancelled_shares() == 0x11223344u);
}

TEST_CASE("1.4.4 Order Delete 'D' (19)") {
    Buf b(19);
    b.header('D', kLoc, kTrack, kTs).be(11, 8, 0xFFEEDDCCBBAA9988ull);
    const auto m = view_of<OrderDelete>(b);
    check_header(m, 'D');
    CHECK(m.order_reference_number() == 0xFFEEDDCCBBAA9988ull);
}

TEST_CASE("1.4.5 Order Replace 'U' (35)") {
    Buf b(35);
    b.header('U', kLoc, kTrack, kTs)
        .be(11, 8, 0x0101010101010102ull)
        .be(19, 8, 0x0202020202020203ull)
        .be(27, 4, 0x00000A0B)
        .be(31, 4, 0x0C0D0E0F);
    const auto m = view_of<OrderReplace>(b);
    check_header(m, 'U');
    CHECK(m.original_order_reference_number() == 0x0101010101010102ull);
    CHECK(m.new_order_reference_number() == 0x0202020202020203ull);
    CHECK(m.shares() == 0x0A0Bu);
    CHECK(m.price() == 0x0C0D0E0Fu);
}

TEST_CASE("1.5.1 Trade non-cross 'P' (44)") {
    Buf b(44);
    b.header('P', kLoc, kTrack, kTs)
        .be(11, 8, 0)
        .ch(19, 'B')
        .be(20, 4, 0x0000C350)
        .str(24, "QQQ     ")
        .be(32, 4, 0x002101D0)
        .be(36, 8, 0x0102030405060708ull);
    const auto m = view_of<Trade>(b);
    check_header(m, 'P');
    CHECK(m.order_reference_number() == 0u);
    CHECK(m.buy_sell_indicator() == 'B');
    CHECK(m.shares() == 50000u);
    CHECK(m.stock() == "QQQ     ");
    CHECK(m.price() == 0x002101D0u);
    CHECK(m.match_number() == 0x0102030405060708ull);
}

TEST_CASE("1.5.2 Cross Trade 'Q' (40), 8-byte shares") {
    Buf b(40);
    b.header('Q', kLoc, kTrack, kTs)
        .be(11, 8, 0x0000000102030405ull)
        .str(19, "AAPL    ")
        .be(27, 4, 2915200)
        .be(31, 8, 0x0A0B0C0D0E0F1011ull)
        .ch(39, 'C');
    const auto m = view_of<CrossTrade>(b);
    check_header(m, 'Q');
    CHECK(m.shares() == 0x0102030405ull);
    CHECK(m.stock() == "AAPL    ");
    CHECK(m.cross_price() == 2915200u);
    CHECK(m.match_number() == 0x0A0B0C0D0E0F1011ull);
    CHECK(m.cross_type() == 'C');
}

TEST_CASE("1.5.3 Broken Trade 'B' (19)") {
    Buf b(19);
    b.header('B', kLoc, kTrack, kTs).be(11, 8, 0x1020304050607080ull);
    const auto m = view_of<BrokenTrade>(b);
    check_header(m, 'B');
    CHECK(m.match_number() == 0x1020304050607080ull);
}

TEST_CASE("1.6 NOII 'I' (50)") {
    Buf b(50);
    b.header('I', kLoc, kTrack, kTs)
        .be(11, 8, 0x0000000011223344ull)
        .be(19, 8, 0x0000000055667788ull)
        .ch(27, 'S')
        .str(28, "MSFT    ")
        .be(36, 4, 0x01010101)
        .be(40, 4, 0x02020202)
        .be(44, 4, 0x03030303)
        .ch(48, 'O')
        .ch(49, 'A');
    const auto m = view_of<Noii>(b);
    check_header(m, 'I');
    CHECK(m.paired_shares() == 0x11223344ull);
    CHECK(m.imbalance_shares() == 0x55667788ull);
    CHECK(m.imbalance_direction() == 'S');
    CHECK(m.stock() == "MSFT    ");
    CHECK(m.far_price() == 0x01010101u);
    CHECK(m.near_price() == 0x02020202u);
    CHECK(m.current_reference_price() == 0x03030303u);
    CHECK(m.cross_type() == 'O');
    CHECK(m.price_variation_indicator() == 'A');
}

TEST_CASE("1.7 RPII 'N' (20)") {
    Buf b(20);
    b.header('N', kLoc, kTrack, kTs).str(11, "AMZN    ").ch(19, 'A');
    const auto m = view_of<Rpii>(b);
    check_header(m, 'N');
    CHECK(m.stock() == "AMZN    ");
    CHECK(m.interest_flag() == 'A');
}

TEST_CASE("1.8 DLCR 'O' (48)") {
    Buf b(48);
    b.header('O', kLoc, kTrack, kTs)
        .str(11, "DLCRX   ")
        .ch(19, 'Y')
        .be(20, 4, 0x00010001)
        .be(24, 4, 0x00020002)
        .be(28, 4, 0x00030003)
        .be(32, 8, 0x0000123456789ABCull)
        .be(40, 4, 0x00040004)
        .be(44, 4, 0x00050005);
    const auto m = view_of<Dlcr>(b);
    check_header(m, 'O');
    CHECK(m.stock() == "DLCRX   ");
    CHECK(m.open_eligibility_status() == 'Y');
    CHECK(m.minimum_allowable_price() == 0x00010001u);
    CHECK(m.maximum_allowable_price() == 0x00020002u);
    CHECK(m.near_execution_price() == 0x00030003u);
    CHECK(m.near_execution_time() == 0x123456789ABCull);
    CHECK(m.lower_price_range_collar() == 0x00040004u);
    CHECK(m.upper_price_range_collar() == 0x00050005u);
}

TEST_CASE("message sizes match the spec tables") {
    const std::pair<char, std::size_t> sizes[] = {
        {'S', 12}, {'R', 39}, {'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35}, {'W', 12}, {'K', 28},
        {'J', 35}, {'h', 21}, {'A', 36}, {'F', 40}, {'E', 31}, {'C', 36}, {'X', 23}, {'D', 19},
        {'U', 35}, {'P', 44}, {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48}};
    for (const auto& [t, n] : sizes) {
        CAPTURE(t);
        CHECK(message_size(t) == n);
    }
    CHECK(message_size('Z') == 0u);
    CHECK(message_size('a') == 0u);
}

TEST_CASE("dispatch rejects wrong lengths and unknown types") {
    Capture c;
    Buf a(35);  // one byte short of an Add Order
    a.ch(0, 'A');
    CHECK(dispatch(a.data(), a.size(), c) == Status::bad_length);
    Buf a2(37);  // one byte long
    a2.ch(0, 'A');
    CHECK(dispatch(a2.data(), a2.size(), c) == Status::bad_length);
    CHECK(c.bad == 2);
    CHECK(c.calls == 0);
    Buf z(20);
    z.ch(0, 'Z');
    CHECK(dispatch(z.data(), z.size(), c) == Status::unknown_type);
    CHECK(c.unknown == 1);
    CHECK(dispatch(z.data(), 0, c) == Status::bad_length);
}

TEST_CASE("handlers only see the types they accept") {
    struct OnlyAdds {
        int adds = 0;
        void on(const AddOrder&) { ++adds; }
    } h;
    Buf a(36);
    a.header('A', 1, 0, 0);
    Buf d(19);
    d.header('D', 1, 0, 0);
    CHECK(dispatch(a.data(), a.size(), h) == Status::ok);
    CHECK(dispatch(d.data(), d.size(), h) == Status::ok);
    CHECK(h.adds == 1);
}

TEST_CASE("framing: length prefixes, partial trailing message") {
    std::vector<char> stream;
    auto append = [&](const Buf& m) {
        stream.push_back(static_cast<char>((m.size() >> 8) & 0xFF));
        stream.push_back(static_cast<char>(m.size() & 0xFF));
        stream.insert(stream.end(), m.b.begin(), m.b.end());
    };
    Buf s(12);
    s.header('S', 0, 0, 1).ch(11, 'O');
    Buf a(36);
    a.header('A', 7, 0, 2).be(11, 8, 99).ch(19, 'B').be(20, 4, 100).str(24, "X       ").be(32, 4, 10000);
    Buf d(19);
    d.header('D', 7, 0, 3).be(11, 8, 99);
    append(s);
    append(a);
    append(d);
    const std::size_t full = stream.size();

    TypeLog h;
    CHECK(dispatch_buffer(stream.data(), stream.size(), h) == full);
    CHECK(h.types == std::vector<char>{'S', 'A', 'D'});

    // Cut the last message short: only the first two are consumed.
    h.types.clear();
    CHECK(dispatch_buffer(stream.data(), full - 1, h) == 2 + 12 + 2 + 36);
    CHECK(h.types == std::vector<char>{'S', 'A'});
    // Only one byte of a length prefix.
    h.types.clear();
    CHECK(dispatch_buffer(stream.data(), 1, h) == 0u);
    CHECK(h.types.empty());
}
