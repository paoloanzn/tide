#include "solana.hpp"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace tide::sol {

// --------------------------------------------------------------- encodings --

namespace {
constexpr char b58_alphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr char b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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
        while (carry) {
            digits.push_back(static_cast<std::uint8_t>(carry % 58));
            carry /= 58;
        }
    }
    std::string out(zeros, '1');
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) out += b58_alphabet[*it];
    return out;
}

std::optional<Bytes> base58_decode(std::string_view text) {
    std::size_t ones = 0;
    while (ones < text.size() && text[ones] == '1') ++ones;
    std::vector<std::uint8_t> bytes;  // base-256, little-endian
    for (std::size_t i = ones; i < text.size(); ++i) {
        const char* p = std::strchr(b58_alphabet, text[i]);
        if (!p || text[i] == '\0') return std::nullopt;
        int carry = static_cast<int>(p - b58_alphabet);
        for (auto& b : bytes) {
            carry += b * 58;
            b = static_cast<std::uint8_t>(carry & 0xff);
            carry >>= 8;
        }
        while (carry) {
            bytes.push_back(static_cast<std::uint8_t>(carry & 0xff));
            carry >>= 8;
        }
    }
    Bytes out(ones, 0);
    out.insert(out.end(), bytes.rbegin(), bytes.rend());
    return out;
}

std::string hex_encode(ByteView data) {
    static const char* digits = "0123456789abcdef";
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
    if (text.size() % 2) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Bytes out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        int hi = nibble(text[i]), lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
    }
    return out;
}

std::string base64_encode(ByteView data) {
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        for (int s = 18; s >= 0; s -= 6) out += b64_alphabet[(v >> s) & 63];
    }
    if (std::size_t rest = data.size() - i) {
        std::uint32_t v = data[i] << 16;
        if (rest == 2) v |= data[i + 1] << 8;
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
    for (char c : text) {
        const char* p = std::strchr(b64_alphabet, c);
        if (!p || c == '\0') return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(p - b64_alphabet);
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
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
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
    for (char c : json) {
        if (c >= '0' && c <= '9') cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
        else {
            if (cur >= 0) nums.push_back(cur);
            cur = -1;
            if (c != ',' && c != '[' && c != ']' && !std::isspace(static_cast<unsigned char>(c)))
                throw std::invalid_argument("keypair file is not a JSON array of bytes");
        }
    }
    if (cur >= 0) nums.push_back(cur);
    if (nums.size() != 64) throw std::invalid_argument("keypair file must contain 64 bytes");
    std::array<std::uint8_t, 32> seed{};
    for (int i = 0; i < 32; ++i) {
        if (nums[i] > 255) throw std::invalid_argument("keypair byte out of range");
        seed[i] = static_cast<std::uint8_t>(nums[i]);
    }
    Keypair kp = from_seed(seed);
    for (int i = 0; i < 32; ++i)
        if (kp.pubkey.bytes[i] != nums[32 + i]) throw std::invalid_argument("keypair file public key does not match its secret key");
    return kp;
}

std::string Keypair::to_json() const {
    std::string out = "[";
    for (std::size_t i = 0; i < secret.size(); ++i) out += (i ? "," : "") + std::to_string(secret[i]);
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
    struct BnCtx {
        BN_CTX* c = BN_CTX_new();
        ~BnCtx() { BN_CTX_free(c); }
    } ctx;
    auto bn = [] { return std::unique_ptr<BIGNUM, decltype(&BN_free)>(BN_new(), BN_free); };
    auto p = bn(), d = bn(), y = bn(), y2 = bn(), u = bn(), v = bn(), w = bn(), e = bn(), tmp = bn(), one = bn();
    BN_one(one.get());
    // p = 2^255 - 19
    BN_set_bit(p.get(), 255);
    BN_sub_word(p.get(), 19);
    // d = -121665 / 121666 mod p
    BN_set_word(tmp.get(), 121666);
    BN_mod_inverse(d.get(), tmp.get(), p.get(), ctx.c);
    BN_set_word(tmp.get(), 121665);
    BN_mod_mul(d.get(), d.get(), tmp.get(), p.get(), ctx.c);
    BN_sub(d.get(), p.get(), d.get());

    std::array<std::uint8_t, 32> ybytes = key.bytes;
    ybytes[31] &= 0x7f;
    BN_lebin2bn(ybytes.data(), 32, y.get());
    BN_nnmod(y.get(), y.get(), p.get(), ctx.c);
    BN_mod_sqr(y2.get(), y.get(), p.get(), ctx.c);
    BN_mod_sub(u.get(), y2.get(), one.get(), p.get(), ctx.c);  // u = y^2 - 1
    BN_mod_mul(v.get(), d.get(), y2.get(), p.get(), ctx.c);
    BN_mod_add(v.get(), v.get(), one.get(), p.get(), ctx.c);  // v = d*y^2 + 1
    if (BN_is_zero(v.get())) return false;
    BN_mod_inverse(tmp.get(), v.get(), p.get(), ctx.c);
    BN_mod_mul(w.get(), u.get(), tmp.get(), p.get(), ctx.c);
    if (BN_is_zero(w.get())) return true;
    // Euler's criterion: w^((p-1)/2) == 1
    BN_sub(e.get(), p.get(), one.get());
    BN_rshift1(e.get(), e.get());
    BN_mod_exp(tmp.get(), w.get(), e.get(), p.get(), ctx.c);
    return BN_is_one(tmp.get());
}

// ----------------------------------------------------- address derivation --

std::optional<Pubkey> create_program_address(const std::vector<Bytes>& seeds, std::uint8_t bump, const Pubkey& program) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr);
    for (const auto& s : seeds) EVP_DigestUpdate(ctx.get(), s.data(), s.size());
    EVP_DigestUpdate(ctx.get(), &bump, 1);
    EVP_DigestUpdate(ctx.get(), program.bytes.data(), 32);
    static constexpr char marker[] = "ProgramDerivedAddress";
    EVP_DigestUpdate(ctx.get(), marker, sizeof(marker) - 1);
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
    std::vector<Bytes> seeds = {Bytes(owner.bytes.begin(), owner.bytes.end()),
                                Bytes(token_program.bytes.begin(), token_program.bytes.end()),
                                Bytes(mint.bytes.begin(), mint.bytes.end())};
    return find_program_address(seeds, ids::associated_token_program).first;
}

// ------------------------------------------------------- well-known ids --

namespace ids {
const Pubkey system_program = pubkey_const("11111111111111111111111111111111");
const Pubkey token_program = pubkey_const("TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA");
const Pubkey token_2022_program = pubkey_const("TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb");
const Pubkey associated_token_program = pubkey_const("ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL");
const Pubkey bpf_loader = pubkey_const("BPFLoader2111111111111111111111111111111111");
const Pubkey bpf_loader_deprecated = pubkey_const("BPFLoader1111111111111111111111111111111111");
const Pubkey bpf_upgradeable_loader = pubkey_const("BPFLoaderUpgradeab1e11111111111111111111111");
const Pubkey loader_v4 = pubkey_const("LoaderV411111111111111111111111111111111111");
const Pubkey memo_program = pubkey_const("MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr");
const Pubkey compute_budget_program = pubkey_const("ComputeBudget111111111111111111111111111111");
const Pubkey address_lookup_table_program = pubkey_const("AddressLookupTab1e1111111111111111111111111");
const Pubkey native_loader = pubkey_const("NativeLoader1111111111111111111111111111111");
const Pubkey sysvar_clock = pubkey_const("SysvarC1ock11111111111111111111111111111111");
const Pubkey sysvar_rent = pubkey_const("SysvarRent111111111111111111111111111111111");
const Pubkey sysvar_epoch_schedule = pubkey_const("SysvarEpochSchedu1e111111111111111111111111");
}  // namespace ids

std::uint64_t rent_exempt_minimum(std::size_t data_len) { return (static_cast<std::uint64_t>(data_len) + 128) * 3480 * 2; }

// ------------------------------------------------------------------ borsh --

void BorshWriter::f32(float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    u32(bits);
}
void BorshWriter::f64(double v) {
    std::uint64_t bits;
    std::memcpy(&bits, &v, 8);
    u64(bits);
}

unsigned __int128 BorshReader::uint_le(int n) {
    if (remaining() < static_cast<std::size_t>(n)) throw DecodeError("unexpected end of data");
    unsigned __int128 v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<unsigned __int128>(data_[pos_ + i]) << (8 * i);
    pos_ += n;
    return v;
}
bool BorshReader::boolean() {
    auto b = u8();
    if (b > 1) throw DecodeError("invalid bool");
    return b == 1;
}
float BorshReader::f32() {
    std::uint32_t bits = u32();
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}
double BorshReader::f64() {
    std::uint64_t bits = u64();
    double v;
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
    return tag ? std::optional<Pubkey>(p) : std::nullopt;
}

void append_extensions(Bytes& out, std::size_t base_len, std::uint8_t account_type, const std::vector<Extension>& exts) {
    if (exts.empty()) return;
    out.resize(token_account_base_len, 0);  // pad the base state to the account length
    (void)base_len;
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
    append_extensions(out, mint_base_len, 1, m.extensions);
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
    append_extensions(out, token_account_base_len, 2, a.extensions);
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
        if (native_tag) a.is_native = native;
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
void compact_u16(Bytes& out, std::size_t v) {
    do {
        std::uint8_t b = v & 0x7f;
        v >>= 7;
        if (v) b |= 0x80;
        out.push_back(b);
    } while (v);
}
}  // namespace

Bytes build_transaction(const std::vector<Instruction>& instructions, const Pubkey& fee_payer, const Hash& blockhash,
                        const std::vector<Keypair>& signers) {
    struct Key {
        Pubkey key;
        bool signer, writable;
    };
    std::vector<Key> keys{{fee_payer, true, true}};
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
    // Order like the Solana SDK's CompiledKeys: fee payer first, then
    // signer+writable, signer+readonly, writable, readonly; each group sorted
    // by pubkey bytes.
    auto rank = [](const Key& k) { return k.signer ? (k.writable ? 0 : 1) : (k.writable ? 2 : 3); };
    std::sort(keys.begin() + 1, keys.end(), [&](const Key& a, const Key& b) {
        return rank(a) != rank(b) ? rank(a) < rank(b) : a.key < b.key;
    });

    std::uint8_t num_signers = 0, ro_signed = 0, ro_unsigned = 0;
    for (const auto& k : keys) {
        if (k.signer) {
            ++num_signers;
            if (!k.writable) ++ro_signed;
        } else if (!k.writable) {
            ++ro_unsigned;
        }
    }
    auto index_of = [&](const Pubkey& p) {
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i].key == p) return static_cast<std::uint8_t>(i);
        throw std::logic_error("account key missing");
    };
    if (keys.size() > 256) throw std::invalid_argument("too many accounts in transaction");

    Bytes msg{num_signers, ro_signed, ro_unsigned};
    compact_u16(msg, keys.size());
    for (const auto& k : keys) msg.insert(msg.end(), k.key.bytes.begin(), k.key.bytes.end());
    msg.insert(msg.end(), blockhash.begin(), blockhash.end());
    compact_u16(msg, instructions.size());
    for (const auto& ix : instructions) {
        msg.push_back(index_of(ix.program_id));
        compact_u16(msg, ix.accounts.size());
        for (const auto& m : ix.accounts) msg.push_back(index_of(m.pubkey));
        compact_u16(msg, ix.data.size());
        msg.insert(msg.end(), ix.data.begin(), ix.data.end());
    }

    Bytes tx;
    compact_u16(tx, num_signers);
    for (std::size_t i = 0; i < num_signers; ++i) {
        const Keypair* kp = nullptr;
        for (const auto& s : signers)
            if (s.pubkey == keys[i].key) kp = &s;
        if (!kp) throw std::invalid_argument("missing signer for " + keys[i].key.base58());
        auto sig = kp->sign(msg);
        tx.insert(tx.end(), sig.begin(), sig.end());
    }
    tx.insert(tx.end(), msg.begin(), msg.end());
    return tx;
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
