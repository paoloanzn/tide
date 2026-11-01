// Backend-agnostic Solana primitives: keys, encodings, address derivation,
// borsh, SPL / address-lookup-table account layouts and legacy transaction
// serialization. No RPC, no process management, no AST.
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tide::sol {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;
using Hash = std::array<std::uint8_t, 32>;

inline ByteView as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

// --------------------------------------------------------------- encodings --

std::string base58_encode(ByteView data);
std::optional<Bytes> base58_decode(std::string_view text);
std::string hex_encode(ByteView data);
std::optional<Bytes> hex_decode(std::string_view text);  // accepts optional 0x prefix
std::string base64_encode(ByteView data);
std::optional<Bytes> base64_decode(std::string_view text);
Hash sha256(ByteView data);
Hash sha256(std::initializer_list<ByteView> parts);

// -------------------------------------------------------------------- keys --

struct Pubkey {
    std::array<std::uint8_t, 32> bytes{};
    auto operator<=>(const Pubkey&) const = default;
    std::string base58() const { return base58_encode(bytes); }
    static std::optional<Pubkey> from_base58(std::string_view text);
    static Pubkey from_bytes(ByteView b);  // requires b.size() == 32
};

// Compile-time-free constant constructor; throws on malformed input.
Pubkey pubkey_const(std::string_view base58);

struct Keypair {
    std::array<std::uint8_t, 64> secret{};  // ed25519 seed || public key (Solana layout)
    Pubkey pubkey;

    static Keypair from_seed(const std::array<std::uint8_t, 32>& seed);
    // Deterministic keypair for `seed("name")`: ed25519 seed = sha256("tide:seed:" + name).
    static Keypair from_name(std::string_view name);
    // Solana CLI JSON keypair file: [64 integers].
    static Keypair from_json(std::string_view json);
    std::string to_json() const;
    std::array<std::uint8_t, 64> sign(ByteView message) const;
};

bool is_on_curve(const Pubkey& key);

// ----------------------------------------------------- address derivation --

constexpr std::size_t max_seeds = 16;
constexpr std::size_t max_seed_len = 32;

std::optional<Pubkey> create_program_address(const std::vector<Bytes>& seeds, std::uint8_t bump,
                                             const Pubkey& program);
// Throws std::invalid_argument for too many / too long seeds.
std::pair<Pubkey, std::uint8_t> find_program_address(const std::vector<Bytes>& seeds, const Pubkey& program);
Pubkey associated_token_address(const Pubkey& owner, const Pubkey& mint, const Pubkey& token_program);

// ------------------------------------------------------- well-known ids --

namespace ids {
extern const Pubkey system_program, token_program, token_2022_program, associated_token_program, bpf_loader,
    bpf_loader_deprecated, bpf_upgradeable_loader, loader_v4, memo_program, compute_budget_program,
    address_lookup_table_program, native_loader, sysvar_clock, sysvar_rent, sysvar_epoch_schedule;
}

// Rent: (data_len + 128) * 3480 lamports/byte-year * 2 years.
std::uint64_t rent_exempt_minimum(std::size_t data_len);
constexpr std::uint64_t lamports_per_sol = 1'000'000'000;

// ------------------------------------------------------------------ borsh --

class BorshWriter {
public:
    void u8(std::uint8_t v) { out_.push_back(v); }
    void u16(std::uint16_t v) { le(v, 2); }
    void u32(std::uint32_t v) { le(v, 4); }
    void u64(std::uint64_t v) { le(v, 8); }
    void u128(unsigned __int128 v) { le(v, 16); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void f32(float v);
    void f64(double v);
    void raw(ByteView b) { out_.insert(out_.end(), b.begin(), b.end()); }
    void pubkey(const Pubkey& p) { raw(p.bytes); }
    void bytes(ByteView b) { u32(static_cast<std::uint32_t>(b.size())); raw(b); }  // Vec<u8>
    void string(std::string_view s) { bytes(as_bytes(s)); }
    Bytes& data() { return out_; }
    Bytes take() { return std::move(out_); }

private:
    template <typename T>
    void le(T v, int n) {
        for (int i = 0; i < n; ++i) out_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    Bytes out_;
};

struct DecodeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class BorshReader {
public:
    explicit BorshReader(ByteView data) : data_(data) {}
    std::uint8_t u8() { return static_cast<std::uint8_t>(le(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(le(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(le(4)); }
    std::uint64_t u64() { return static_cast<std::uint64_t>(le(8)); }
    unsigned __int128 u128() { return le(16); }
    bool boolean();
    float f32();
    double f64();
    Bytes raw(std::size_t n);
    Pubkey pubkey() { return Pubkey::from_bytes(raw(32)); }
    Bytes bytes() { return raw(u32()); }
    std::string string();
    std::size_t remaining() const { return data_.size() - pos_; }
    std::size_t position() const { return pos_; }

private:
    unsigned __int128 le(int n);
    ByteView data_;
    std::size_t pos_ = 0;
};

// ------------------------------------------------------------ SPL layouts --

// Token-2022 TLV extensions supported by Tide.
enum class ExtensionType : std::uint16_t {
    mint_close_authority = 3,
    default_account_state = 6,
    immutable_owner = 7,
    non_transferable = 9,
    permanent_delegate = 12,
    metadata_pointer = 18,
};

struct Extension {
    ExtensionType type;
    Bytes value;
};

enum class TokenAccountState : std::uint8_t { uninitialized = 0, initialized = 1, frozen = 2 };

struct MintLayout {
    std::optional<Pubkey> mint_authority;
    std::uint64_t supply = 0;
    std::uint8_t decimals = 0;
    bool is_initialized = true;
    std::optional<Pubkey> freeze_authority;
    std::vector<Extension> extensions;  // Token-2022 only
};

struct TokenAccountLayout {
    Pubkey mint, owner;
    std::uint64_t amount = 0;
    std::optional<Pubkey> delegate;
    TokenAccountState state = TokenAccountState::initialized;
    std::optional<std::uint64_t> is_native;  // rent-exempt reserve for wrapped SOL
    std::uint64_t delegated_amount = 0;
    std::optional<Pubkey> close_authority;
    std::vector<Extension> extensions;  // Token-2022 only
};

constexpr std::size_t mint_base_len = 82;
constexpr std::size_t token_account_base_len = 165;

Bytes encode_mint(const MintLayout& m);
Bytes encode_token_account(const TokenAccountLayout& a);
std::optional<MintLayout> decode_mint(ByteView data);
std::optional<TokenAccountLayout> decode_token_account(ByteView data);

struct LookupTableLayout {
    std::uint64_t deactivation_slot = UINT64_MAX;
    std::uint64_t last_extended_slot = 0;
    std::uint8_t last_extended_slot_start_index = 0;
    std::optional<Pubkey> authority;
    std::vector<Pubkey> addresses;
};
constexpr std::size_t lookup_table_meta_len = 56;
Bytes encode_lookup_table(const LookupTableLayout& t);

// ----------------------------------------------------------- transactions --

struct AccountMeta {
    Pubkey pubkey;
    bool is_signer = false;
    bool is_writable = false;
};

struct Instruction {
    Pubkey program_id;
    std::vector<AccountMeta> accounts;
    Bytes data;
};

// Serializes and signs a legacy transaction. Throws std::invalid_argument if a
// required signature has no matching keypair.
Bytes build_transaction(const std::vector<Instruction>& instructions, const Pubkey& fee_payer, const Hash& blockhash,
                        const std::vector<Keypair>& signers);

// Instruction data builders.
Bytes system_transfer_data(std::uint64_t lamports);
Bytes spl_transfer_data(std::uint64_t amount);
Bytes spl_transfer_checked_data(std::uint64_t amount, std::uint8_t decimals);
Bytes spl_mint_to_data(std::uint64_t amount);
Instruction compute_unit_limit_ix(std::uint32_t units);
Instruction compute_unit_price_ix(std::uint64_t micro_lamports);

// Anchor: first 8 bytes of sha256("<ns>:<name>"), e.g. ("global", "increment").
std::array<std::uint8_t, 8> anchor_discriminator(std::string_view ns, std::string_view name);

}  // namespace tide::sol
