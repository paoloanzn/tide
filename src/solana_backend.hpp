// NetworkBackend implementation driving Agave's `solana-test-validator`.
//
// Genesis state is persisted as account JSON files plus program flags under
// `<state_root>/<network>/`; bootstrap runs against a private validator
// instance on that state; the final network is launched as a detached process
// that resumes the persisted ledger and keeps running after Tide exits.
#pragma once

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>

#include "state_builder.hpp"

namespace tide::backend {

struct SolanaBackendOptions {
    std::filesystem::path state_root;  // default: <script dir>/.tide
    std::string validator_binary = "solana-test-validator";
    std::string solana_cli = "solana";
    bool dry_run = false;  // write state and print the command, never start processes
};

class SolanaTestValidatorBackend final : public NetworkBackend {
public:
    SolanaTestValidatorBackend(SolanaBackendOptions options, Diagnostics& diags, std::ostream& log);
    ~SolanaTestValidatorBackend() override;

    void configure(const NetworkConfig& config) override;
    GenesisBuilder& genesis() override;
    RuntimeClient& start_bootstrap() override;
    void persist() override;
    RuntimeClient& launch() override;
    LaunchInfo detach() override;
    void abort() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Stops a network launched earlier from `state_dir` (used by `tide down`).
// Returns false and sets `error` if nothing could be stopped.
bool stop_network(const std::filesystem::path& state_dir, std::ostream& log, std::string& error);

}  // namespace tide::backend
