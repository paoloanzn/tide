#include "solana_backend.hpp"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <ostream>
#include <sstream>
#include <thread>

extern char** environ;

namespace tide::backend {

using Json = nlohmann::json;
namespace fs = std::filesystem;

// solana-test-validator defaults and the waits Tide applies around it.
constexpr std::uint16_t default_rpc_port = 8899;
constexpr std::uint16_t default_faucet_port = 9900;
constexpr std::int64_t default_startup_timeout_ms = 120'000;
constexpr auto rpc_request_timeout = std::chrono::milliseconds(30'000);
constexpr auto health_request_timeout = std::chrono::milliseconds(2'000);
constexpr auto health_poll_interval = std::chrono::milliseconds(500);
constexpr auto slot_poll_interval = std::chrono::milliseconds(400);
constexpr auto confirm_poll_interval = std::chrono::milliseconds(300);
constexpr auto process_poll_interval = std::chrono::milliseconds(100);
constexpr auto finalize_timeout = std::chrono::seconds(90);
// Grace periods before a stop escalates to SIGKILL.
constexpr auto graceful_stop_timeout = std::chrono::seconds(30);  // bootstrap instance: flush the ledger
constexpr auto down_stop_timeout = std::chrono::seconds(20);      // `tide down`
constexpr auto abort_stop_timeout = std::chrono::seconds(10);     // teardown after an error

fs::path default_state_root(const fs::path& script_dir) { return script_dir / ".tide"; }

namespace {

[[noreturn]] void fail(Loc loc, const std::string& msg) { throw TideError(loc, msg); }

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
    if (!out) throw std::runtime_error("cannot write " + p.string());
}

std::string tail_lines(const fs::path& p, std::size_t n) {
    std::string text = read_file(p);
    std::size_t pos = text.size();
    for (std::size_t i = 0; i <= n && pos > 0; ++i) {
        pos = text.rfind('\n', pos - 1);
        if (pos == std::string::npos) return text;
    }
    return text.substr(pos + 1);
}

std::optional<fs::path> find_in_path(const std::string& name) {
    if (name.find('/') != std::string::npos) return fs::exists(name) ? std::optional<fs::path>(name) : std::nullopt;
    const char* path = std::getenv("PATH");
    if (!path) return std::nullopt;
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        fs::path candidate = fs::path(dir) / name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return std::nullopt;
}

bool port_in_use(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bool in_use = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0;
    ::close(fd);
    return in_use;
}

bool process_alive(pid_t pid) { return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM); }

// Spawns `argv` in a new session with stdout/stderr appended to `log_path`.
pid_t spawn_process(const std::vector<std::string>& argv, const fs::path& log_path) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, args[0], &actions, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) throw std::runtime_error("failed to start " + argv[0] + ": " + std::strerror(rc));
    return pid;
}

// Sends `sig`, waits up to `timeout`, escalating to SIGKILL. Works for both
// children (reaped) and unrelated processes (polled).
void stop_process(pid_t pid, int sig, std::chrono::milliseconds timeout) {
    if (pid <= 0) return;
    ::kill(pid, sig);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD && !process_alive(pid))) return;
        std::this_thread::sleep_for(process_poll_interval);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
}

std::string run_capture(const std::vector<std::string>& argv, int& exit_code) {
    char tmpl[] = "/tmp/tide-cmd-XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) throw std::runtime_error("cannot create temporary file");
    ::close(fd);
    pid_t pid = spawn_process(argv, tmpl);
    int status = 0;
    ::waitpid(pid, &status, 0);
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    std::string out = read_file(tmpl);
    ::unlink(tmpl);
    return out;
}

// ------------------------------------------------------------------ RPC --

struct RpcError : std::runtime_error {
    Json data;
    RpcError(const std::string& msg, Json d) : std::runtime_error(msg), data(std::move(d)) {}
};

std::optional<AccountView> parse_account(const Pubkey& address, const Json& v) {
    if (v.is_null()) return std::nullopt;
    AccountView a;
    a.address = address;
    a.lamports = v["lamports"].get<std::uint64_t>();
    a.owner = sol::pubkey_const(v["owner"].get<std::string>());
    a.executable = v["executable"].get<bool>();
    a.rent_epoch = v["rentEpoch"].is_number_unsigned() ? v["rentEpoch"].get<std::uint64_t>() : UINT64_MAX;
    auto data = sol::base64_decode(v["data"][0].get<std::string>());
    if (!data) throw std::runtime_error("RPC returned undecodable account data");
    a.data = std::move(*data);
    return a;
}

// JSON-RPC 2.0 transport to one Solana RPC endpoint (local or remote).
class JsonRpc {
public:
    explicit JsonRpc(std::string url) : url_(std::move(url)) {}

    const std::string& url() const { return url_; }

    // Returns `result`; throws RpcError for a JSON-RPC error and TideError for
    // transport failures.
    Json call(const std::string& method, Json params, Loc origin = {}) {
        Json req = {{"jsonrpc", "2.0"}, {"id", ++id_}, {"method", method}, {"params", std::move(params)}};
        cpr::Response r = cpr::Post(cpr::Url{url_}, cpr::Header{{"Content-Type", "application/json"}}, cpr::Body{req.dump()},
                                    cpr::Timeout{rpc_request_timeout});
        if (r.error) fail(origin, "RPC request '" + method + "' to " + url_ + " failed: " + r.error.message);
        Json body = Json::parse(r.text, nullptr, false);
        if (body.is_discarded()) fail(origin, "RPC '" + method + "' returned invalid JSON (HTTP " + std::to_string(r.status_code) + ")");
        if (body.contains("error"))
            throw RpcError(body["error"].value("message", "unknown RPC error"), body["error"].value("data", Json()));
        return body["result"];
    }

    // Short-timeout probe used while a validator starts.
    bool healthy() {
        cpr::Response r = cpr::Post(cpr::Url{url_}, cpr::Header{{"Content-Type", "application/json"}},
                                    cpr::Body{R"({"jsonrpc":"2.0","id":1,"method":"getHealth"})"}, cpr::Timeout{health_request_timeout});
        if (r.error || r.status_code != 200) return false;
        Json body = Json::parse(r.text, nullptr, false);
        return !body.is_discarded() && body.value("result", "") == "ok";
    }

    std::optional<AccountView> account(const Pubkey& address, Loc origin = {}) {
        Json r = call("getAccountInfo", {address.base58(), {{"encoding", "base64"}, {"commitment", "confirmed"}}}, origin);
        return parse_account(address, r["value"]);
    }

    std::uint64_t slot(const char* commitment) { return call("getSlot", {{{"commitment", commitment}}}).get<std::uint64_t>(); }

private:
    std::string url_;
    std::uint64_t id_ = 0;
};

// The RuntimeClient used for bootstrap and scenarios: transactions, airdrops,
// deploys (through the solana CLI) and state queries over JSON-RPC.
class RpcRuntime final : public RuntimeClient {
public:
    RpcRuntime(std::string url, std::int64_t confirm_timeout_ms, std::string solana_cli, fs::path keys_dir)
        : rpc_(std::move(url)), confirm_timeout_ms_(confirm_timeout_ms), solana_cli_(std::move(solana_cli)), keys_dir_(std::move(keys_dir)) {}

    JsonRpc& rpc() { return rpc_; }

    TxResult send_transaction(const TransactionRequest& req) override {
        std::vector<sol::Instruction> ixs;
        if (req.compute_unit_limit) ixs.push_back(sol::compute_unit_limit_ix(*req.compute_unit_limit));
        if (req.compute_unit_price) ixs.push_back(sol::compute_unit_price_ix(*req.compute_unit_price));
        ixs.insert(ixs.end(), req.instructions.begin(), req.instructions.end());
        Bytes tx;
        try {
            tx = sol::build_transaction(ixs, req.fee_payer, latest_blockhash(req.origin), req.signers);
        } catch (const std::invalid_argument& e) {
            fail(req.origin, std::string("cannot build transaction: ") + e.what());
        }
        if (tx.size() > sol::max_transaction_size)
            fail(req.origin, "transaction is " + std::to_string(tx.size()) + " bytes; the maximum is " +
                                 std::to_string(sol::max_transaction_size));
        return submit(sol::base64_encode(tx), req.origin);
    }

    TxResult airdrop(const Pubkey& to, std::uint64_t lamports, Loc origin) override {
        try {
            std::string sig = rpc_.call("requestAirdrop", {to.base58(), lamports, {{"commitment", "confirmed"}}}, origin).get<std::string>();
            return confirm(sig, origin);
        } catch (const RpcError& e) {
            return TxResult{.error = e.what()};
        }
    }

    TxResult deploy(const DeployRequest& req) override {
        fs::create_directories(keys_dir_);
        sol::Keypair payer = sol::Keypair::from_name("tide:deployer");
        fs::path payer_file = keys_dir_ / "deployer.json";
        fs::path program_file = keys_dir_ / (req.program.pubkey.base58() + ".json");
        write_file(payer_file, payer.to_json());
        write_file(program_file, req.program.to_json());
        // Fund the deployer: program buffers cost roughly 2x the binary's rent.
        std::uint64_t size = fs::file_size(req.binary);
        std::uint64_t need = sol::rent_exempt_minimum(size * 2) + 10 * sol::lamports_per_sol;
        auto acc = account(payer.pubkey);
        if (!acc || acc->lamports < need) {
            TxResult r = airdrop(payer.pubkey, need, req.origin);
            if (!r.success) return TxResult{.error = "funding the deployer failed: " + r.error};
        }
        int code = 0;
        std::string out = run_capture({solana_cli_, "program", "deploy", "--url", rpc_.url(), "--keypair", payer_file.string(),
                                       "--program-id", program_file.string(), "--commitment", "confirmed",
                                       req.binary.string()},
                                      code);
        if (code != 0) return TxResult{.error = "solana program deploy failed:\n" + out};
        return TxResult{.success = true};
    }

    std::optional<AccountView> account(const Pubkey& address) override { return rpc_.account(address); }
    std::uint64_t slot() override { return rpc_.slot("confirmed"); }
    std::uint64_t epoch() override { return rpc_.call("getEpochInfo", {{{"commitment", "confirmed"}}})["epoch"].get<std::uint64_t>(); }

private:
    sol::Hash latest_blockhash(Loc origin) {
        Json r = rpc_.call("getLatestBlockhash", {{{"commitment", "confirmed"}}}, origin);
        auto bytes = sol::base58_decode(r["value"]["blockhash"].get<std::string>());
        if (!bytes || bytes->size() != 32) fail(origin, "RPC returned an invalid blockhash");
        sol::Hash h{};
        std::copy(bytes->begin(), bytes->end(), h.begin());
        return h;
    }

    TxResult submit(const std::string& tx_b64, Loc origin) {
        std::string sig;
        try {
            sig = rpc_.call("sendTransaction", {tx_b64, {{"encoding", "base64"}, {"preflightCommitment", "confirmed"}}}, origin)
                      .get<std::string>();
        } catch (const RpcError& e) {
            TxResult r{.error = e.what()};
            if (e.data.is_object() && e.data.contains("logs") && e.data["logs"].is_array())
                for (const auto& l : e.data["logs"]) r.logs.push_back(l.get<std::string>());
            return r;
        }
        return confirm(sig, origin);
    }

    TxResult confirm(const std::string& sig, Loc origin) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(confirm_timeout_ms_);
        while (std::chrono::steady_clock::now() < deadline) {
            Json r = rpc_.call("getSignatureStatuses", {Json::array({sig}), {{"searchTransactionHistory", false}}}, origin);
            const Json& st = r["value"][0];
            if (!st.is_null()) {
                std::string level = st.value("confirmationStatus", "");
                if (level == "confirmed" || level == "finalized") {
                    bool ok = st["err"].is_null();
                    TxResult res{.success = ok, .signature = sig, .error = ok ? "" : st["err"].dump(), .slot = st.value("slot", 0ull)};
                    fetch_logs(res, origin);
                    return res;
                }
            }
            std::this_thread::sleep_for(confirm_poll_interval);
        }
        return TxResult{.signature = sig, .error = "transaction was not confirmed within " + std::to_string(confirm_timeout_ms_) + "ms"};
    }

    void fetch_logs(TxResult& res, Loc origin) {
        try {
            Json t = rpc_.call("getTransaction", {res.signature, {{"encoding", "json"}, {"commitment", "confirmed"}, {"maxSupportedTransactionVersion", 0}}}, origin);
            if (t.is_object() && t.contains("meta") && t["meta"].contains("logMessages") && t["meta"]["logMessages"].is_array())
                for (const auto& l : t["meta"]["logMessages"]) res.logs.push_back(l.get<std::string>());
        } catch (const std::exception&) {
            // Logs are best effort.
        }
    }

    JsonRpc rpc_;
    std::int64_t confirm_timeout_ms_;
    std::string solana_cli_;
    fs::path keys_dir_;
};

// Fetches an account from an arbitrary cluster (used for clones).
std::optional<AccountView> fetch_remote_account(const std::string& url, const Pubkey& address, Loc origin) {
    try {
        return JsonRpc(url).account(address, origin);
    } catch (const RpcError& e) {
        fail(origin, "fetching " + address.base58() + " from " + url + " failed: " + e.what());
    }
}

std::string resolve_cluster_url(const std::string& url) {
    if (url == "mainnet" || url == "mainnet-beta" || url == "m") return "https://api.mainnet-beta.solana.com";
    if (url == "devnet" || url == "d") return "https://api.devnet.solana.com";
    if (url == "testnet" || url == "t") return "https://api.testnet.solana.com";
    if (url == "localhost" || url == "l") return "http://127.0.0.1:8899";
    return url;
}

// --------------------------------------------------------------- genesis --

class Genesis final : public GenesisBuilder {
public:
    explicit Genesis(Diagnostics& diags) : diags_(diags) {}

    void put_account(const AccountSpec& s) override {
        insert(AccountView{s.address, s.lamports, s.owner, s.data, s.executable, s.rent_epoch}, s.origin);
    }

    // Single entry point for genesis accounts: refuses builtin program ids and
    // silent overwrites (e.g. of a program data account added by a clone).
    void insert(const AccountView& a, Loc origin) {
        if (a.address == sol::ids::system_program || a.address == sol::ids::token_program ||
            a.address == sol::ids::token_2022_program || a.address == sol::ids::associated_token_program)
            fail(origin, "cannot overwrite builtin program account " + a.address.base58());
        auto [it, inserted] = accounts_.try_emplace(a.address, a, origin);
        if (!inserted) {
            TideError err(origin, "account " + a.address.base58() + " is already part of the genesis state");
            err.notes.push_back({it->second.second, "it was added here"});
            throw err;
        }
    }

    void put_program(const ProgramSpec& s) override {
        if (s.loader == sol::ids::bpf_loader_deprecated || s.loader == sol::ids::loader_v4)
            fail(s.origin, "program '" + s.name + "': the solana-test-validator backend can only preload programs with "
                                                   "bpf_loader() or bpf_upgradeable_loader()");
        programs_.push_back(s);
    }

    void clone_account(const CloneSpec& s) override {
        std::string url = resolve_cluster_url(s.url);
        if (s.slot)
            diags_.warning(s.origin, "'at slot' is ignored: the solana-test-validator backend clones the current state of " + url);
        auto acc = fetch_remote_account(url, s.address, s.origin);
        if (!acc) fail(s.origin, "account " + s.address.base58() + " does not exist on " + url);
        if (s.program) {
            if (!acc->executable) fail(s.origin, s.address.base58() + " on " + url + " is not an executable program account");
            if (acc->owner == sol::ids::bpf_upgradeable_loader) {
                // UpgradeableLoaderState::Program { programdata_address }
                if (acc->data.size() < 36) fail(s.origin, "unexpected upgradeable program account layout");
                Pubkey pd = Pubkey::from_bytes(sol::ByteView(acc->data).subspan(4, 32));
                auto pdata = fetch_remote_account(url, pd, s.origin);
                if (!pdata) fail(s.origin, "program data account " + pd.base58() + " is missing on " + url);
                insert(*pdata, s.origin);
            }
        } else {
            const auto& o = s.overrides;
            if (o.lamports) acc->lamports = *o.lamports;
            if (o.owner) acc->owner = *o.owner;
            if (o.data) acc->data = *o.data;
            if (o.executable) acc->executable = *o.executable;
            if (o.rent_epoch) acc->rent_epoch = *o.rent_epoch;
        }
        insert(*acc, s.origin);
    }

    void remove_account(const DeleteSpec& s) override {
        if (accounts_.erase(s.address)) return;
        auto it = std::find_if(programs_.begin(), programs_.end(), [&](const ProgramSpec& p) { return p.id == s.address; });
        if (it == programs_.end()) fail(s.origin, "cannot delete " + s.address.base58() + ": not part of the genesis state");
        programs_.erase(it);
    }

    void set_feature(const FeatureSpec& s) override {
        if (s.enabled) deactivated_.erase(s.feature);
        else deactivated_[s.feature] = s.origin;
    }

    void set_sysvar(const SysvarSpec& s) override {
        auto num = [&](const std::string& k) -> std::uint64_t {
            const auto& v = s.fields.at(k);
            if (!std::holds_alternative<__int128>(v) || std::get<__int128>(v) < 0)
                fail(s.field_origins.at(k), k + " must be a non-negative integer");
            return static_cast<std::uint64_t>(std::get<__int128>(v));
        };
        for (const auto& [k, _] : s.fields) {
            Loc at = s.field_origins.at(k);
            if (s.name == "clock" && k == "slot") warp_slot_ = num(k);
            else if (s.name == "epoch_schedule" && k == "slots_per_epoch") slots_per_epoch_ = num(k);
            else if (s.name == "epoch_schedule" && k == "warmup") {
                if (std::get<bool>(s.fields.at(k)))
                    fail(at, "epoch warmup cannot be enabled with the solana-test-validator backend");
            } else {
                fail(at, "sysvar field '" + s.name + "." + k + "' cannot be set by the solana-test-validator backend "
                                                             "(supported: clock.slot, epoch_schedule.slots_per_epoch, epoch_schedule.warmup = false)");
            }
        }
    }

    void fork(const ForkSpec& s) override {
        if (s.slot) diags_.warning(s.origin, "'at slot' is ignored: the solana-test-validator backend copies the current feature set of the cluster");
        diags_.warning(s.origin, "fork copies the cluster's feature set only; clone the accounts you need explicitly");
        fork_url_ = resolve_cluster_url(s.url);
    }

    std::optional<AccountView> account(const Pubkey& address) const override {
        if (auto it = accounts_.find(address); it != accounts_.end()) return it->second.first;
        for (const auto& p : programs_)
            if (p.id == address) return AccountView{p.id, 1, p.loader, {}, true, 0};
        return std::nullopt;
    }

    // Accounts to preload, keyed by address, with the construct that added each.
    const std::map<Pubkey, std::pair<AccountView, Loc>>& accounts() const { return accounts_; }
    std::size_t program_count() const { return programs_.size(); }

    // Validator flags for everything genesis decided besides plain accounts.
    // Sysvar settings take precedence over the equivalent network options.
    void append_ledger_flags(std::vector<std::string>& a, const NetworkConfig& cfg) const {
        auto spe = slots_per_epoch_ ? slots_per_epoch_ : cfg.slots_per_epoch;
        if (spe) a.insert(a.end(), {"--slots-per-epoch", std::to_string(*spe)});
        if (cfg.ticks_per_slot) a.insert(a.end(), {"--ticks-per-slot", std::to_string(*cfg.ticks_per_slot)});
        auto warp = warp_slot_ ? warp_slot_ : cfg.warp_slot;
        if (warp) a.insert(a.end(), {"--warp-slot", std::to_string(*warp)});
        if (fork_url_) a.insert(a.end(), {"--url", *fork_url_, "--clone-feature-set"});
        for (const auto& [f, _] : deactivated_) a.insert(a.end(), {"--deactivate-feature", f.base58()});
        for (const auto& p : programs_) {
            if (p.loader == sol::ids::bpf_upgradeable_loader)
                a.insert(a.end(), {"--upgradeable-program", p.id.base58(), p.binary.string(),
                                   p.upgrade_authority ? p.upgrade_authority->base58() : "none"});
            else
                a.insert(a.end(), {"--bpf-program", p.id.base58(), p.binary.string()});
        }
    }

private:
    Diagnostics& diags_;
    std::map<Pubkey, std::pair<AccountView, Loc>> accounts_;
    std::vector<ProgramSpec> programs_;
    std::map<Pubkey, Loc> deactivated_;
    std::optional<std::uint64_t> warp_slot_, slots_per_epoch_;
    std::optional<std::string> fork_url_;
};

}  // namespace

// =============================================================== backend ==

struct SolanaTestValidatorBackend::Impl {
    SolanaBackendOptions opts;
    Diagnostics& diags;
    std::ostream& log;
    NetworkConfig cfg;
    Genesis gen;
    fs::path state_dir, ledger_dir, accounts_dir, keys_dir, log_file;
    std::uint16_t rpc_port = default_rpc_port;
    std::string bind_host = "127.0.0.1";
    pid_t bootstrap_pid = -1, network_pid = -1;
    bool ledger_initialized = false;  // the ledger already contains genesis (+ bootstrap)
    bool files_written = false;
    bool detached = false;
    std::optional<std::uint64_t> persisted_slot;  // last bootstrap slot, finalized before shutdown
    std::unique_ptr<RpcRuntime> client;

    Impl(SolanaBackendOptions o, Diagnostics& d, std::ostream& l) : opts(std::move(o)), diags(d), log(l), gen(d) {}

    // `validator { bind_address }` overrides `options { bind_address }`.
    std::optional<std::string> effective_bind_address() const {
        if (!cfg.validators.empty() && cfg.validators[0].bind_address) return cfg.validators[0].bind_address;
        return cfg.bind_address;
    }

    std::string rpc_url() const { return "http://" + bind_host + ":" + std::to_string(rpc_port); }

    std::chrono::milliseconds startup_timeout() const {
        return std::chrono::milliseconds(cfg.startup_timeout_ms.value_or(default_startup_timeout_ms));
    }

    void configure(const NetworkConfig& c) {
        cfg = c;
        if (cfg.validators.size() > 1) {
            TideError err(cfg.validators[1].origin, "the solana-test-validator backend runs a single validator, but network '" +
                                                        cfg.name + "' declares " + std::to_string(cfg.validators.size()));
            err.notes.push_back({cfg.validators[0].origin, "first validator is declared here"});
            throw err;
        }
        fs::path root = opts.state_root.empty() ? default_state_root(cfg.base_dir) : opts.state_root;
        state_dir = root / cfg.name;
        ledger_dir = cfg.ledger ? (fs::path(*cfg.ledger).is_absolute() ? fs::path(*cfg.ledger) : cfg.base_dir / *cfg.ledger)
                                : state_dir / "ledger";
        accounts_dir = state_dir / "accounts";
        keys_dir = state_dir / "keys";
        log_file = state_dir / "validator.log";
        if (!cfg.validators.empty()) {
            const auto& v = cfg.validators[0];
            if (v.identity)
                fail(v.origin, "validator 'identity' cannot be set with the solana-test-validator backend (it generates its own identity)");
            if (v.rpc_port) rpc_port = *v.rpc_port;
        }
        if (auto bind = effective_bind_address(); bind && *bind != "0.0.0.0") bind_host = *bind;
        if (opts.dry_run) return;

        if (!find_in_path(opts.validator_binary))
            fail(cfg.origin, "'" + opts.validator_binary + "' was not found in PATH; install the Agave tools or pass --validator");
        if (auto pid = read_pid(); pid && process_alive(*pid))
            fail(cfg.origin, "network '" + cfg.name + "' is already running (pid " + std::to_string(*pid) +
                                 "); stop it first with 'tide down'");
        auto check_port = [&](std::optional<std::uint16_t> p, std::uint16_t dflt, const char* what) {
            std::uint16_t port = p.value_or(dflt);
            if (port_in_use(port)) {
                Loc at = cfg.validators.empty() ? cfg.origin : cfg.validators[0].origin;
                fail(at, std::string(what) + " " + std::to_string(port) + " is already in use (is another validator running?)");
            }
        };
        const ValidatorConfig* v = cfg.validators.empty() ? nullptr : &cfg.validators[0];
        check_port(v ? v->rpc_port : std::nullopt, default_rpc_port, "RPC port");
        check_port(v ? v->faucet_port : std::nullopt, default_faucet_port, "faucet port");
    }

    std::optional<pid_t> read_pid() const {
        fs::path p = state_dir / "validator.pid";
        if (!fs::exists(p)) return std::nullopt;
        try {
            return static_cast<pid_t>(std::stol(read_file(p)));
        } catch (...) {
            return std::nullopt;
        }
    }

    void write_genesis_files() {
        if (files_written) return;
        fs::create_directories(state_dir);
        fs::remove_all(accounts_dir);
        fs::create_directories(accounts_dir);
        for (const auto& [addr, entry] : gen.accounts()) {
            const AccountView& a = entry.first;
            Json j = {{"pubkey", addr.base58()},
                      {"account",
                       {{"lamports", a.lamports},
                        {"data", {sol::base64_encode(a.data), "base64"}},
                        {"owner", a.owner.base58()},
                        {"executable", a.executable},
                        {"rentEpoch", a.rent_epoch},
                        {"space", a.data.size()}}}};
            write_file(accounts_dir / (addr.base58() + ".json"), j.dump(2));
        }
        files_written = true;
    }

    std::vector<std::string> base_args() const {
        std::vector<std::string> a = {opts.validator_binary, "--ledger", ledger_dir.string(), "--rpc-port", std::to_string(rpc_port)};
        if (!cfg.validators.empty()) {
            const auto& v = cfg.validators[0];
            if (v.gossip_port) a.insert(a.end(), {"--gossip-port", std::to_string(*v.gossip_port)});
            if (v.faucet_port) a.insert(a.end(), {"--faucet-port", std::to_string(*v.faucet_port)});
            if (v.dynamic_port_range) a.insert(a.end(), {"--dynamic-port-range", *v.dynamic_port_range});
        }
        if (auto bind = effective_bind_address()) a.insert(a.end(), {"--bind-address", *bind});
        if (cfg.compute_unit_limit) a.insert(a.end(), {"--compute-unit-limit", std::to_string(*cfg.compute_unit_limit)});
        if (cfg.limit_ledger_size) a.insert(a.end(), {"--limit-ledger-size", std::to_string(*cfg.limit_ledger_size)});
        if (cfg.faucet_sol) {
            std::ostringstream ss;
            ss << *cfg.faucet_sol;
            a.insert(a.end(), {"--faucet-sol", ss.str()});
        }
        return a;
    }

    // Flags that only take effect when the ledger is created.
    std::vector<std::string> genesis_args() const {
        std::vector<std::string> a = {"--reset", "--account-dir", accounts_dir.string()};
        gen.append_ledger_flags(a, cfg);
        return a;
    }

    // Full command line. Genesis flags only take effect when the ledger is
    // created, so they are added for the first start only.
    std::vector<std::string> validator_args(bool with_genesis) const {
        auto args = base_args();
        if (with_genesis) {
            auto g = genesis_args();
            args.insert(args.end(), g.begin(), g.end());
        }
        return args;
    }

    static std::string shell_join(const std::vector<std::string>& args) {
        std::string s;
        for (const auto& a : args) {
            bool plain = a.find_first_of(" \t'\"$\\") == std::string::npos && !a.empty();
            s += (s.empty() ? "" : " ") + (plain ? a : "'" + a + "'");
        }
        return s;
    }

    pid_t start_validator(const std::vector<std::string>& args, const char* role) {
        fs::create_directories(state_dir);
        { std::ofstream(log_file, std::ios::app) << "\n=== tide: starting " << role << " instance ===\n" << shell_join(args) << "\n"; }
        log << "    starting solana-test-validator (" << role << ", rpc " << rpc_url() << ")\n";
        pid_t pid = spawn_process(args, log_file);
        wait_until_healthy(pid, role);
        return pid;
    }

    void wait_until_healthy(pid_t pid, const char* role) {
        JsonRpc probe(rpc_url());
        auto timeout = startup_timeout();
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == pid)
                fail(cfg.origin, std::string("solana-test-validator (") + role + ") exited during startup; last log lines:\n" +
                                     tail_lines(log_file, 15));
            if (probe.healthy()) return;
            std::this_thread::sleep_for(health_poll_interval);
        }
        stop_process(pid, SIGTERM, abort_stop_timeout);
        fail(cfg.origin, std::string("solana-test-validator (") + role + ") did not become healthy within " +
                             std::to_string(timeout.count()) + "ms; see " + log_file.string());
    }

    RpcRuntime& make_client() {
        client = std::make_unique<RpcRuntime>(rpc_url(), cfg.confirm_timeout_ms.value_or(default_confirm_timeout_ms),
                                             opts.solana_cli, keys_dir);
        return *client;
    }

    RuntimeClient& start_bootstrap() {
        write_genesis_files();
        bootstrap_pid = start_validator(validator_args(true), "bootstrap");
        ledger_initialized = true;
        return make_client();
    }

    void persist() {
        if (bootstrap_pid > 0) {
            // Bootstrap effects must be rooted before shutdown so the resumed
            // ledger provably contains them.
            std::uint64_t target = client->slot();
            log << "    waiting for bootstrap state to finalize (slot " << target << ")\n";
            wait_for_slot(client->rpc(), "finalized", target, finalize_timeout, "bootstrap state did not finalize");
            persisted_slot = target;
            log << "    stopping bootstrap instance\n";
            stop_process(bootstrap_pid, SIGINT, graceful_stop_timeout);
            bootstrap_pid = -1;
        } else {
            write_genesis_files();
        }
        Json manifest = {{"network", cfg.name},
                         {"rpc_url", rpc_url()},
                         {"ledger", ledger_dir.string()},
                         {"accounts", gen.accounts().size()},
                         {"programs", gen.program_count()},
                         {"bootstrapped", ledger_initialized}};
        Json accounts = Json::array();
        for (const auto& [addr, e] : gen.accounts())
            accounts.push_back({{"address", addr.base58()}, {"owner", e.first.owner.base58()}, {"lamports", e.first.lamports}});
        manifest["genesis_accounts"] = accounts;
        if (opts.dry_run) {
            auto args = validator_args(true);
            manifest["command"] = args;
            log << "    state written to " << state_dir.string() << "\n    launch command: " << shell_join(args) << "\n";
        }
        write_file(state_dir / "manifest.json", manifest.dump(2));
    }

    RuntimeClient& launch() {
        network_pid = start_validator(validator_args(!ledger_initialized), "network");
        write_file(state_dir / "validator.pid", std::to_string(network_pid));
        RpcRuntime& c = make_client();
        if (persisted_slot) {
            // A resumed validator answers health checks while still replaying.
            wait_for_slot(c.rpc(), "confirmed", *persisted_slot, startup_timeout(),
                          "the relaunched validator did not replay the persisted ledger");
        }
        return c;
    }

    void wait_for_slot(JsonRpc& c, const char* commitment, std::uint64_t target, std::chrono::milliseconds timeout,
                       const char* what) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (c.slot(commitment) < target) {
            if (std::chrono::steady_clock::now() > deadline)
                fail(cfg.origin, std::string(what) + " within " + std::to_string(timeout.count()) + "ms (target slot " +
                                     std::to_string(target) + ")");
            std::this_thread::sleep_for(slot_poll_interval);
        }
    }

    LaunchInfo detach() {
        detached = true;
        return {rpc_url(), state_dir.string(), network_pid > 0 ? std::optional<int>(network_pid) : std::nullopt};
    }

    void abort() noexcept {
        try {
            if (bootstrap_pid > 0) stop_process(bootstrap_pid, SIGTERM, abort_stop_timeout);
            bootstrap_pid = -1;
            if (network_pid > 0 && !detached) {
                stop_process(network_pid, SIGTERM, abort_stop_timeout);
                std::error_code ec;
                fs::remove(state_dir / "validator.pid", ec);
            }
            network_pid = -1;
        } catch (...) {
        }
    }
};

SolanaTestValidatorBackend::SolanaTestValidatorBackend(SolanaBackendOptions options, Diagnostics& diags, std::ostream& log)
    : impl_(std::make_unique<Impl>(std::move(options), diags, log)) {}
SolanaTestValidatorBackend::~SolanaTestValidatorBackend() {
    if (!impl_->detached) impl_->abort();
}
void SolanaTestValidatorBackend::configure(const NetworkConfig& config) { impl_->configure(config); }
GenesisBuilder& SolanaTestValidatorBackend::genesis() { return impl_->gen; }
RuntimeClient& SolanaTestValidatorBackend::start_bootstrap() { return impl_->start_bootstrap(); }
void SolanaTestValidatorBackend::persist() { impl_->persist(); }
RuntimeClient& SolanaTestValidatorBackend::launch() { return impl_->launch(); }
LaunchInfo SolanaTestValidatorBackend::detach() { return impl_->detach(); }
void SolanaTestValidatorBackend::abort() noexcept { impl_->abort(); }

bool stop_network(const fs::path& state_dir, std::ostream& log, std::string& error) {
    fs::path pid_file = state_dir / "validator.pid";
    if (!fs::exists(pid_file)) {
        error = "no running network found in " + state_dir.string();
        return false;
    }
    pid_t pid = 0;
    try {
        pid = static_cast<pid_t>(std::stol(read_file(pid_file)));
    } catch (...) {
        error = "corrupt pid file " + pid_file.string();
        return false;
    }
    if (process_alive(pid)) {
        log << "stopping validator (pid " << pid << ")\n";
        stop_process(pid, SIGINT, down_stop_timeout);
    } else {
        log << "validator (pid " << pid << ") was not running\n";
    }
    fs::remove(pid_file);
    return true;
}

}  // namespace tide::backend
