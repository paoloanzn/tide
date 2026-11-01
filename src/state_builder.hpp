// The backend boundary.
//
// The interpreter talks to a Solana network implementation exclusively through
// the interfaces below. They speak in Solana-level terms (accounts, programs,
// instructions, features) - never in AST nodes - and carry only an opaque
// source `Loc` so backends can attach positioned diagnostics. Nothing here
// knows about RPC, validator processes or ledger formats; those live in a
// concrete backend (see solana_backend.hpp).
//
// Lifecycle driven by the interpreter:
//
//   configure(config)            interpret configuration
//   genesis().put_account(...)   construct genesis state (privileged writes)
//   start_bootstrap()            private instance for bootstrap transactions
//   persist()                    finalize durable state (stops the private instance)
//   launch()                     start the long-running network
//   detach()                     release it: the network outlives Tide
//
// abort() tears down anything started so far. Errors are reported by throwing
// tide::TideError carrying the origin location of the offending construct.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "diag.hpp"
#include "solana.hpp"

namespace tide::backend {

using sol::Bytes;
using sol::Keypair;
using sol::Pubkey;

// ------------------------------------------------------------ configuration --

struct ValidatorConfig {
    std::string name;
    Loc origin;
    std::optional<std::uint16_t> rpc_port, gossip_port, faucet_port;
    std::optional<std::string> dynamic_port_range, bind_address;
    std::optional<Keypair> identity;
};

struct NetworkConfig {
    std::string name;
    Loc origin;
    std::filesystem::path base_dir;  // directory of the script; relative paths resolve here
    std::optional<std::uint64_t> slots_per_epoch, ticks_per_slot, compute_unit_limit, limit_ledger_size, warp_slot;
    std::optional<double> faucet_sol;
    std::optional<std::string> ledger, bind_address;
    std::optional<std::int64_t> startup_timeout_ms, confirm_timeout_ms;  // unset: backend / default_confirm_timeout_ms
    std::vector<ValidatorConfig> validators;
};

// How long a transaction may take to confirm when `options { confirm_timeout }`
// is not set. Used by the interpreter (`await`) and by backends alike.
constexpr std::int64_t default_confirm_timeout_ms = 60'000;

// ------------------------------------------------------------------ genesis --

struct AccountSpec {
    Pubkey address;
    std::uint64_t lamports = 0;
    Pubkey owner;
    Bytes data;
    bool executable = false;
    std::uint64_t rent_epoch = 0;
    Loc origin;
};

struct ProgramSpec {
    std::string name;
    Pubkey id;
    std::filesystem::path binary;
    Pubkey loader;
    std::optional<Pubkey> upgrade_authority;
    std::optional<std::filesystem::path> idl;
    Loc origin;
};

using SysvarValue = std::variant<__int128, double, bool>;
struct SysvarSpec {
    std::string name;
    std::map<std::string, SysvarValue> fields;
    std::map<std::string, Loc> field_origins;
    Loc origin;
};

struct FeatureSpec {
    Pubkey feature;
    bool enabled = true;
    Loc origin;
};

struct ForkSpec {
    std::string url;
    std::optional<std::uint64_t> slot;
    Loc origin;
};

struct AccountOverrides {
    std::optional<std::uint64_t> lamports, rent_epoch;
    std::optional<Pubkey> owner;
    std::optional<Bytes> data;
    std::optional<bool> executable;
};

struct CloneSpec {
    Pubkey address;
    bool program = false;
    std::string url;
    std::optional<std::uint64_t> slot;
    AccountOverrides overrides;
    Loc origin;
};

struct DeleteSpec {
    Pubkey address;
    bool program = false;
    Loc origin;
};

struct AccountView {
    Pubkey address;
    std::uint64_t lamports = 0;
    Pubkey owner;
    Bytes data;
    bool executable = false;
    std::uint64_t rent_epoch = 0;
};

// Privileged, pre-launch state construction.
class GenesisBuilder {
public:
    virtual ~GenesisBuilder() = default;
    virtual void put_account(const AccountSpec& spec) = 0;
    virtual void put_program(const ProgramSpec& spec) = 0;
    virtual void clone_account(const CloneSpec& spec) = 0;
    virtual void remove_account(const DeleteSpec& spec) = 0;
    virtual void set_feature(const FeatureSpec& spec) = 0;
    virtual void set_sysvar(const SysvarSpec& spec) = 0;
    virtual void fork(const ForkSpec& spec) = 0;
    // The planned genesis state, for queries made while building it.
    virtual std::optional<AccountView> account(const Pubkey& address) const = 0;
};

// ------------------------------------------------------------------ runtime --

struct TransactionRequest {
    std::vector<sol::Instruction> instructions;
    Pubkey fee_payer;
    std::vector<Keypair> signers;
    std::optional<std::uint32_t> compute_unit_limit;
    std::optional<std::uint64_t> compute_unit_price;
    Loc origin;
};

struct TxResult {
    bool success = false;
    std::string signature;
    std::string error;
    std::vector<std::string> logs;
    std::uint64_t slot = 0;
};

struct DeployRequest {
    Keypair program;
    std::filesystem::path binary;
    Loc origin;
};

// Normal, authorized interaction with a running network.
class RuntimeClient {
public:
    virtual ~RuntimeClient() = default;
    virtual TxResult send_transaction(const TransactionRequest& request) = 0;
    virtual TxResult airdrop(const Pubkey& to, std::uint64_t lamports, Loc origin) = 0;
    virtual TxResult deploy(const DeployRequest& request) = 0;
    virtual std::optional<AccountView> account(const Pubkey& address) = 0;
    virtual std::uint64_t slot() = 0;
    virtual std::uint64_t epoch() = 0;
};

// ---------------------------------------------------------------- lifecycle --

struct LaunchInfo {
    std::string rpc_url;
    std::string state_dir;
    std::optional<int> pid;
};

class NetworkBackend {
public:
    virtual ~NetworkBackend() = default;
    virtual void configure(const NetworkConfig& config) = 0;
    virtual GenesisBuilder& genesis() = 0;
    virtual RuntimeClient& start_bootstrap() = 0;
    virtual void persist() = 0;
    virtual RuntimeClient& launch() = 0;
    virtual LaunchInfo detach() = 0;
    virtual void abort() noexcept = 0;
};

}  // namespace tide::backend
