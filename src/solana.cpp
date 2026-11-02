#include "solana.hpp"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <sodium/core.h>
#include <sodium/crypto_sign.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tide::sol {

// --------------------------------------------------------------- encodings --

namespace {
constexpr std::string_view b58_alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr std::string_view b64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}  // namespace

std::string base58_encode(ByteView data) {
    std::size_t zeros = 0;
    while (zeros < data.size() && data[zeros] == 0) ++zeros;
    std::vector<std::uint8_t> digits;  // base-58, little-endian
    for (std::size_t i = zeros; i < data.size(); ++i) {
        int carry = data[i];
        for (auto& d : digits) {
            carry += d * 256;
            d = static_cast<std::uint8_t>(carry % 58);
            carry /= 58;
        }
        while (carry != 0) {
            digits.push_back(static_cast<std::uint8_t>(carry % 58));
            carry /= 58;
        }
    }
    std::string out(zeros, '1');
    for (const unsigned char& digit : std::ranges::reverse_view(digits)) out += b58_alphabet[digit];
    return out;
}

std::optional<Bytes> base58_decode(std::string_view text) {
    std::size_t ones = 0;
    while (ones < text.size() && text[ones] == '1') ++ones;
    std::vector<std::uint8_t> bytes;  // base-256, little-endian
    for (std::size_t i = ones; i < text.size(); ++i) {
        const std::size_t digit = b58_alphabet.find(text[i]);
        if (digit == std::string_view::npos) return std::nullopt;
        int carry = static_cast<int>(digit);
        for (auto& b : bytes) {
            carry += b * 58;
            b = static_cast<std::uint8_t>(carry & 0xff);
            carry >>= 8;
        }
        while (carry != 0) {
            bytes.push_back(static_cast<std::uint8_t>(carry & 0xff));
            carry >>= 8;
        }
    }
    Bytes out(ones, 0);
    out.insert(out.end(), bytes.rbegin(), bytes.rend());
    return out;
}

std::string hex_encode(ByteView data) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (auto b : data) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

std::optional<Bytes> hex_decode(std::string_view text) {
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.remove_prefix(2);
    if ((text.size() % 2) != 0U) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Bytes out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = nibble(text[i]);
        const int lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
    }
    return out;
}

std::string base64_encode(ByteView data) {
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        for (int s = 18; s >= 0; s -= 6) out += b64_alphabet[(v >> s) & 63];
    }
    if (const std::size_t rest = data.size() - i) {
        std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        if (rest == 2) v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        out += b64_alphabet[(v >> 18) & 63];
        out += b64_alphabet[(v >> 12) & 63];
        out += rest == 2 ? b64_alphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::optional<Bytes> base64_decode(std::string_view text) {
    while (!text.empty() && text.back() == '=') text.remove_suffix(1);
    Bytes out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : text) {
        const std::size_t sextet = b64_alphabet.find(c);
        if (sextet == std::string_view::npos) return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(sextet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xff));
        }
    }
    if (bits >= 6) return std::nullopt;  // a lone trailing sextet is malformed
    return out;
}

Hash sha256(ByteView data) { return sha256({data}); }

Hash sha256(std::initializer_list<ByteView> parts) {
    Hash out{};
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr);
    for (auto p : parts) EVP_DigestUpdate(ctx.get(), p.data(), p.size());
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx.get(), out.data(), &len);
    return out;
}

// -------------------------------------------------------------------- keys --

std::optional<Pubkey> Pubkey::from_base58(std::string_view text) {
    auto bytes = base58_decode(text);
    if (!bytes || bytes->size() != 32) return std::nullopt;
    return from_bytes(*bytes);
}

Pubkey Pubkey::from_bytes(ByteView b) {
    if (b.size() != 32) throw std::invalid_argument("pubkey must be 32 bytes");
    Pubkey p;
    std::copy(b.begin(), b.end(), p.bytes.begin());
    return p;
}

Pubkey pubkey_const(std::string_view base58) {
    auto p = Pubkey::from_base58(base58);
    if (!p) throw std::invalid_argument("invalid pubkey constant: " + std::string(base58));
    return *p;
}

namespace {
void ensure_sodium() {
    static const bool ok = sodium_init() >= 0;
    if (!ok) throw std::runtime_error("libsodium initialization failed");
}
}  // namespace

Keypair Keypair::from_seed(const std::array<std::uint8_t, 32>& seed) {
    ensure_sodium();
    Keypair kp;
    crypto_sign_seed_keypair(kp.pubkey.bytes.data(), kp.secret.data(), seed.data());
    return kp;
}

Keypair Keypair::from_name(std::string_view name) {
    return from_seed(sha256({as_bytes("tide:seed:"), as_bytes(name)}));
}

Keypair Keypair::from_json(std::string_view json) {
    std::vector<int> nums;
    int cur = -1;
    for (const char c : json) {
        if (c >= '0' && c <= '9')
            cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
        else {
            if (cur >= 0) nums.push_back(cur);
            cur = -1;
            if (c != ',' && c != '[' && c != ']' && (std::isspace(static_cast<unsigned char>(c)) == 0))
                throw std::invalid_argument("keypair file is not a JSON array of bytes");
        }
    }
    if (cur >= 0) nums.push_back(cur);
    if (nums.size() != 64) throw std::invalid_argument("keypair file must contain 64 bytes");
    const std::span<const int> all(nums);
    std::array<std::uint8_t, 32> seed{};
    std::ranges::transform(all.first(32), seed.begin(), [](int n) {
        if (n > 255) throw std::invalid_argument("keypair byte out of range");
        return static_cast<std::uint8_t>(n);
    });
    Keypair kp = from_seed(seed);
    if (!std::ranges::equal(kp.pubkey.bytes, all.subspan(32)))
        throw std::invalid_argument("keypair file public key does not match its secret key");
    return kp;
}

std::string Keypair::to_json() const {
    std::string out = "[";
    for (const std::uint8_t b : secret) {
        if (out.size() > 1) out += ',';
        out += std::to_string(b);
    }
    return out + "]";
}

std::array<std::uint8_t, 64> Keypair::sign(ByteView message) const {
    ensure_sodium();
    std::array<std::uint8_t, 64> sig{};
    crypto_sign_detached(sig.data(), nullptr, message.data(), message.size(), secret.data());
    return sig;
}

// Matches curve25519-dalek's CompressedEdwardsY::decompress().is_some(), which
// Solana uses: the point is "on curve" iff (y^2 - 1) / (d*y^2 + 1) is a square
// in GF(2^255 - 19). (libsodium's validity check also rejects small-order and
// non-prime-subgroup points, which would yield wrong PDAs.)
bool is_on_curve(const Pubkey& key) {
    const std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> ctx(BN_CTX_new(), BN_CTX_free);
    auto bn = [] { return std::unique_ptr<BIGNUM, decltype(&BN_free)>(BN_new(), BN_free); };
    auto p = bn();
    auto d = bn();
    auto y = bn();
    auto y2 = bn();
    auto u = bn();
    auto v = bn();
    auto w = bn();
    auto e = bn();
    auto tmp = bn();
    auto one = bn();
    BN_one(one.get());
    // p = 2^255 - 19
    BN_set_bit(p.get(), 255);
    BN_sub_word(p.get(), 19);
    // d = -121665 / 121666 mod p
    BN_set_word(tmp.get(), 121666);
    BN_mod_inverse(d.get(), tmp.get(), p.get(), ctx.get());
    BN_set_word(tmp.get(), 121665);
    BN_mod_mul(d.get(), d.get(), tmp.get(), p.get(), ctx.get());
    BN_sub(d.get(), p.get(), d.get());

    std::array<std::uint8_t, 32> ybytes = key.bytes;
    ybytes[31] &= 0x7f;
    BN_lebin2bn(ybytes.data(), 32, y.get());
    BN_nnmod(y.get(), y.get(), p.get(), ctx.get());
    BN_mod_sqr(y2.get(), y.get(), p.get(), ctx.get());
    BN_mod_sub(u.get(), y2.get(), one.get(), p.get(), ctx.get());  // u = y^2 - 1
    BN_mod_mul(v.get(), d.get(), y2.get(), p.get(), ctx.get());
    BN_mod_add(v.get(), v.get(), one.get(), p.get(), ctx.get());  // v = d*y^2 + 1
    if (BN_is_zero(v.get()) != 0) return false;
    BN_mod_inverse(tmp.get(), v.get(), p.get(), ctx.get());
    BN_mod_mul(w.get(), u.get(), tmp.get(), p.get(), ctx.get());
    if (BN_is_zero(w.get()) != 0) return true;
    // Euler's criterion: w^((p-1)/2) == 1
    BN_sub(e.get(), p.get(), one.get());
    BN_rshift1(e.get(), e.get());
    BN_mod_exp(tmp.get(), w.get(), e.get(), p.get(), ctx.get());
    return BN_is_one(tmp.get()) != 0;
}

// ----------------------------------------------------- address derivation --

std::optional<Pubkey> create_program_address(const std::vector<Bytes>& seeds, std::uint8_t bump,
                                             const Pubkey& program) {
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr);
    for (const auto& s : seeds) EVP_DigestUpdate(ctx.get(), s.data(), s.size());
    EVP_DigestUpdate(ctx.get(), &bump, 1);
    EVP_DigestUpdate(ctx.get(), program.bytes.data(), 32);
    constexpr std::string_view marker = "ProgramDerivedAddress";
    EVP_DigestUpdate(ctx.get(), marker.data(), marker.size());
    Pubkey out;
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx.get(), out.bytes.data(), &len);
    if (is_on_curve(out)) return std::nullopt;
    return out;
}

std::pair<Pubkey, std::uint8_t> find_program_address(const std::vector<Bytes>& seeds, const Pubkey& program) {
    if (seeds.size() > max_seeds - 1) throw std::invalid_argument("too many seeds (max 15 plus the bump)");
    for (const auto& s : seeds)
        if (s.size() > max_seed_len) throw std::invalid_argument("seed longer than 32 bytes");
    for (int bump = 255; bump >= 0; --bump)
        if (auto addr = create_program_address(seeds, static_cast<std::uint8_t>(bump), program))
            return {*addr, static_cast<std::uint8_t>(bump)};
    throw std::invalid_argument("unable to find a viable program address bump seed");
}

Pubkey associated_token_address(const Pubkey& owner, const Pubkey& mint, const Pubkey& token_program) {
    const std::vector<Bytes> seeds = {Bytes(owner.bytes.begin(), owner.bytes.end()),
                                      Bytes(token_program.bytes.begin(), token_program.bytes.end()),
                                      Bytes(mint.bytes.begin(), mint.bytes.end())};
    return find_program_address(seeds, ids::associated_token_program).first;
}

// ------------------------------------------------------- well-known ids --

namespace {
// Compile-time base58 decoding of a 32-byte key, so the ids below are
// constant-initialized (no static initialization order, nothing to throw at
// startup). A malformed literal fails to compile.
consteval Pubkey pubkey_literal(std::string_view text) {
    std::size_t ones = 0;
    while (ones < text.size() && text[ones] == '1') ++ones;
    Pubkey key;  // big-endian base-256 accumulator
    for (const char c : text.substr(ones)) {
        const std::size_t digit = b58_alphabet.find(c);
        if (digit == std::string_view::npos) throw std::invalid_argument("invalid base58 character");
        std::size_t carry = digit;
        for (std::uint8_t& b : std::ranges::reverse_view(key.bytes)) {
            carry += std::size_t{b} * 58;
            b = static_cast<std::uint8_t>(carry & 0xff);
            carry >>= 8;
        }
        if (carry != 0) throw std::invalid_argument("base58 key longer than 32 bytes");
    }
    // Leading '1's encode leading zero bytes; the rest must fill the key exactly.
    std::size_t leading_zeros = 0;
    for (const std::uint8_t b : key.bytes) {
        if (b != 0) break;
        ++leading_zeros;
    }
    if (leading_zeros != ones) throw std::invalid_argument("base58 key shorter than 32 bytes");
    return key;
}
}  // namespace

namespace ids {
constexpr Pubkey system_program = pubkey_literal("11111111111111111111111111111111");
constexpr Pubkey token_program = pubkey_literal("TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA");
constexpr Pubkey token_2022_program = pubkey_literal("TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb");
constexpr Pubkey associated_token_program = pubkey_literal("ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL");
constexpr Pubkey bpf_loader = pubkey_literal("BPFLoader2111111111111111111111111111111111");
constexpr Pubkey bpf_loader_deprecated = pubkey_literal("BPFLoader1111111111111111111111111111111111");
constexpr Pubkey bpf_upgradeable_loader = pubkey_literal("BPFLoaderUpgradeab1e11111111111111111111111");
constexpr Pubkey loader_v4 = pubkey_literal("LoaderV411111111111111111111111111111111111");
constexpr Pubkey memo_program = pubkey_literal("MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr");
constexpr Pubkey compute_budget_program = pubkey_literal("ComputeBudget111111111111111111111111111111");
constexpr Pubkey address_lookup_table_program = pubkey_literal("AddressLookupTab1e1111111111111111111111111");
constexpr Pubkey native_loader = pubkey_literal("NativeLoader1111111111111111111111111111111");
constexpr Pubkey sysvar_clock = pubkey_literal("SysvarC1ock11111111111111111111111111111111");
constexpr Pubkey sysvar_rent = pubkey_literal("SysvarRent111111111111111111111111111111111");
constexpr Pubkey sysvar_epoch_schedule = pubkey_literal("SysvarEpochSchedu1e111111111111111111111111");
}  // namespace ids

std::uint64_t rent_exempt_minimum(std::size_t data_len) {
    return (static_cast<std::uint64_t>(data_len) + 128) * 3480 * 2;
}

// ------------------------------------------------------------------ borsh --

void BorshWriter::f32(float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    u32(bits);
}
void BorshWriter::f64(double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, 8);
    u64(bits);
}

uint128 BorshReader::uint_le(int n) {
    const auto len = static_cast<std::size_t>(n);
    if (remaining() < len) throw DecodeError("unexpected end of data");
    uint128 v = 0;
    for (std::size_t i = 0; i < len; ++i) v |= static_cast<uint128>(data_[pos_ + i]) << (8 * i);
    pos_ += len;
    return v;
}
bool BorshReader::boolean() {
    auto b = u8();
    if (b > 1) throw DecodeError("invalid bool");
    return b == 1;
}
float BorshReader::f32() {
    std::uint32_t bits = u32();
    float v = NAN;
    std::memcpy(&v, &bits, 4);
    return v;
}
double BorshReader::f64() {
    std::uint64_t bits = u64();
    double v = NAN;
    std::memcpy(&v, &bits, 8);
    return v;
}
Bytes BorshReader::raw(std::size_t n) {
    if (remaining() < n) throw DecodeError("unexpected end of data");
    Bytes out(data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
    pos_ += n;
    return out;
}
std::string BorshReader::string() {
    Bytes b = bytes();
    return std::string(b.begin(), b.end());
}

// ------------------------------------------------------------ SPL layouts --

namespace {
// SPL `COption<Pubkey>`: u32 tag + 32 bytes (always present).
void coption_pubkey(BorshWriter& w, const std::optional<Pubkey>& p) {
    w.u32(p ? 1 : 0);
    w.pubkey(p ? *p : Pubkey{});
}
std::optional<Pubkey> read_coption_pubkey(BorshReader& r) {
    auto tag = r.u32();
    Pubkey p = r.pubkey();
    if (tag > 1) throw DecodeError("invalid COption tag");
    return (tag != 0U) ? std::optional<Pubkey>(p) : std::nullopt;
}

// Token-2022 pads mints and token accounts alike to the token account length
// before the account type byte, so the TLV area starts at the same offset.
void append_extensions(Bytes& out, std::uint8_t account_type, const std::vector<Extension>& exts) {
    if (exts.empty()) return;
    out.resize(token_account_base_len, 0);
    out.push_back(account_type);
    for (const auto& e : exts) {
        auto t = static_cast<std::uint16_t>(e.type);
        auto l = static_cast<std::uint16_t>(e.value.size());
        out.push_back(static_cast<std::uint8_t>(t & 0xff));
        out.push_back(static_cast<std::uint8_t>(t >> 8));
        out.push_back(static_cast<std::uint8_t>(l & 0xff));
        out.push_back(static_cast<std::uint8_t>(l >> 8));
        out.insert(out.end(), e.value.begin(), e.value.end());
    }
}
}  // namespace

Bytes encode_mint(const MintLayout& m) {
    BorshWriter w;
    coption_pubkey(w, m.mint_authority);
    w.u64(m.supply);
    w.u8(m.decimals);
    w.boolean(m.is_initialized);
    coption_pubkey(w, m.freeze_authority);
    Bytes out = w.take();
    append_extensions(out, 1, m.extensions);
    return out;
}

Bytes encode_token_account(const TokenAccountLayout& a) {
    BorshWriter w;
    w.pubkey(a.mint);
    w.pubkey(a.owner);
    w.u64(a.amount);
    coption_pubkey(w, a.delegate);
    w.u8(static_cast<std::uint8_t>(a.state));
    w.u32(a.is_native ? 1 : 0);
    w.u64(a.is_native.value_or(0));
    w.u64(a.delegated_amount);
    coption_pubkey(w, a.close_authority);
    Bytes out = w.take();
    append_extensions(out, 2, a.extensions);
    return out;
}

std::optional<MintLayout> decode_mint(ByteView data) {
    if (data.size() < mint_base_len) return std::nullopt;
    try {
        BorshReader r(data.subspan(0, mint_base_len));
        MintLayout m;
        m.mint_authority = read_coption_pubkey(r);
        m.supply = r.u64();
        m.decimals = r.u8();
        m.is_initialized = r.boolean();
        m.freeze_authority = read_coption_pubkey(r);
        return m;
    } catch (const DecodeError&) {
        return std::nullopt;
    }
}

std::optional<TokenAccountLayout> decode_token_account(ByteView data) {
    if (data.size() < token_account_base_len) return std::nullopt;
    try {
        BorshReader r(data.subspan(0, token_account_base_len));
        TokenAccountLayout a;
        a.mint = r.pubkey();
        a.owner = r.pubkey();
        a.amount = r.u64();
        a.delegate = read_coption_pubkey(r);
        auto st = r.u8();
        if (st > 2) return std::nullopt;
        a.state = static_cast<TokenAccountState>(st);
        auto native_tag = r.u32();
        auto native = r.u64();
        if (native_tag != 0U) a.is_native = native;
        a.delegated_amount = r.u64();
        a.close_authority = read_coption_pubkey(r);
        return a;
    } catch (const DecodeError&) {
        return std::nullopt;
    }
}

Bytes encode_lookup_table(const LookupTableLayout& t) {
    BorshWriter w;
    w.u32(1);  // ProgramState::LookupTable
    w.u64(t.deactivation_slot);
    w.u64(t.last_extended_slot);
    w.u8(t.last_extended_slot_start_index);
    w.u8(t.authority ? 1 : 0);
    if (t.authority) w.pubkey(*t.authority);
    Bytes out = w.take();
    out.resize(lookup_table_meta_len, 0);
    for (const auto& a : t.addresses) out.insert(out.end(), a.bytes.begin(), a.bytes.end());
    return out;
}

// ----------------------------------------------------------- transactions --

namespace {
// Solana's short_vec length: 7 bits per byte, high bit set while more follow.
void compact_u16(Bytes& out, std::size_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}
}  // namespace

namespace {
// An account referenced by a transaction, with the strongest access any
// instruction (or the fee payer) requires.
struct TxKey {
    Pubkey key;
    bool signer, writable;
};

// Fee payer first, then every instruction's accounts and program id, merged.
std::vector<TxKey> collect_keys(const std::vector<Instruction>& instructions, const Pubkey& fee_payer) {
    std::vector<TxKey> keys{{fee_payer, true, true}};
    auto upsert = [&](const Pubkey& k, bool signer, bool writable) {
        for (auto& e : keys)
            if (e.key == k) {
                e.signer |= signer;
                e.writable |= writable;
                return;
            }
        keys.push_back({k, signer, writable});
    };
    for (const auto& ix : instructions) {
        for (const auto& m : ix.accounts) upsert(m.pubkey, m.is_signer, m.is_writable);
        upsert(ix.program_id, false, false);
    }
    return keys;
}

// Group order of the Solana SDK's CompiledKeys: signer+writable,
// signer+readonly, writable, readonly.
int key_rank(const TxKey& k) {
    if (k.signer) return k.writable ? 0 : 1;
    return k.writable ? 2 : 3;
}

// Fee payer stays first; the rest is grouped by rank, each group sorted by
// pubkey bytes.
void order_keys(std::vector<TxKey>& keys) {
    std::sort(std::next(keys.begin()), keys.end(), [](const TxKey& a, const TxKey& b) {
        return key_rank(a) != key_rank(b) ? key_rank(a) < key_rank(b) : a.key < b.key;
    });
}

struct MessageHeader {
    std::uint8_t num_signers = 0;
    std::uint8_t ro_signed = 0;
    std::uint8_t ro_unsigned = 0;
};

MessageHeader message_header(const std::vector<TxKey>& keys) {
    MessageHeader h;
    for (const auto& k : keys) {
        if (k.signer) {
            ++h.num_signers;
            if (!k.writable) ++h.ro_signed;
        } else if (!k.writable) {
            ++h.ro_unsigned;
        }
    }
    return h;
}

std::uint8_t key_index(const std::vector<TxKey>& keys, const Pubkey& p) {
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (keys[i].key == p) return static_cast<std::uint8_t>(i);
    throw std::logic_error("account key missing");
}

// The legacy message: header, account keys, blockhash, compiled instructions.
Bytes compile_message(const std::vector<Instruction>& instructions, const std::vector<TxKey>& keys,
                      const MessageHeader& header, const Hash& blockhash) {
    Bytes msg{header.num_signers, header.ro_signed, header.ro_unsigned};
    compact_u16(msg, keys.size());
    for (const auto& k : keys) msg.insert(msg.end(), k.key.bytes.begin(), k.key.bytes.end());
    msg.insert(msg.end(), blockhash.begin(), blockhash.end());
    compact_u16(msg, instructions.size());
    for (const auto& ix : instructions) {
        msg.push_back(key_index(keys, ix.program_id));
        compact_u16(msg, ix.accounts.size());
        for (const auto& m : ix.accounts) msg.push_back(key_index(keys, m.pubkey));
        compact_u16(msg, ix.data.size());
        msg.insert(msg.end(), ix.data.begin(), ix.data.end());
    }
    return msg;
}

// One signature per signer key, in key order; the last matching keypair wins.
Bytes sign_message(const Bytes& msg, std::span<const TxKey> signer_keys, const std::vector<Keypair>& signers) {
    Bytes tx;
    compact_u16(tx, signer_keys.size());
    for (const auto& k : signer_keys) {
        const auto newest_first = signers | std::views::reverse;
        const auto match = std::ranges::find(newest_first, k.key, &Keypair::pubkey);
        if (match == newest_first.end()) throw std::invalid_argument("missing signer for " + k.key.base58());
        const auto sig = match->sign(msg);
        tx.insert(tx.end(), sig.begin(), sig.end());
    }
    tx.insert(tx.end(), msg.begin(), msg.end());
    return tx;
}
}  // namespace

Bytes build_transaction(const std::vector<Instruction>& instructions, const Pubkey& fee_payer, const Hash& blockhash,
                        const std::vector<Keypair>& signers) {
    std::vector<TxKey> keys = collect_keys(instructions, fee_payer);
    order_keys(keys);
    const MessageHeader header = message_header(keys);
    if (keys.size() > 256) throw std::invalid_argument("too many accounts in transaction");
    const Bytes msg = compile_message(instructions, keys, header, blockhash);
    return sign_message(msg, std::span(keys).first(header.num_signers), signers);
}

Bytes system_transfer_data(std::uint64_t lamports) {
    BorshWriter w;
    w.u32(2);
    w.u64(lamports);
    return w.take();
}
Bytes spl_transfer_data(std::uint64_t amount) {
    BorshWriter w;
    w.u8(3);
    w.u64(amount);
    return w.take();
}
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): -Wconversion rejects a swapped call (narrows to u8)
Bytes spl_transfer_checked_data(std::uint64_t amount, std::uint8_t decimals) {
    BorshWriter w;
    w.u8(12);
    w.u64(amount);
    w.u8(decimals);
    return w.take();
}
Bytes spl_mint_to_data(std::uint64_t amount) {
    BorshWriter w;
    w.u8(7);
    w.u64(amount);
    return w.take();
}
Instruction compute_unit_limit_ix(std::uint32_t units) {
    BorshWriter w;
    w.u8(2);
    w.u32(units);
    return {ids::compute_budget_program, {}, w.take()};
}
Instruction compute_unit_price_ix(std::uint64_t micro_lamports) {
    BorshWriter w;
    w.u8(3);
    w.u64(micro_lamports);
    return {ids::compute_budget_program, {}, w.take()};
}

std::array<std::uint8_t, 8> anchor_discriminator(std::string_view ns, std::string_view name) {
    Hash h = sha256({as_bytes(ns), as_bytes(":"), as_bytes(name)});
    std::array<std::uint8_t, 8> out{};
    std::copy_n(h.begin(), 8, out.begin());
    return out;
}

}  // namespace tide::sol
