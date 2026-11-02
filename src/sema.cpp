#include "sema.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "ast.hpp"
#include "diag.hpp"
#include "parser.hpp"
#include "solana.hpp"

namespace tide::sema {

using ast::AirdropStmt;
using ast::AssertStmt;
using ast::AssignOp;
using ast::AssignStmt;
using ast::AwaitStmt;
using ast::BinaryExpr;
using ast::BinaryOp;
using ast::BindingKind;
using ast::Block;
using ast::BoolLit;
using ast::BootstrapEntry;
using ast::BreakStmt;
using ast::BuiltinId;
using ast::CallExpr;
using ast::CloneDecl;
using ast::compound_op;
using ast::ContinueStmt;
using ast::DeleteDecl;
using ast::DeployStmt;
using ast::Document;
using ast::EntityDecl;
using ast::EntityKind;
using ast::Expr;
using ast::ExprStmt;
using ast::FeatureDecl;
using ast::ForkDecl;
using ast::ForStmt;
using ast::FunctionDecl;
using ast::GenesisEntry;
using ast::Ident;
using ast::IdentExpr;
using ast::IfStmt;
using ast::ImportDecl;
using ast::IndexExpr;
using ast::InvokeStmt;
using ast::LetStmt;
using ast::ListLit;
using ast::MemberExpr;
using ast::NetworkDecl;
using ast::NullLit;
using ast::NumberLit;
using ast::ObjectLit;
using ast::OptionsEntry;
using ast::RecordBlock;
using ast::RecordEntry;
using ast::RestoreStmt;
using ast::ReturnStmt;
using ast::ScenarioEntry;
using ast::SchemaDecl;
using ast::SnapshotStmt;
using ast::Stmt;
using ast::StringLit;
using ast::SysvarDecl;
using ast::TransactionStmt;
using ast::TypeExpr;
using ast::UnaryExpr;
using ast::UnaryOp;
using ast::Unit;
using ast::ValidatorEntry;
using ast::WarpKind;
using ast::WarpStmt;
using ast::WhileStmt;

// ============================================================ TypeContext ==

TypeContext::TypeContext() {
    auto prim = [&](TypeKind k) { return make(Type{k}); };
    error_ = prim(TypeKind::error);
    any_ = prim(TypeKind::any);
    void_t_ = prim(TypeKind::void_);
    null_ = prim(TypeKind::null);
    bool_t_ = prim(TypeKind::bool_);
    float_t_ = prim(TypeKind::float_);
    {
        Type f{TypeKind::float_};
        f.is_f32 = true;
        float32_ = make(std::move(f));
    }
    string_ = prim(TypeKind::string);
    bytes_ = prim(TypeKind::bytes);
    duration_ = prim(TypeKind::duration);
    pubkey_ = prim(TypeKind::pubkey);
    keypair_ = prim(TypeKind::keypair);
    source_ = prim(TypeKind::source);
    range_ = prim(TypeKind::range);
    meta_ = prim(TypeKind::account_meta);
    for (const IntWidth w : all_int_widths) {
        Type t{TypeKind::int_};
        t.width = w;
        ints_.at(static_cast<std::size_t>(w)) = make(std::move(t));
    }
    account_info_ = record({{"address", pubkey_},
                            {"lamports", int_(IntWidth::u64)},
                            {"owner", pubkey_},
                            {"data", bytes_},
                            {"executable", bool_t_},
                            {"rent_epoch", int_(IntWidth::u64)}});
    tx_result_ = record({{"success", bool_t_},
                         {"signature", string_},
                         {"error", string_},
                         {"logs", list(string_)},
                         {"slot", int_(IntWidth::u64)}});
}

const Type* TypeContext::make(Type t) {
    arena_.push_back(std::move(t));
    return &arena_.back();
}
const Type* TypeContext::list(const Type* elem, std::optional<std::uint64_t> len) {
    Type t{TypeKind::list};
    t.elem = elem;
    t.fixed_len = len;
    return make(std::move(t));
}
const Type* TypeContext::option(const Type* inner) {
    if (inner->kind == TypeKind::option || inner->kind == TypeKind::error) return inner;
    Type t{TypeKind::option};
    t.elem = inner;
    return make(std::move(t));
}
const Type* TypeContext::record(std::vector<std::pair<std::string, const Type*>> fields, const Symbol* schema) {
    Type t{TypeKind::record};
    t.fields = std::move(fields);
    t.schema = schema;
    return make(std::move(t));
}
const Type* TypeContext::tuple(std::vector<const Type*> elems) {
    Type t{TypeKind::tuple};
    t.elems = std::move(elems);
    return make(std::move(t));
}
const Type* TypeContext::schema(const Symbol* s) {
    Type t{TypeKind::schema};
    t.schema = s;
    return make(std::move(t));
}
const Type* TypeContext::module(const Module* m) {
    Type t{TypeKind::module};
    t.module = m;
    return make(std::move(t));
}

std::string to_string(IntWidth w) {
    switch (w) {
        case IntWidth::any: return "int";
        case IntWidth::u8: return "u8";
        case IntWidth::u16: return "u16";
        case IntWidth::u32: return "u32";
        case IntWidth::u64: return "u64";
        case IntWidth::u128: return "u128";
        case IntWidth::i8: return "i8";
        case IntWidth::i16: return "i16";
        case IntWidth::i32: return "i32";
        case IntWidth::i64: return "i64";
        case IntWidth::i128: return "i128";
    }
    return "int";
}

std::pair<i128, i128> int_bounds(IntWidth w) {
    constexpr i128 i128_max = ~(i128{1} << 127);
    constexpr i128 i128_min = -i128_max - 1;
    switch (w) {
        case IntWidth::u8: return {0, 0xff};
        case IntWidth::u16: return {0, 0xffff};
        case IntWidth::u32: return {0, 0xffffffffLL};
        case IntWidth::u64: return {0, static_cast<i128>(UINT64_MAX)};
        case IntWidth::u128: return {0, i128_max};  // values are 128-bit signed at runtime
        case IntWidth::i8: return {INT8_MIN, INT8_MAX};
        case IntWidth::i16: return {INT16_MIN, INT16_MAX};
        case IntWidth::i32: return {INT32_MIN, INT32_MAX};
        case IntWidth::i64: return {INT64_MIN, INT64_MAX};
        case IntWidth::i128:
        case IntWidth::any: return {i128_min, i128_max};
    }
    return {i128_min, i128_max};
}

std::string to_string(const Type* t) {
    if (!t) return "<none>";
    switch (t->kind) {
        case TypeKind::error: return "<error>";
        case TypeKind::any: return "any";
        case TypeKind::void_: return "void";
        case TypeKind::null: return "null";
        case TypeKind::bool_: return "bool";
        case TypeKind::int_: return to_string(t->width);
        case TypeKind::float_: return t->is_f32 ? "f32" : "float";
        case TypeKind::string: return "string";
        case TypeKind::bytes: return "bytes";
        case TypeKind::duration: return "duration";
        case TypeKind::pubkey: return "pubkey";
        case TypeKind::keypair: return "keypair";
        case TypeKind::source: return "source";
        case TypeKind::range: return "range";
        case TypeKind::account_meta: return "account_meta";
        case TypeKind::list:
            return t->fixed_len ? "[" + to_string(t->elem) + "; " + std::to_string(*t->fixed_len) + "]"
                                : "[" + to_string(t->elem) + "]";
        case TypeKind::option: return "Option<" + to_string(t->elem) + ">";
        case TypeKind::record: {
            if (t->schema) return t->schema->name;
            std::string s = "{";
            for (std::size_t i = 0; i < t->fields.size(); ++i)
                s += ((i != 0U) ? ", " : "") + t->fields[i].first + ": " + to_string(t->fields[i].second);
            return s + "}";
        }
        case TypeKind::tuple: {
            std::string s = "(";
            for (std::size_t i = 0; i < t->elems.size(); ++i) s += ((i != 0U) ? ", " : "") + to_string(t->elems[i]);
            return s + ")";
        }
        case TypeKind::schema: return "schema " + t->schema->name;
        case TypeKind::module: return "module";
    }
    return "?";
}

const ast::NetworkDecl* Program::find_network(std::string_view name) const {
    for (const auto* n : networks())
        if (n->name.name == name) return n;
    return nullptr;
}

std::vector<const ast::NetworkDecl*> Program::networks() const {
    std::vector<const ast::NetworkDecl*> out;
    for (const auto& d : root()->doc->decls)
        if (const auto* n = std::get_if<NetworkDecl>(&d.node)) out.push_back(n);
    return out;
}

// =============================================================== builtins ==

namespace {

enum class Effect : std::uint8_t { pure, state, runtime_only };

struct BuiltinDef {
    std::string_view name;
    BuiltinId id;
    Effect effect;
};

constexpr auto builtin_table = std::to_array<BuiltinDef>({
    {"seed", BuiltinId::seed, Effect::pure},
    {"keypair_file", BuiltinId::keypair_file, Effect::pure},
    {"pubkey", BuiltinId::pubkey, Effect::pure},
    {"pda", BuiltinId::pda, Effect::pure},
    {"pda_bump", BuiltinId::pda_bump, Effect::pure},
    {"ata", BuiltinId::ata, Effect::pure},
    {"system_program", BuiltinId::system_program, Effect::pure},
    {"token_program", BuiltinId::token_program, Effect::pure},
    {"token_2022_program", BuiltinId::token_2022_program, Effect::pure},
    {"associated_token_program", BuiltinId::associated_token_program, Effect::pure},
    {"bpf_loader", BuiltinId::bpf_loader, Effect::pure},
    {"bpf_loader_deprecated", BuiltinId::bpf_loader_deprecated, Effect::pure},
    {"bpf_upgradeable_loader", BuiltinId::bpf_upgradeable_loader, Effect::pure},
    {"loader_v4", BuiltinId::loader_v4, Effect::pure},
    {"memo_program", BuiltinId::memo_program, Effect::pure},
    {"compute_budget_program", BuiltinId::compute_budget_program, Effect::pure},
    {"address_lookup_table_program", BuiltinId::address_lookup_table_program, Effect::pure},
    {"hex", BuiltinId::hex, Effect::pure},
    {"base64", BuiltinId::base64, Effect::pure},
    {"base58", BuiltinId::base58, Effect::pure},
    {"utf8", BuiltinId::utf8, Effect::pure},
    {"sha256", BuiltinId::sha256, Effect::pure},
    {"file", BuiltinId::file, Effect::pure},
    {"u8", BuiltinId::u8, Effect::pure},
    {"u16_le", BuiltinId::u16_le, Effect::pure},
    {"u32_le", BuiltinId::u32_le, Effect::pure},
    {"u64_le", BuiltinId::u64_le, Effect::pure},
    {"i64_le", BuiltinId::i64_le, Effect::pure},
    {"borsh", BuiltinId::borsh, Effect::pure},
    {"anchor", BuiltinId::anchor, Effect::pure},
    {"decode", BuiltinId::decode, Effect::pure},
    {"len", BuiltinId::len, Effect::pure},
    {"str", BuiltinId::str, Effect::pure},
    {"print", BuiltinId::print, Effect::pure},
    {"rent_exempt", BuiltinId::rent_exempt, Effect::pure},
    {"rpc", BuiltinId::rpc, Effect::pure},
    {"readonly", BuiltinId::readonly, Effect::pure},
    {"writable", BuiltinId::writable, Effect::pure},
    {"signer", BuiltinId::signer, Effect::pure},
    {"sol", BuiltinId::sol, Effect::pure},
    {"system_transfer", BuiltinId::system_transfer, Effect::pure},
    {"spl_transfer", BuiltinId::spl_transfer, Effect::pure},
    {"spl_transfer_checked", BuiltinId::spl_transfer_checked, Effect::pure},
    {"spl_mint_to", BuiltinId::spl_mint_to, Effect::pure},
    {"memo", BuiltinId::memo, Effect::pure},
    {"balance", BuiltinId::balance, Effect::state},
    {"token_balance", BuiltinId::token_balance, Effect::state},
    {"account_info", BuiltinId::account, Effect::state},
    {"exists", BuiltinId::exists, Effect::state},
    {"slot", BuiltinId::slot, Effect::runtime_only},
    {"epoch", BuiltinId::epoch, Effect::runtime_only},
});

const BuiltinDef* find_builtin(std::string_view name) {
    for (const auto& b : builtin_table)
        if (b.name == name) return &b;
    return nullptr;
}

}  // namespace

const char* builtin_name(BuiltinId id) {
    for (const auto& b : builtin_table)
        if (b.id == id) return b.name.data();
    return "<builtin>";
}

// ================================================================ checker ==

namespace {

enum class Ctx : std::uint8_t { top_level, function, config, genesis, runtime };

// Field/key specifications for record-like blocks.
enum class Ty : std::uint8_t {
    any,
    int_,
    u8,
    u16,
    u32,
    u64,
    float_,
    number,
    bool_,
    string,
    bytes,
    pubkey,
    opt_pubkey,
    keypair,
    source,
    duration,
    list_pubkey,
    list_keypair,
    list_meta,
    record,
};

struct FieldSpec {
    std::string_view name;
    Ty type;
    bool required = false;
};

struct ExtSpec {
    std::string_view name;
    std::span<const FieldSpec> fields;
};

struct BlockSpec {
    std::string_view what;
    std::span<const FieldSpec> fields;
    std::span<const ExtSpec> extensions;
};

// --- genesis entities
constexpr auto account_fields = std::to_array<FieldSpec>({
    {"lamports", Ty::u64, true},
    {"owner", Ty::pubkey},
    {"data", Ty::bytes},
    {"executable", Ty::bool_},
    {"rent_epoch", Ty::u64},
});
constexpr auto clone_override_fields = std::to_array<FieldSpec>({
    {"lamports", Ty::u64},
    {"owner", Ty::pubkey},
    {"data", Ty::bytes},
    {"executable", Ty::bool_},
    {"rent_epoch", Ty::u64},
});
constexpr auto program_fields = std::to_array<FieldSpec>({
    {"id", Ty::pubkey, true},
    {"binary", Ty::string, true},
    {"loader", Ty::pubkey},
    {"upgrade_authority", Ty::opt_pubkey},
    {"idl", Ty::string},
});
constexpr auto mint_fields = std::to_array<FieldSpec>({
    {"token_program", Ty::pubkey},
    {"decimals", Ty::u8, true},
    {"mint_authority", Ty::opt_pubkey},
    {"freeze_authority", Ty::opt_pubkey},
    {"supply", Ty::u64},
    {"lamports", Ty::u64},
});
constexpr auto token_account_fields = std::to_array<FieldSpec>({
    {"token_program", Ty::pubkey},
    {"mint", Ty::pubkey, true},
    {"owner", Ty::pubkey, true},
    {"amount", Ty::u64},
    {"delegate", Ty::opt_pubkey},
    {"delegated_amount", Ty::u64},
    {"state", Ty::string},
    {"close_authority", Ty::opt_pubkey},
    {"lamports", Ty::u64},
});
constexpr auto ata_fields = std::to_array<FieldSpec>({
    {"token_program", Ty::pubkey},
    {"mint", Ty::pubkey, true},
    {"owner", Ty::pubkey, true},
    {"amount", Ty::u64},
    {"lamports", Ty::u64},
});
constexpr auto lookup_table_fields = std::to_array<FieldSpec>({
    {"authority", Ty::opt_pubkey},
    {"addresses", Ty::list_pubkey},
    {"deactivation_slot", Ty::u64},
    {"last_extended_slot", Ty::u64},
    {"lamports", Ty::u64},
});

constexpr auto ext_close_authority = std::to_array<FieldSpec>({{"close_authority", Ty::opt_pubkey}});
constexpr auto ext_permanent_delegate = std::to_array<FieldSpec>({{"delegate", Ty::opt_pubkey}});
constexpr auto ext_metadata_pointer =
    std::to_array<FieldSpec>({{"authority", Ty::opt_pubkey}, {"metadata_address", Ty::opt_pubkey}});
constexpr auto ext_default_state = std::to_array<FieldSpec>({{"state", Ty::string, true}});
constexpr auto mint_extensions = std::to_array<ExtSpec>({
    {"mint_close_authority", ext_close_authority},
    {"permanent_delegate", ext_permanent_delegate},
    {"non_transferable", {}},
    {"metadata_pointer", ext_metadata_pointer},
    {"default_account_state", ext_default_state},
});
constexpr auto token_account_extensions = std::to_array<ExtSpec>({{"immutable_owner", {}}});

// --- sysvars
constexpr auto clock_fields = std::to_array<FieldSpec>({
    {"slot", Ty::u64},
    {"epoch", Ty::u64},
    {"unix_timestamp", Ty::int_},
    {"epoch_start_timestamp", Ty::int_},
    {"leader_schedule_epoch", Ty::u64},
});
constexpr auto rent_fields = std::to_array<FieldSpec>({
    {"lamports_per_byte_year", Ty::u64},
    {"exemption_threshold", Ty::number},
    {"burn_percent", Ty::u8},
});
constexpr auto epoch_schedule_fields = std::to_array<FieldSpec>({
    {"slots_per_epoch", Ty::u64},
    {"leader_schedule_slot_offset", Ty::u64},
    {"warmup", Ty::bool_},
    {"first_normal_epoch", Ty::u64},
    {"first_normal_slot", Ty::u64},
});
struct SysvarSpec {
    std::string_view name;
    std::span<const FieldSpec> fields;
};
constexpr auto sysvar_specs = std::to_array<SysvarSpec>({
    {"clock", clock_fields},
    {"rent", rent_fields},
    {"epoch_schedule", epoch_schedule_fields},
});

// --- network configuration
constexpr auto option_fields = std::to_array<FieldSpec>({
    {"slots_per_epoch", Ty::u64},
    {"ticks_per_slot", Ty::u64},
    {"ledger", Ty::string},
    {"faucet_sol", Ty::number},
    {"compute_unit_limit", Ty::u64},
    {"limit_ledger_size", Ty::u64},
    {"warp_slot", Ty::u64},
    {"bind_address", Ty::string},
    {"startup_timeout", Ty::duration},
    {"confirm_timeout", Ty::duration},
});
constexpr auto validator_fields = std::to_array<FieldSpec>({
    {"rpc_port", Ty::u16},
    {"gossip_port", Ty::u16},
    {"faucet_port", Ty::u16},
    {"dynamic_port_range", Ty::string},
    {"identity", Ty::keypair},
    {"bind_address", Ty::string},
});

// --- runtime
constexpr auto tx_fields = std::to_array<FieldSpec>({
    {"fee_payer", Ty::pubkey},
    {"signers", Ty::list_keypair},
    {"compute_unit_limit", Ty::u32},
    {"compute_unit_price", Ty::u64},
});
constexpr auto instruction_fields = std::to_array<FieldSpec>({{"accounts", Ty::list_meta}, {"data", Ty::bytes}});
constexpr auto invoke_fields = std::to_array<FieldSpec>({
    {"method", Ty::string, true},
    {"accounts", Ty::record},
    {"args", Ty::record},
    {"signers", Ty::list_keypair},
    {"fee_payer", Ty::pubkey},
    {"idl", Ty::string},
    {"compute_unit_limit", Ty::u32},
    {"remaining_accounts", Ty::list_meta},
});

struct Scope {
    Scope* parent = nullptr;
    std::map<std::string, Symbol*, std::less<>> names;
};

class Checker {
public:
    Checker(Program& prog, SourceManager& sm, Diagnostics& diags)
        : prog_(prog), types_(prog.types), sm_(sm), diags_(diags) {}

    Module* load_root(Document& doc) {
        auto m = std::make_unique<Module>();
        m->doc = &doc;
        m->dir = doc.file ? doc.file->path.parent_path() : std::filesystem::current_path();
        Module* raw = m.get();
        prog_.modules.push_back(std::move(m));
        if (doc.file) {
            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(doc.file->path, ec);
            loaded_[ec ? doc.file->path : canon] = raw;
            in_progress_.insert(ec ? doc.file->path : canon);
        }
        check_module(*raw);
        return raw;
    }

private:
    // ------------------------------------------------------------ scopes --

    struct ScopeGuard {
        Checker& c;
        Scope scope;
        explicit ScopeGuard(Checker& ch) : c(ch) {
            scope.parent = c.scope_;
            c.scope_ = &scope;
        }
        ~ScopeGuard() { c.scope_ = scope.parent; }
        ScopeGuard(const ScopeGuard&) = delete;
        ScopeGuard& operator=(const ScopeGuard&) = delete;
        ScopeGuard(ScopeGuard&&) = delete;
        ScopeGuard& operator=(ScopeGuard&&) = delete;
    };

    Symbol* new_symbol(SymbolKind kind, const Ident& name, const Type* type) {
        Symbol& s = prog_.symbols.emplace_back();
        s.kind = kind;
        s.name = name.name;
        s.loc = name.loc;
        s.type = type;
        s.owner = module_;
        return &s;
    }

    Symbol* declare(SymbolKind kind, const Ident& name, const Type* type) {
        Symbol* s = new_symbol(kind, name, type);
        auto [it, inserted] = scope_->names.try_emplace(name.name, s);
        if (!inserted) {
            diags_.error(name.loc, "redeclaration of '" + name.name + "'")
                .notes.emplace_back(it->second->loc, "previous declaration is here");
        }
        return s;
    }

    // Records `key` at `loc`. A repeat reports `msg` with a note at the first
    // occurrence. Returns whether `key` was new.
    bool check_unique(std::map<std::string, Loc>& seen, const std::string& key, Loc loc, const std::string& msg,
                      const char* note = "previous definition is here") {
        auto [it, inserted] = seen.try_emplace(key, loc);
        if (!inserted) diags_.error(loc, msg).notes.emplace_back(it->second, note);
        return inserted;
    }

    [[nodiscard]] Symbol* lookup(std::string_view name) const {
        for (Scope* s = scope_; s; s = s->parent)
            if (auto it = s->names.find(name); it != s->names.end()) return it->second;
        return nullptr;
    }

    [[nodiscard]] std::vector<std::string> visible_names() const {
        std::vector<std::string> out;
        for (Scope* s = scope_; s; s = s->parent)
            for (const auto& [n, _] : s->names) out.push_back(n);
        return out;
    }

    // ----------------------------------------------------------- modules --

    void check_module(Module& m) {
        Scope global;
        const Restore in_module(module_, &m);
        const Restore in_global(scope_, &global);
        const Restore at_top(ctx_, Ctx::top_level);

        std::map<std::string, Loc> networks;
        for (auto& d : m.doc->decls) {
            std::visit(Overloaded{
                           [&](ImportDecl& i) { check_import(i); },
                           [&](LetStmt& l) { check_binding(l, /*global=*/true); },
                           [&](SchemaDecl& s) { check_schema(s); },
                           [&](FunctionDecl& f) { check_function(f); },
                           [&](NetworkDecl& n) {
                               check_unique(networks, n.name.name, n.name.loc,
                                            "duplicate network '" + n.name.name + "'");
                               check_network(n);
                           },
                       },
                       d.node);
        }
        for (const auto& [name, sym] : global.names) m.exports[name] = sym;
        prog_.init_order.push_back(&m);
    }

    void check_import(ImportDecl& imp) {
        const std::filesystem::path p = module_->dir / imp.path;
        std::error_code ec;
        auto canon = std::filesystem::weakly_canonical(p, ec);
        if (ec) canon = p;
        Module* target = nullptr;
        if (auto it = loaded_.find(canon); it != loaded_.end()) {
            target = it->second;
            if (in_progress_.contains(canon)) {
                diags_.error(imp.path_loc, "import cycle: '" + imp.path + "' is already being imported");
                target = nullptr;
            }
        } else {
            const SourceFile* file = sm_.load(canon);
            if (!file) {
                diags_.error(imp.path_loc, "cannot open imported file '" + canon.string() + "'");
            } else if (auto doc = parse_document(*file, diags_)) {
                auto owned = std::make_unique<Document>(std::move(*doc));
                auto m = std::make_unique<Module>();
                m->doc = owned.get();
                m->dir = canon.parent_path();
                target = m.get();
                prog_.imported_docs.push_back(std::move(owned));
                prog_.modules.push_back(std::move(m));
                loaded_[canon] = target;
                in_progress_.insert(canon);
                check_module(*target);
                in_progress_.erase(canon);
            }
        }
        imp.module = target;
        Symbol* s = declare(SymbolKind::module, imp.alias, target ? types_.module(target) : types_.error());
        s->module = target;
        s->is_global = true;
        imp.sym = s;
    }

    // ---------------------------------------------------------- bindings --

    void check_binding(LetStmt& l, bool global) {
        const Type* type = nullptr;
        switch (l.kind) {
            case BindingKind::let: {
                if (l.annotation) {
                    const Type* ann = resolve_type(*l.annotation, false);
                    expect(*l.init, ann, "initializer of '" + l.name.name + "'");
                    type = ann;
                } else {
                    type = value_type(*l.init, "initializer of '" + l.name.name + "'");
                    if (type->kind == TypeKind::null) {
                        diags_.error(l.init->loc, "cannot infer the type of '" + l.name.name +
                                                      "' from null; add a type annotation such as ': Option<pubkey>'");
                        type = types_.error();
                    }
                }
                l.sym = declare(SymbolKind::variable, l.name, type);
                l.sym->is_mutable = true;
                break;
            }
            case BindingKind::keypair:
                expect(*l.init, types_.keypair(), "keypair '" + l.name.name + "'");
                l.sym = declare(SymbolKind::keypair, l.name, types_.keypair());
                break;
            case BindingKind::source:
                expect_source(*l.init, "source '" + l.name.name + "'");
                l.sym = declare(SymbolKind::source, l.name, types_.source());
                break;
        }
        l.sym->is_global = global;
    }

    // ----------------------------------------------------------- schemas --

    void check_schema(SchemaDecl& s) {
        Symbol* sym = declare(SymbolKind::schema, s.name, nullptr);
        sym->schema = &s;
        sym->is_global = true;
        s.sym = sym;
        sym->type = types_.schema(sym);
        if (s.format.name == "anchor") {
            sym->schema_format = SchemaFormat::anchor;
        } else if (s.format.name != "borsh") {
            constexpr std::array<std::string_view, 2> formats = {"borsh", "anchor"};
            diags_.error(s.format.loc, "unknown schema format '" + s.format.name + "'; expected 'borsh' or 'anchor'" +
                                           suggest(s.format.name, formats));
        }
        std::vector<std::pair<std::string, const Type*>> fields;
        std::map<std::string, Loc> seen;
        for (auto& f : s.fields) {
            check_unique(seen, f.name.name, f.name.loc,
                         "duplicate field '" + f.name.name + "' in schema '" + s.name.name + "'",
                         "previous field is here");
            fields.emplace_back(f.name.name, resolve_type(f.type, true));
        }
        sym->schema_record = types_.record(std::move(fields), sym);
    }

    // --------------------------------------------------------- functions --

    void check_function(FunctionDecl& f) {
        Symbol* sym = declare(SymbolKind::function, f.name, nullptr);
        sym->function = &f;
        sym->is_global = true;
        f.sym = sym;
        for (auto& p : f.params) sym->param_types.push_back(resolve_type(p.type, false));
        sym->return_type = f.ret ? resolve_type(*f.ret, false) : types_.void_();

        {
            const Restore in_function(ctx_, Ctx::function);
            const Restore current(function_, static_cast<const Symbol*>(sym));
            const Restore no_loops(loop_depth_, 0);
            const ScopeGuard g(*this);
            for (std::size_t i = 0; i < f.params.size(); ++i) {
                f.params[i].sym = declare(SymbolKind::parameter, f.params[i].name, sym->param_types[i]);
            }
            for (auto& st : f.body) stmt(*st);
        }
        if (sym->return_type->kind != TypeKind::void_ && sym->return_type->kind != TypeKind::error &&
            !block_terminates(f.body))
            diags_.error(f.name.loc, "function '" + f.name.name +
                                         "' may reach the end of its body without returning a " +
                                         to_string(sym->return_type));
    }

    static bool block_terminates(const Block& b) {
        if (b.empty()) return false;
        const Stmt& last = *b.back();
        if (last.as<ReturnStmt>()) return true;
        if (const auto* i = last.as<IfStmt>())
            return i->else_block && block_terminates(i->then_block) && block_terminates(*i->else_block);
        return false;
    }

    // ---------------------------------------------------------- networks --

    void check_network(NetworkDecl& n) {
        const ScopeGuard g(*this);
        std::optional<Loc> options_loc;
        std::optional<Loc> genesis_loc;
        std::optional<Loc> bootstrap_loc;
        std::map<std::string, Loc> validators;
        std::map<std::string, Loc> scenarios;
        auto once = [&](std::optional<Loc>& slot, Loc at, const char* what) {
            if (slot) {
                diags_.error(at, std::string("network '") + n.name.name + "' already has " + what)
                    .notes.emplace_back(*slot, "previous definition is here");
            }
            slot = at;
        };
        for (auto& e : n.entries) {
            std::visit(
                Overloaded{
                    [&](OptionsEntry& o) {
                        once(options_loc, e.loc, "an options block");
                        with_ctx(Ctx::config, [&] { check_block(o.body, {"options", option_fields}); });
                    },
                    [&](ValidatorEntry& v) {
                        check_unique(validators, v.name.name, v.name.loc, "duplicate validator '" + v.name.name + "'");
                        with_ctx(Ctx::config, [&] { check_block(v.body, {"validator", validator_fields}); });
                    },
                    [&](LetStmt& l) { check_binding(l, false); },
                    [&](GenesisEntry& ge) {
                        once(genesis_loc, e.loc, "a genesis block");
                        program_labels_.clear();
                        with_ctx(Ctx::genesis, [&] { block(ge.body); });
                    },
                    [&](BootstrapEntry& b) {
                        once(bootstrap_loc, e.loc, "a bootstrap block");
                        with_ctx(Ctx::runtime, [&] { block(b.body); });
                    },
                    [&](ScenarioEntry& s) {
                        check_unique(scenarios, s.name.name, s.name.loc, "duplicate scenario '" + s.name.name + "'");
                        with_ctx(Ctx::runtime, [&] { block(s.body); });
                    },
                },
                e.node);
        }
    }

    template <typename F>
    void with_ctx(Ctx c, F&& f) {
        const Restore in_ctx(ctx_, c);
        const Restore no_loops(loop_depth_, 0);
        std::forward<F>(f)();
    }

    // ------------------------------------------------------------- types --

    const Type* resolve_type(TypeExpr& t, bool schema_mode) {
        switch (t.kind) {
            case TypeExpr::Kind::list: return types_.list(resolve_type(t.args[0], schema_mode));
            case TypeExpr::Kind::array: return resolve_array_type(t, schema_mode);
            case TypeExpr::Kind::tuple: return resolve_tuple_type(t, schema_mode);
            case TypeExpr::Kind::named: break;
        }
        return resolve_named_type(t, schema_mode);
    }

    // `[T; N]`: N must fold to a non-negative integer constant.
    const Type* resolve_array_type(TypeExpr& t, bool schema_mode) {
        const Type* elem = resolve_type(t.args[0], schema_mode);
        value_type(*t.size, "array length");
        auto n = const_int(*t.size);
        if (!n || *n < 0) {
            diags_.error(t.size->loc, "array length must be a non-negative integer literal");
            return types_.list(elem);
        }
        return types_.list(elem, static_cast<std::uint64_t>(*n));
    }

    const Type* resolve_tuple_type(TypeExpr& t, bool schema_mode) {
        std::vector<const Type*> elems;
        elems.reserve(t.args.size());
        for (auto& a : t.args) elems.push_back(resolve_type(a, schema_mode));
        if (!schema_mode) {
            diags_.error(t.loc, "tuple types are only allowed in schema fields");
            return types_.error();
        }
        return types_.tuple(std::move(elems));
    }

    static std::optional<IntWidth> int_width_named(std::string_view name) {
        static constexpr auto ints = std::to_array<std::pair<std::string_view, IntWidth>>({
            {"int", IntWidth::any},
            {"u8", IntWidth::u8},
            {"u16", IntWidth::u16},
            {"u32", IntWidth::u32},
            {"u64", IntWidth::u64},
            {"u128", IntWidth::u128},
            {"i8", IntWidth::i8},
            {"i16", IntWidth::i16},
            {"i32", IntWidth::i32},
            {"i64", IntWidth::i64},
            {"i128", IntWidth::i128},
        });
        for (const auto& [n, w] : ints)
            if (n == name) return w;
        return std::nullopt;
    }

    // Built-in type names other than the integers and Option. Types without a
    // binary encoding are rejected in schema fields.
    struct NamedType {
        std::string_view name;
        const Type* (TypeContext::*get)() const;
        bool serializable;
    };
    static const NamedType* named_type(std::string_view name) {
        static constexpr auto named = std::to_array<NamedType>({
            {"bool", &TypeContext::bool_, true},
            {"float", &TypeContext::float_, true},
            {"f64", &TypeContext::float_, true},
            {"f32", &TypeContext::float32, true},
            {"string", &TypeContext::string, true},
            {"bytes", &TypeContext::bytes, true},
            {"pubkey", &TypeContext::pubkey, true},
            {"duration", &TypeContext::duration, false},
            {"keypair", &TypeContext::keypair, false},
            {"source", &TypeContext::source, false},
            {"range", &TypeContext::range, false},
            {"account_meta", &TypeContext::account_meta, false},
        });
        for (const auto& n : named)
            if (n.name == name) return &n;
        return nullptr;
    }

    const Type* no_type_args(const TypeExpr& t, const Type* r) {
        if (!t.args.empty()) diags_.error(t.loc, "type '" + t.name + "' does not take type arguments");
        return r;
    }

    const Type* resolve_named_type(TypeExpr& t, bool schema_mode) {
        if (auto w = int_width_named(t.name)) {
            if (schema_mode && *w == IntWidth::any) {
                diags_.error(t.loc, "schema fields need a sized integer type (u8, u16, u32, u64, u128, i8, ..., i128)");
                return types_.error();
            }
            return no_type_args(t, types_.int_(*w));
        }
        if (const NamedType* n = named_type(t.name)) {
            const Type* r = no_type_args(t, (types_.*n->get)());
            if (schema_mode && !n->serializable) {
                diags_.error(t.loc, "type '" + t.name + "' cannot be used in a schema (it has no binary encoding)");
                return types_.error();
            }
            return r;
        }
        if (t.name == "Option") {
            if (t.args.size() != 1) {
                diags_.error(t.loc, "'Option' takes exactly one type argument");
                return types_.error();
            }
            return types_.option(resolve_type(t.args[0], schema_mode));
        }
        if (Symbol* s = lookup(t.name); s && s->kind == SymbolKind::schema) return no_type_args(t, s->schema_record);
        report_unknown_type(t);
        return types_.error();
    }

    void report_unknown_type(const TypeExpr& t) {
        std::vector<std::string> candidates = {"int",      "u8",     "u16",          "u32",    "u64",     "u128",
                                               "i8",       "i16",    "i32",          "i64",    "i128",    "bool",
                                               "float",    "string", "bytes",        "pubkey", "keypair", "source",
                                               "duration", "range",  "account_meta", "Option"};
        for (Scope* s = scope_; s; s = s->parent)
            for (const auto& [n, sym] : s->names)
                if (sym->kind == SymbolKind::schema) candidates.push_back(n);
        diags_.error(t.loc, "unknown type '" + t.name + "'" + suggest(t.name, candidates));
    }

    // ------------------------------------------------- type relationships --

    static bool is_err(const Type* t) { return (t == nullptr) || t->kind == TypeKind::error; }
    static bool is_numeric(const Type* t) { return t->kind == TypeKind::int_ || t->kind == TypeKind::float_; }

    bool assignable(const Type* from, const Type* to) const {
        if (is_err(from) || is_err(to)) return true;
        if (to->kind == TypeKind::any || from->kind == TypeKind::any) return true;
        if (to->kind == TypeKind::option) {
            if (from->kind == TypeKind::null) return true;
            if (from->kind == TypeKind::option) return assignable(from->elem, to->elem);
            return assignable(from, to->elem);
        }
        switch (from->kind) {
            case TypeKind::int_: return to->kind == TypeKind::int_ || to->kind == TypeKind::float_;
            case TypeKind::pubkey: return to->kind == TypeKind::pubkey || to->kind == TypeKind::account_meta;
            case TypeKind::bytes:
                return to->kind == TypeKind::bytes || (to->kind == TypeKind::list && is_byte_elem(to));
            case TypeKind::list: return list_assignable(from, to);
            case TypeKind::record: return record_assignable(from, to);
            case TypeKind::tuple: return tuple_assignable(from, to);
            case TypeKind::schema: return to->kind == TypeKind::schema && to->schema == from->schema;
            case TypeKind::module: return false;
            default: return from->kind == to->kind;
        }
    }

    // A list whose elements can stand in for bytes.
    static bool is_byte_elem(const Type* list) {
        return list->elem->kind == TypeKind::int_ || list->elem->kind == TypeKind::any;
    }

    bool list_assignable(const Type* from, const Type* to) const {
        if (to->kind == TypeKind::bytes) return is_byte_elem(from);
        if (to->kind != TypeKind::list) return false;
        if (to->fixed_len && from->fixed_len && *to->fixed_len != *from->fixed_len) return false;
        return assignable(from->elem, to->elem);
    }

    bool record_assignable(const Type* from, const Type* to) const {
        if (to->kind != TypeKind::record) return false;
        if (to->fields.empty() && !to->schema) return true;  // "any object" (config keys such as invoke args)
        if (to->schema && from->schema) return to->schema == from->schema;
        if (to->fields.size() != from->fields.size()) return false;
        return std::ranges::all_of(to->fields, [&](const auto& field) {
            const Type* f = from->field(field.first);
            return f && assignable(f, field.second);
        });
    }

    bool tuple_assignable(const Type* from, const Type* to) const {
        if (to->kind != TypeKind::tuple) return false;
        return std::ranges::equal(from->elems, to->elems,
                                  [&](const Type* a, const Type* b) { return assignable(a, b); });
    }

    // Least upper bound for list-literal inference; nullptr if none.
    const Type* join(const Type* a, const Type* b) {
        if (assignable(b, a) && (a->kind != TypeKind::int_ || b->kind != TypeKind::float_)) {
            if (a->kind == TypeKind::null) return types_.option(b);
            return a;
        }
        if (assignable(a, b)) return b->kind == TypeKind::null ? types_.option(a) : b;
        if (a->kind == TypeKind::null) return types_.option(b);
        if (b->kind == TypeKind::null) return types_.option(a);
        return nullptr;
    }

    // --------------------------------------------------------- constants --

    static std::optional<i128> const_int(const Expr& e) {
        if (const auto* n = e.as<NumberLit>(); n && n->folded_int && e.type && e.type->kind == TypeKind::int_)
            return n->int_value;
        if (const auto* u = e.as<UnaryExpr>(); u && u->op == UnaryOp::neg)
            if (auto v = const_int(*u->operand)) return -*v;
        return std::nullopt;
    }
    static const std::string* const_str(const Expr& e) {
        if (const auto* s = e.as<StringLit>()) return &s->value;
        return nullptr;
    }
    static BuiltinId const_builtin(const Expr& e) {
        if (const auto* c = e.as<CallExpr>(); c && c->args.empty()) return c->builtin;
        return BuiltinId::none;
    }

    // ------------------------------------------------------- expressions --

    // Checks `e` against `want`, reporting a mismatch. Returns the actual type.
    const Type* expect(Expr& e, const Type* want, const std::string& what) {
        const Type* t = expr(e, want);
        if (t->kind == TypeKind::void_) {
            diags_.error(e.loc, what + " has no value (the expression returns nothing)");
            return types_.error();
        }
        if (!assignable(t, want)) {
            diags_.error(e.loc,
                         "type mismatch in " + what + ": expected " + to_string(want) + ", found " + to_string(t));
            return types_.error();
        }
        if (want->kind == TypeKind::int_ && want->width != IntWidth::any) {
            if (auto v = const_int(e)) {
                auto [lo, hi] = int_bounds(want->width);
                if (*v < lo || *v > hi)
                    diags_.error(e.loc, "value out of range for " + to_string(want->width) + " in " + what);
            }
        }
        return t;
    }

    // An expression used for its value: must not be void.
    const Type* value_type(Expr& e, const std::string& what) {
        const Type* t = expr(e, nullptr);
        if (t->kind == TypeKind::void_) {
            diags_.error(e.loc, what + " has no value (the expression returns nothing)");
            return types_.error();
        }
        return t;
    }

    void expect_source(Expr& e, const std::string& what) {
        const Type* t = value_type(e, what);
        if (!is_err(t) && t->kind != TypeKind::source && t->kind != TypeKind::string)
            diags_.error(
                e.loc, "type mismatch in " + what + ": expected source (or an RPC URL string), found " + to_string(t));
    }

    const Type* expr(Expr& e, const Type* expected) {
        const Type* t = std::visit([&](auto& n) { return this->check(e, n, expected); }, e.node);
        e.type = t;
        return t;
    }

    // Parses decimal digits into `out`; false on overflow of the signed 128-bit range.
    static bool parse_decimal(std::string_view digits, i128& out) {
        out = 0;
        constexpr i128 limit = ~(i128{1} << 127);
        for (const char c : digits) {
            if (out > (limit - (c - '0')) / 10) return false;
            out = out * 10 + (c - '0');
        }
        return true;
    }

    // Exact decimal scaling of a literal: value * 10^scale must be integral.
    std::optional<i128> scaled_literal(const Expr& e, const NumberLit& n, int scale, const char* unit_desc) {
        auto dot = n.text.find('.');
        const std::string whole = n.text.substr(0, dot);
        std::string frac = dot == std::string::npos ? "" : n.text.substr(dot + 1);
        while (!frac.empty() && frac.back() == '0') frac.pop_back();
        if (static_cast<int>(frac.size()) > scale) {
            diags_.error(e.loc, "'" + n.text + "' is not a whole number of " + unit_desc);
            return std::nullopt;
        }
        const std::string digits = whole + frac + std::string(static_cast<std::size_t>(scale) - frac.size(), '0');
        i128 v = 0;
        if (!parse_decimal(digits, v)) {
            diags_.error(e.loc, "integer literal is too large");
            return std::nullopt;
        }
        return v;
    }

    // Folds the literal scaled by 10^scale into `n.int_value` when it is exact.
    void fold_scaled(const Expr& e, NumberLit& n, int scale, const char* unit_desc) {
        if (auto v = scaled_literal(e, n, scale, unit_desc)) n.int_value = *v;
    }

    const Type* check(Expr& e, NumberLit& n, const Type* expected) {
        switch (n.unit) {
            case Unit::none:
                if (n.is_float || (expected && expected->kind == TypeKind::float_)) {
                    n.folded_int = false;
                    n.float_value = std::strtod(n.text.c_str(), nullptr);
                    return types_.float_();
                }
                fold_scaled(e, n, 0, "units");
                return types_.int_();
            case Unit::lamports:
            case Unit::slots:
                fold_scaled(e, n, 0, n.unit == Unit::lamports ? "lamports" : "slots");
                return types_.int_();
            case Unit::sol: fold_scaled(e, n, 9, "lamports"); return types_.int_();
            case Unit::ms: fold_scaled(e, n, 0, "milliseconds"); return types_.duration();
            case Unit::s: fold_scaled(e, n, 3, "milliseconds"); return types_.duration();
        }
        return types_.error();
    }
    const Type* check(Expr& /*unused*/, StringLit& /*unused*/, const Type* /*unused*/) { return types_.string(); }
    const Type* check(Expr& /*unused*/, BoolLit& /*unused*/, const Type* /*unused*/) { return types_.bool_(); }
    const Type* check(Expr& /*unused*/, NullLit& /*unused*/, const Type* /*unused*/) { return types_.null(); }

    const Type* check(Expr& e, IdentExpr& id, const Type* /*unused*/) {
        Symbol* s = lookup(id.name);
        if (!s) {
            if (find_builtin(id.name))
                diags_.error(e.loc, "builtin function '" + id.name + "' must be called, e.g. '" + id.name + "(...)'");
            else
                diags_.error(e.loc, "unknown identifier '" + id.name + "'" + suggest(id.name, visible_names()));
            return types_.error();
        }
        id.sym = s;
        if (s->kind == SymbolKind::function) {
            diags_.error(e.loc, "function '" + id.name + "' must be called; functions are not values");
            return types_.error();
        }
        return s->type;
    }

    const Type* check(Expr& e, ListLit& l, const Type* expected) {
        if (expected && expected->kind == TypeKind::option) expected = expected->elem;
        if (expected && expected->kind == TypeKind::tuple) return check_tuple_literal(l, expected);
        const Type* want = nullptr;
        if (expected && expected->kind == TypeKind::list) want = expected->elem;
        if (expected && expected->kind == TypeKind::bytes) want = types_.int_(IntWidth::u8);
        if (want && want->kind != TypeKind::any) return check_list_literal_against(e, l, expected, want);
        return infer_list_literal(l);
    }

    // A list literal written for a tuple-typed slot (schema fields).
    const Type* check_tuple_literal(ListLit& l, const Type* expected) {
        if (l.elems.size() != expected->elems.size()) {
            diags_.error(l.elems.empty() ? Loc{} : l.elems.front()->loc,
                         "expected " + std::to_string(expected->elems.size()) + " elements for " + to_string(expected) +
                             ", found " + std::to_string(l.elems.size()));
            for (auto& el : l.elems) value_type(*el, "list element");
            return types_.error();
        }
        for (std::size_t i = 0; i < l.elems.size(); ++i)
            expect(*l.elems[i], expected->elems[i], "tuple element " + std::to_string(i));
        return expected;
    }

    // A list literal whose element type `want` comes from the expected list or bytes type.
    const Type* check_list_literal_against(Expr& e, ListLit& l, const Type* expected, const Type* want) {
        for (auto& el : l.elems) expect(*el, want, "list element");
        if (expected->kind == TypeKind::list && expected->fixed_len && l.elems.size() != *expected->fixed_len) {
            diags_.error(e.loc, "expected " + std::to_string(*expected->fixed_len) + " elements for " +
                                    to_string(expected) + ", found " + std::to_string(l.elems.size()));
            return types_.error();
        }
        if (expected->kind == TypeKind::bytes) return types_.list(types_.int_(IntWidth::u8));
        return types_.list(want, expected->fixed_len);
    }

    // Without an expected element type the elements' types are joined.
    const Type* infer_list_literal(ListLit& l) {
        const Type* elem = nullptr;
        for (auto& el : l.elems) {
            const Type* t = value_type(*el, "list element");
            if (is_err(t)) continue;
            if (!elem) {
                elem = t;
                continue;
            }
            const Type* j = join(elem, t);
            if (!j) {
                diags_.error(el->loc,
                             "list elements have incompatible types " + to_string(elem) + " and " + to_string(t));
                return types_.error();
            }
            elem = j;
        }
        return types_.list(elem ? elem : types_.any());
    }

    const Type* check(Expr& e, ObjectLit& o, const Type* expected) {
        if (expected && expected->kind == TypeKind::option) expected = expected->elem;
        std::map<std::string, Loc> seen;
        for (auto& f : o.fields) {
            check_unique(seen, f.key, f.loc, "duplicate key '" + f.key + "' in object literal", "previous key is here");
        }
        if (expected && expected->kind == TypeKind::record && !expected->fields.empty())
            return check_object_against_record(e, o, expected, seen);
        std::vector<std::pair<std::string, const Type*>> fields;
        fields.reserve(o.fields.size());
        for (auto& f : o.fields) fields.emplace_back(f.key, value_type(*f.value, "field '" + f.key + "'"));
        return types_.record(std::move(fields));
    }

    // An object literal checked field by field against a known record shape; `seen` holds its keys.
    const Type* check_object_against_record(Expr& e, ObjectLit& o, const Type* expected,
                                            const std::map<std::string, Loc>& seen) {
        const std::string what = expected->schema ? "schema '" + expected->schema->name + "'" : to_string(expected);
        std::vector<std::string> names;
        names.reserve(expected->fields.size());
        for (const auto& [n, _] : expected->fields) names.push_back(n);
        bool ok = true;
        for (auto& f : o.fields) {
            const Type* ft = expected->field(f.key);
            if (!ft) {
                diags_.error(f.loc, "unknown field '" + f.key + "' for " + what + suggest(f.key, names));
                value_type(*f.value, "field '" + f.key + "'");
                ok = false;
                continue;
            }
            if (is_err(expect(*f.value, ft, "field '" + f.key + "' of " + what))) ok = false;
        }
        for (const auto& [n, ft] : expected->fields) {
            if (!seen.contains(n) && ft->kind != TypeKind::option) {
                diags_.error(e.loc, std::string("missing field '").append(n).append("' for ").append(what));
                ok = false;
            }
        }
        return ok ? expected : types_.error();
    }

    const Type* check(Expr& e, UnaryExpr& u, const Type* /*unused*/) {
        const Type* t = value_type(*u.operand, "operand");
        if (is_err(t)) return t;
        if (u.op == UnaryOp::not_) {
            if (t->kind != TypeKind::bool_) {
                diags_.error(e.loc, "operator '!' expects bool, found " + to_string(t));
                return types_.error();
            }
            return t;
        }
        if (t->kind == TypeKind::int_) return types_.int_();
        if (t->kind == TypeKind::float_ || t->kind == TypeKind::duration) return t;
        diags_.error(e.loc, "operator '-' expects a number, found " + to_string(t));
        return types_.error();
    }

    static const char* op_text(BinaryOp op) {
        switch (op) {
            case BinaryOp::or_: return "||";
            case BinaryOp::and_: return "&&";
            case BinaryOp::eq: return "==";
            case BinaryOp::ne: return "!=";
            case BinaryOp::lt: return "<";
            case BinaryOp::le: return "<=";
            case BinaryOp::gt: return ">";
            case BinaryOp::ge: return ">=";
            case BinaryOp::range: return "..";
            case BinaryOp::range_incl: return "..=";
            case BinaryOp::add: return "+";
            case BinaryOp::sub: return "-";
            case BinaryOp::mul: return "*";
            case BinaryOp::div: return "/";
            case BinaryOp::mod: return "%";
        }
        return "?";
    }

    // Result type of an arithmetic operator; nullptr if not applicable.
    const Type* arith(BinaryOp op, const Type* a, const Type* b) {
        auto k = [](const Type* t) { return t->kind; };
        if (k(a) == TypeKind::int_ && k(b) == TypeKind::int_) return types_.int_();
        if (is_numeric(a) && is_numeric(b)) return types_.float_();
        if (k(a) == TypeKind::duration && k(b) == TypeKind::duration && (op == BinaryOp::add || op == BinaryOp::sub))
            return types_.duration();
        if (k(a) == TypeKind::duration && k(b) == TypeKind::int_ && (op == BinaryOp::mul || op == BinaryOp::div))
            return types_.duration();
        if (k(a) == TypeKind::int_ && k(b) == TypeKind::duration && op == BinaryOp::mul) return types_.duration();
        if (op == BinaryOp::add) {
            if (k(a) == TypeKind::string && k(b) == TypeKind::string) return types_.string();
            if (k(a) == TypeKind::bytes && k(b) == TypeKind::bytes) return types_.bytes();
            if (k(a) == TypeKind::list && k(b) == TypeKind::list) {
                if (const Type* j = join(a->elem, b->elem)) return types_.list(j);
            }
        }
        return nullptr;
    }

    const Type* check(Expr& /*unused*/, BinaryExpr& b, const Type* /*unused*/) {
        const Type* l = value_type(*b.lhs, "left operand");
        const Type* r = value_type(*b.rhs, "right operand");
        if (is_err(l) || is_err(r)) return types_.error();
        auto fail = [&]() {
            diags_.error(b.op_loc, std::string("operator '") + op_text(b.op) + "' cannot be applied to " +
                                       to_string(l) + " and " + to_string(r));
            return types_.error();
        };
        switch (b.op) {
            case BinaryOp::or_:
            case BinaryOp::and_:
                if (l->kind != TypeKind::bool_ || r->kind != TypeKind::bool_) return fail();
                return types_.bool_();
            case BinaryOp::eq:
            case BinaryOp::ne:
                if (!(assignable(l, r) || assignable(r, l) || (is_numeric(l) && is_numeric(r)))) return fail();
                if (l->kind == TypeKind::keypair || l->kind == TypeKind::module || l->kind == TypeKind::schema)
                    return fail();
                return types_.bool_();
            case BinaryOp::lt:
            case BinaryOp::le:
            case BinaryOp::gt:
            case BinaryOp::ge:
                if ((is_numeric(l) && is_numeric(r)) ||
                    (l->kind == r->kind && (l->kind == TypeKind::duration || l->kind == TypeKind::string)))
                    return types_.bool_();
                return fail();
            case BinaryOp::range:
            case BinaryOp::range_incl:
                if (l->kind != TypeKind::int_ || r->kind != TypeKind::int_) return fail();
                return types_.range();
            default:
                if (const Type* t = arith(b.op, l, r)) return t;
                return fail();
        }
    }

    const Type* check(Expr& e, MemberExpr& m, const Type* /*unused*/) {
        if (auto* id = m.object->as<IdentExpr>()) {
            Symbol* s = lookup(id->name);
            if (s && s->kind == SymbolKind::module) return check_module_member(e, m, *id, *s);
        }
        const Type* t = value_type(*m.object, "member access");
        if (is_err(t)) return t;
        return member_of_value(m, t);
    }

    // `alias.name` where `alias` is an imported module.
    const Type* check_module_member(Expr& e, MemberExpr& m, IdentExpr& alias, Symbol& s) {
        alias.sym = &s;
        m.object->type = s.type;
        if (!s.module) return types_.error();
        auto it = s.module->exports.find(m.member.name);
        if (it == s.module->exports.end()) {
            std::vector<std::string> names;
            names.reserve(s.module->exports.size());
            for (const auto& [n, _] : s.module->exports) names.push_back(n);
            diags_.error(m.member.loc, "module '" + alias.name + "' has no member '" + m.member.name + "'" +
                                           suggest(m.member.name, names));
            return types_.error();
        }
        m.module_member = it->second;
        if (it->second->kind == SymbolKind::function) {
            diags_.error(e.loc, "function '" + m.member.name + "' must be called; functions are not values");
            return types_.error();
        }
        return it->second->type;
    }

    // Member `m` of a value of type `t` (records see through Option).
    const Type* member_of_value(const MemberExpr& m, const Type* t) {
        const std::string& name = m.member.name;
        auto no_member = [&](const std::vector<std::string>& names = {}) {
            diags_.error(m.member.loc, "type " + to_string(t) + " has no member '" + name + "'" + suggest(name, names));
            return types_.error();
        };
        const Type* rec = t->kind == TypeKind::option ? t->elem : t;
        switch (rec->kind) {
            case TypeKind::keypair:
                if (name == "pubkey") return types_.pubkey();
                return no_member({"pubkey"});
            case TypeKind::source:
                if (name == "url") return types_.string();
                return no_member({"url"});
            case TypeKind::record: {
                if (const Type* f = rec->field(name)) return f;
                std::vector<std::string> names;
                names.reserve(rec->fields.size());
                for (const auto& [n, _] : rec->fields) names.push_back(n);
                return no_member(names);
            }
            default: return no_member();
        }
    }

    const Type* check(Expr& e, IndexExpr& ix, const Type* /*unused*/) {
        const Type* t = value_type(*ix.object, "indexed expression");
        expect(*ix.index, types_.int_(), "index");
        if (is_err(t)) return t;
        if (t->kind == TypeKind::list) return t->elem->kind == TypeKind::any ? types_.error() : t->elem;
        if (t->kind == TypeKind::bytes) return types_.int_(IntWidth::u8);
        diags_.error(e.loc, "type " + to_string(t) + " cannot be indexed");
        return types_.error();
    }

    const Type* check(Expr& e, CallExpr& c, const Type* /*expected*/) {
        // User function or builtin by name, or a user function through a module.
        if (auto* id = c.callee->as<IdentExpr>()) return check_named_call(e, c, *id);
        if (auto* m = c.callee->as<MemberExpr>(); m && m->object->as<IdentExpr>()) {
            auto* alias = m->object->as<IdentExpr>();
            Symbol* s = lookup(alias->name);
            if (s && s->kind == SymbolKind::module) return check_module_call(e, c, *m, *alias, *s);
        }
        value_type(*c.callee, "callee");
        diags_.error(c.callee->loc, "expression is not callable");
        check_args_as_values(c);
        return types_.error();
    }

    // Arguments of a call that cannot be matched to parameters are still checked.
    void check_args_as_values(CallExpr& c) {
        for (auto& a : c.args) value_type(*a, "argument");
    }

    const Type* check_named_call(Expr& e, CallExpr& c, IdentExpr& id) {
        Symbol* s = lookup(id.name);
        if (!s) {
            if (const BuiltinDef* b = find_builtin(id.name)) {
                c.builtin = b->id;
                return check_builtin(e, c, *b);
            }
            report_unknown_function(c, id);
            check_args_as_values(c);
            return types_.error();
        }
        id.sym = s;
        if (s->kind != SymbolKind::function) {
            diags_.error(c.callee->loc, "'" + id.name + "' is not a function (it has type " + to_string(s->type) + ")");
            check_args_as_values(c);
            return types_.error();
        }
        return check_user_call(e, c, *s, id.name);
    }

    void report_unknown_function(const CallExpr& c, const IdentExpr& id) {
        std::vector<std::string> names;
        names.reserve(builtin_table.size());
        for (const auto& b : builtin_table) names.emplace_back(b.name);
        for (Scope* sc = scope_; sc; sc = sc->parent)
            for (const auto& [n, sym] : sc->names)
                if (sym->kind == SymbolKind::function) names.push_back(n);
        diags_.error(c.callee->loc, "unknown function '" + id.name + "'" + suggest(id.name, names));
    }

    // `alias.fn(...)` where `alias` (symbol `s`) is an imported module.
    const Type* check_module_call(Expr& e, CallExpr& c, MemberExpr& m, IdentExpr& alias, Symbol& s) {
        alias.sym = &s;
        if (!s.module) return types_.error();
        auto it = s.module->exports.find(m.member.name);
        if (it == s.module->exports.end() || it->second->kind != SymbolKind::function) {
            diags_.error(m.member.loc, "module '" + alias.name + "' has no function '" + m.member.name + "'");
            check_args_as_values(c);
            return types_.error();
        }
        m.module_member = it->second;
        return check_user_call(e, c, *it->second, alias.name + "." + m.member.name);
    }

    const Type* check_user_call(Expr& e, CallExpr& c, const Symbol& fn, const std::string& fn_name) {
        c.function = &fn;
        if (c.args.size() != fn.param_types.size()) {
            diags_
                .error(e.loc, "function '" + fn_name + "' expects " + std::to_string(fn.param_types.size()) +
                                  " argument(s), found " + std::to_string(c.args.size()))
                .notes.emplace_back(fn.loc, "'" + fn.name + "' is declared here");
            check_args_as_values(c);
            return fn.return_type;
        }
        for (std::size_t i = 0; i < c.args.size(); ++i)
            expect(*c.args[i], fn.param_types[i],
                   "argument " + std::to_string(i + 1) + " ('" + fn.function->params[i].name.name + "') of '" +
                       fn_name + "'");
        return fn.return_type;
    }

    // ---------------------------------------------------------- builtins --

    bool arity(Expr& e, CallExpr& c, const BuiltinDef& b, std::size_t min, std::size_t max) {
        if (c.args.size() >= min && c.args.size() <= max) return true;
        const std::string expect_s =
            min == max ? std::to_string(min) : std::to_string(min) + " to " + std::to_string(max);
        diags_.error(e.loc, "'" + std::string(b.name) + "' expects " + expect_s + " argument(s), found " +
                                std::to_string(c.args.size()));
        check_args_as_values(c);
        return false;
    }

    static std::string arg_desc(const BuiltinDef& b, std::size_t i) {
        return "argument " + std::to_string(i + 1) + " of '" + std::string(b.name) + "'";
    }

    // Validates a string literal argument with a decoder (pubkey, hex, ...).
    template <typename F>
    void literal_check(Expr& arg, F&& valid, const std::string& msg) {
        if (const std::string* s = const_str(arg); s && !std::forward<F>(valid)(*s)) diags_.error(arg.loc, msg);
    }

    // `pda` seeds: a list whose elements are string, bytes or pubkey.
    void check_seeds(Expr& arg, const BuiltinDef& b) {
        auto seed_ok = [&](Expr& el) {
            const Type* t = value_type(el, "seed");
            if (is_err(t)) return;
            if (t->kind != TypeKind::string && t->kind != TypeKind::bytes && t->kind != TypeKind::pubkey) {
                diags_.error(el.loc, "seeds must be string, bytes or pubkey, found " + to_string(t) +
                                         (t->kind == TypeKind::int_ ? " (use u8(n) or u64_le(n) for integers)" : ""));
                return;
            }
            if (const std::string* s = const_str(el); s && s->size() > sol::max_seed_len)
                diags_.error(el.loc, "seed is " + std::to_string(s->size()) + " bytes; the maximum is 32");
        };
        if (auto* l = arg.as<ListLit>()) {
            if (l->elems.size() > sol::max_seeds - 1)
                diags_.error(arg.loc, "too many seeds (" + std::to_string(l->elems.size()) +
                                          "); the maximum is 15 plus the bump");
            for (auto& el : l->elems) seed_ok(*el);
            arg.type = types_.list(types_.any());
            return;
        }
        const Type* t = value_type(arg, arg_desc(b, 1));
        if (is_err(t)) return;
        if (t->kind != TypeKind::list || (t->elem->kind != TypeKind::string && t->elem->kind != TypeKind::bytes &&
                                          t->elem->kind != TypeKind::pubkey && t->elem->kind != TypeKind::any))
            diags_.error(arg.loc, "type mismatch in " + arg_desc(b, 1) +
                                      ": expected a list of seeds (string, bytes or pubkey), found " + to_string(t));
    }

    // First argument of borsh/anchor/decode: a schema name (possibly module-qualified).
    const Symbol* schema_arg(Expr& arg, const BuiltinDef& b, bool allow_string) {
        const Type* t = value_type(arg, arg_desc(b, 0));
        if (is_err(t)) return nullptr;
        if (t->kind == TypeKind::schema) return t->schema;
        if (allow_string && t->kind == TypeKind::string) {
            if (const std::string* name = const_str(arg)) {
                if (Symbol* s = lookup(*name); s && s->kind == SymbolKind::schema) return s;
            }
            return nullptr;  // resolved at runtime from the program IDL
        }
        diags_.error(arg.loc, "type mismatch in " + arg_desc(b, 0) + ": expected a schema name" +
                                  std::string(allow_string ? " or an account type name string" : "") + ", found " +
                                  to_string(t));
        return nullptr;
    }

    // Effects first: some builtins need network state.
    void check_builtin_effect(const Expr& e, const BuiltinDef& b) {
        if (b.effect == Effect::state && ctx_ != Ctx::genesis && ctx_ != Ctx::runtime)
            diags_.error(e.loc,
                         "'" + std::string(b.name) +
                             "' reads network state and can only be used inside genesis, bootstrap or scenario blocks");
        if (b.effect == Effect::runtime_only && ctx_ != Ctx::runtime)
            diags_.error(e.loc,
                         "'" + std::string(b.name) +
                             "' needs a running network and can only be used inside bootstrap or scenario blocks");
    }

    // The single argument of `c`, checked by value against a predicate; `want` describes the accepted types.
    template <typename Pred>
    void check_sole_arg_kind(CallExpr& c, const BuiltinDef& b, Pred&& ok, const std::string& want) {
        Expr& arg = *c.args[0];
        const Type* t = value_type(arg, arg_desc(b, 0));
        if (!is_err(t) && !std::forward<Pred>(ok)(t))
            diags_.error(arg.loc,
                         "type mismatch in " + arg_desc(b, 0) + ": expected " + want + ", found " + to_string(t));
    }

    const Type* check_pda(Expr& e, CallExpr& c, const BuiltinDef& b) {
        if (arity(e, c, b, 2, 2)) {
            expect(*c.args[0], types_.pubkey(), arg_desc(b, 0) + " (program id)");
            check_seeds(*c.args[1], b);
        }
        return b.id == BuiltinId::pda ? types_.pubkey() : types_.int_(IntWidth::u8);
    }

    const Type* check_ata(Expr& e, CallExpr& c, const BuiltinDef& b) {
        if (arity(e, c, b, 2, 3))
            for (std::size_t i = 0; i < c.args.size(); ++i) expect(*c.args[i], types_.pubkey(), arg_desc(b, i));
        return types_.pubkey();
    }

    const Type* check_sha256(Expr& e, CallExpr& c, const BuiltinDef& b) {
        if (!arity(e, c, b, 1, 1)) return types_.bytes();
        check_sole_arg_kind(
            c, b, [](const Type* t) { return t->kind == TypeKind::string || t->kind == TypeKind::bytes; },
            "string or bytes");
        return types_.bytes();
    }

    // borsh/anchor: a schema (anchor also takes an IDL account name) and the value to encode.
    const Type* check_encode(Expr& e, CallExpr& c, const BuiltinDef& b) {
        if (!arity(e, c, b, 2, 2)) return types_.bytes();
        if (const Symbol* s = schema_arg(*c.args[0], b, b.id == BuiltinId::anchor)) {
            expect(*c.args[1], s->schema_record, arg_desc(b, 1));
            return types_.bytes();
        }
        const Type* t = value_type(*c.args[1], arg_desc(b, 1));
        if (!is_err(t) && t->kind != TypeKind::record)
            diags_.error(c.args[1]->loc,
                         "type mismatch in " + arg_desc(b, 1) + ": expected an object, found " + to_string(t));
        return types_.bytes();
    }

    const Type* check_decode(Expr& e, CallExpr& c, const BuiltinDef& b) {
        if (!arity(e, c, b, 2, 2)) return types_.error();
        const Symbol* s = schema_arg(*c.args[0], b, false);
        expect(*c.args[1], types_.bytes(), arg_desc(b, 1));
        return s ? s->schema_record : types_.error();
    }

    const Type* check_len(Expr& e, CallExpr& c, const BuiltinDef& b) {
        if (!arity(e, c, b, 1, 1)) return types_.int_();
        const Type* t = value_type(*c.args[0], arg_desc(b, 0));
        if (!is_err(t) && t->kind != TypeKind::string && t->kind != TypeKind::bytes && t->kind != TypeKind::list)
            diags_.error(c.args[0]->loc, "'len' expects a string, bytes or list, found " + to_string(t));
        return types_.int_();
    }

    const Type* check_signer(Expr& e, CallExpr& c, const BuiltinDef& b) {
        if (arity(e, c, b, 1, 2)) {
            expect(*c.args[0], types_.pubkey(), arg_desc(b, 0));
            if (c.args.size() == 2) expect(*c.args[1], types_.bool_(), arg_desc(b, 1) + " (writable)");
        }
        return types_.account_meta();
    }

    const Type* check_sol(Expr& e, CallExpr& c, const BuiltinDef& b) {
        if (!arity(e, c, b, 1, 1)) return types_.int_();
        check_sole_arg_kind(c, b, is_numeric, "a number");
        return types_.int_();
    }

    const Type* check_spl_transfer_checked(Expr& e, CallExpr& c, const BuiltinDef& b) {
        if (arity(e, c, b, 2, 2)) {
            expect(*c.args[0], types_.int_(IntWidth::u64), arg_desc(b, 0) + " (amount)");
            expect(*c.args[1], types_.int_(IntWidth::u8), arg_desc(b, 1) + " (decimals)");
        }
        return types_.bytes();
    }

    const Type* check_builtin(Expr& e, CallExpr& c, const BuiltinDef& b) {
        check_builtin_effect(e, b);
        auto unary = [&](const Type* param, const Type* ret) {
            if (arity(e, c, b, 1, 1)) expect(*c.args[0], param, arg_desc(b, 0));
            return ret;
        };
        auto nullary = [&](const Type* ret) {
            arity(e, c, b, 0, 0);
            return ret;
        };
        // A decoder takes one string; a literal argument is validated statically.
        auto decoder = [&](const Type* ret, auto valid, const char* msg) {
            const Type* r = unary(types_.string(), ret);
            if (c.args.size() == 1) literal_check(*c.args[0], valid, msg);
            return r;
        };
        switch (b.id) {
            case BuiltinId::seed:
            case BuiltinId::keypair_file: return unary(types_.string(), types_.keypair());
            case BuiltinId::pubkey:
                return decoder(
                    types_.pubkey(), [](const std::string& s) { return sol::Pubkey::from_base58(s).has_value(); },
                    "invalid pubkey: expected a base58 string encoding 32 bytes");
            case BuiltinId::pda:
            case BuiltinId::pda_bump: return check_pda(e, c, b);
            case BuiltinId::ata: return check_ata(e, c, b);
            case BuiltinId::system_program:
            case BuiltinId::token_program:
            case BuiltinId::token_2022_program:
            case BuiltinId::associated_token_program:
            case BuiltinId::bpf_loader:
            case BuiltinId::bpf_loader_deprecated:
            case BuiltinId::bpf_upgradeable_loader:
            case BuiltinId::loader_v4:
            case BuiltinId::memo_program:
            case BuiltinId::compute_budget_program:
            case BuiltinId::address_lookup_table_program: return nullary(types_.pubkey());
            case BuiltinId::hex:
                return decoder(
                    types_.bytes(), [](const std::string& s) { return sol::hex_decode(s).has_value(); },
                    "invalid hex string: expected an even number of hex digits");
            case BuiltinId::base64:
                return decoder(
                    types_.bytes(), [](const std::string& s) { return sol::base64_decode(s).has_value(); },
                    "invalid base64 string");
            case BuiltinId::base58:
                return decoder(
                    types_.bytes(), [](const std::string& s) { return sol::base58_decode(s).has_value(); },
                    "invalid base58 string");
            case BuiltinId::utf8:
            case BuiltinId::file: return unary(types_.string(), types_.bytes());
            case BuiltinId::sha256: return check_sha256(e, c, b);
            case BuiltinId::u8: return unary(types_.int_(IntWidth::u8), types_.bytes());
            case BuiltinId::u16_le: return unary(types_.int_(IntWidth::u16), types_.bytes());
            case BuiltinId::u32_le: return unary(types_.int_(IntWidth::u32), types_.bytes());
            case BuiltinId::u64_le: return unary(types_.int_(IntWidth::u64), types_.bytes());
            case BuiltinId::i64_le: return unary(types_.int_(IntWidth::i64), types_.bytes());
            case BuiltinId::borsh:
            case BuiltinId::anchor: return check_encode(e, c, b);
            case BuiltinId::decode: return check_decode(e, c, b);
            case BuiltinId::len: return check_len(e, c, b);
            case BuiltinId::str:
                if (arity(e, c, b, 1, 1)) value_type(*c.args[0], arg_desc(b, 0));
                return types_.string();
            case BuiltinId::print:
                for (auto& a : c.args) value_type(*a, "argument of 'print'");
                return types_.void_();
            case BuiltinId::rent_exempt: return unary(types_.int_(IntWidth::u64), types_.int_());
            case BuiltinId::rpc: return unary(types_.string(), types_.source());
            case BuiltinId::readonly:
            case BuiltinId::writable: return unary(types_.pubkey(), types_.account_meta());
            case BuiltinId::signer: return check_signer(e, c, b);
            case BuiltinId::sol: return check_sol(e, c, b);
            case BuiltinId::system_transfer:
            case BuiltinId::spl_transfer:
            case BuiltinId::spl_mint_to: return unary(types_.int_(IntWidth::u64), types_.bytes());
            case BuiltinId::spl_transfer_checked: return check_spl_transfer_checked(e, c, b);
            case BuiltinId::memo: return unary(types_.string(), types_.bytes());
            case BuiltinId::balance:
            case BuiltinId::token_balance: return unary(types_.pubkey(), types_.int_());
            case BuiltinId::account: return unary(types_.pubkey(), types_.option(types_.account_info()));
            case BuiltinId::exists: return unary(types_.pubkey(), types_.bool_());
            case BuiltinId::slot:
            case BuiltinId::epoch: return nullary(types_.int_());
            case BuiltinId::none: break;
        }
        return types_.error();
    }

    // -------------------------------------------------------- statements --

    void block(Block& b) {
        const ScopeGuard g(*this);
        for (auto& s : b) stmt(*s);
    }

    void loop_body(Block& b, ForStmt* f, const Type* var_type) {
        const ScopeGuard g(*this);
        if (f) f->sym = declare(SymbolKind::loop_var, f->var, var_type);
        const Restore in_loop(loop_depth_, loop_depth_ + 1);
        for (auto& s : b) stmt(*s);
    }

    static const char* ctx_name(Ctx c) {
        switch (c) {
            case Ctx::top_level: return "at top level";
            case Ctx::function: return "in a function";
            case Ctx::config: return "in network configuration";
            case Ctx::genesis: return "in genesis";
            case Ctx::runtime: return "in bootstrap/scenario";
        }
        return "";
    }

    bool require_genesis(const Stmt& s, const char* what) {
        if (ctx_ == Ctx::genesis) return true;
        diags_.error(s.loc, std::string("'") + what + "' declarations are only allowed inside genesis (found " +
                                ctx_name(ctx_) + ")");
        return false;
    }

    bool require_runtime(const Stmt& s, const char* what) {
        if (ctx_ == Ctx::runtime) return true;
        const std::string hint =
            ctx_ == Ctx::genesis ? "; genesis builds state directly, use declarations such as 'account' instead" : "";
        diags_.error(s.loc, std::string("'") + what +
                                "' is a runtime statement and is only allowed inside bootstrap or scenario (found " +
                                ctx_name(ctx_) + ")" + hint);
        return false;
    }

    void reserved_feature(const Stmt& s, const char* what) {
        diags_.error(s.loc, std::string("'") + what +
                                "' is reserved for a future version of Tide: live-network control (warp, snapshot, "
                                "restore) is not supported yet");
    }

    void stmt(Stmt& s) {
        std::visit(Overloaded{
                       [&](LetStmt& l) { check_local_binding(s, l); },
                       [&](AssignStmt& a) { check_assign(a); },
                       [&](IfStmt& i) {
                           expect(*i.cond, types_.bool_(), "if condition");
                           block(i.then_block);
                           if (i.else_block) block(*i.else_block);
                       },
                       [&](ForStmt& f) { check_for(f); },
                       [&](WhileStmt& w) {
                           expect(*w.cond, types_.bool_(), "while condition");
                           loop_body(w.body, nullptr, nullptr);
                       },
                       [&](AssertStmt& a) { expect(*a.cond, types_.bool_(), "assertion"); },
                       [&](ExprStmt& es) { check_expr_stmt(es); },
                       [&](BreakStmt&) { require_loop(s, "'break' outside of a loop"); },
                       [&](ContinueStmt&) { require_loop(s, "'continue' outside of a loop"); },
                       [&](ReturnStmt& r) { check_return(s, r); },
                       [&](EntityDecl& d) { check_entity(s, d); },
                       [&](SysvarDecl& d) { check_sysvar(s, d); },
                       [&](FeatureDecl& f) {
                           require_genesis(s, "feature");
                           expect(*f.feature, types_.pubkey(), "feature id");
                       },
                       [&](ForkDecl& f) { check_fork(s, f); },
                       [&](CloneDecl& cl) { check_clone(s, cl); },
                       [&](DeleteDecl& d) {
                           require_genesis(s, "delete");
                           expect(*d.target, types_.pubkey(), "delete target");
                       },
                       [&](TransactionStmt& t) { check_transaction(s, t); },
                       [&](InvokeStmt& i) {
                           require_runtime(s, "invoke");
                           expect(*i.program, types_.pubkey(), "invoked program");
                           check_block(i.with, {"invoke", invoke_fields});
                       },
                       [&](DeployStmt& d) {
                           require_runtime(s, "deploy");
                           expect(*d.program, types_.keypair(), "deployed program (its keypair is the program id)");
                           expect(*d.from, types_.string(), "program binary path");
                       },
                       [&](AirdropStmt& a) {
                           require_runtime(s, "airdrop");
                           expect(*a.to, types_.pubkey(), "airdrop recipient");
                           expect(*a.amount, types_.int_(IntWidth::u64), "airdrop amount");
                       },
                       [&](WarpStmt& w) { check_warp(s, w); },
                       [&](SnapshotStmt& sn) { check_snapshot_control(s, *sn.name, "snapshot"); },
                       [&](RestoreStmt& r) { check_snapshot_control(s, *r.name, "restore"); },
                       [&](AwaitStmt& a) {
                           require_runtime(s, "await");
                           expect(*a.cond, types_.bool_(), "await condition");
                       },
                   },
                   s.node);
    }

    // keypair/source bindings are only legal at top level or directly in a network.
    void check_local_binding(const Stmt& s, LetStmt& l) {
        if (l.kind != BindingKind::let)
            diags_.error(s.loc, std::string("'") + (l.kind == BindingKind::keypair ? "keypair" : "source") +
                                    "' declarations are only allowed at top level or directly inside a network");
        check_binding(l, false);
    }

    void check_for(ForStmt& f) {
        const Type* t = value_type(*f.iterable, "for loop");
        const Type* var = types_.error();
        if (t->kind == TypeKind::range)
            var = types_.int_();
        else if (t->kind == TypeKind::list)
            var = t->elem;
        else if (!is_err(t))
            diags_.error(f.iterable->loc, "cannot iterate over " + to_string(t) + "; expected a range or a list");
        loop_body(f.body, &f, var);
    }

    void check_expr_stmt(ExprStmt& es) {
        const Type* t = expr(*es.expr, nullptr);
        if (!es.expr->as<CallExpr>() && !is_err(t)) diags_.warning(es.expr->loc, "expression result is unused");
    }

    void require_loop(const Stmt& s, const char* msg) {
        if (loop_depth_ == 0) diags_.error(s.loc, msg);
    }

    void check_sysvar(const Stmt& s, SysvarDecl& d) {
        require_genesis(s, "sysvar");
        const SysvarSpec* spec = nullptr;
        for (const auto& sv : sysvar_specs)
            if (sv.name == d.name.name) spec = &sv;
        if (!spec) {
            std::vector<std::string> names;
            names.reserve(sysvar_specs.size());
            for (const auto& sv : sysvar_specs) names.emplace_back(sv.name);
            diags_.error(d.name.loc,
                         "unknown or unsupported sysvar '" + d.name.name + "'" + suggest(d.name.name, names));
            return;
        }
        check_block(d.body, {spec->name, spec->fields});
    }

    void check_fork(const Stmt& s, ForkDecl& f) {
        require_genesis(s, "fork");
        expect_source(*f.source, "fork source");
        if (f.at_slot) expect(*f.at_slot, types_.int_(IntWidth::u64), "fork slot");
    }

    void check_clone(const Stmt& s, CloneDecl& cl) {
        require_genesis(s, "clone");
        expect(*cl.target, types_.pubkey(), "clone target");
        expect_source(*cl.source, "clone source");
        if (cl.at_slot) expect(*cl.at_slot, types_.int_(IntWidth::u64), "clone slot");
        if (cl.overrides) {
            if (cl.program)
                diags_.error(cl.overrides->loc, "overrides are only supported when cloning accounts, not programs");
            check_block(*cl.overrides, {"clone overrides", clone_override_fields});
        }
    }

    void check_warp(const Stmt& s, WarpStmt& w) {
        if (!require_runtime(s, "warp")) return;
        if (w.kind == WarpKind::to_time) {
            value_type(*w.value, "warp time");
        } else {
            expect(*w.value, types_.int_(IntWidth::u64), "warp target");
        }
        reserved_feature(s, "warp");
    }

    // snapshot/restore: both take a snapshot name and are reserved for now.
    void check_snapshot_control(const Stmt& s, Expr& name, const char* what) {
        if (!require_runtime(s, what)) return;
        expect(name, types_.string(), "snapshot name");
        reserved_feature(s, what);
    }

    // Validates an lvalue; returns its type or nullptr after reporting.
    const Type* lvalue(Expr& e) {
        if (auto* id = e.as<IdentExpr>()) {
            Symbol* s = lookup(id->name);
            if (!s) {
                diags_.error(e.loc, "unknown identifier '" + id->name + "'" + suggest(id->name, visible_names()));
                return nullptr;
            }
            id->sym = s;
            e.type = s->type;
            if (!s->is_mutable) {
                diags_
                    .error(e.loc, std::string("cannot assign to ") + symbol_kind_name(s->kind) + " '" + id->name +
                                      "'; only 'let' variables are mutable")
                    .notes.emplace_back(s->loc, "declared here");
                return nullptr;
            }
            if (s->is_global && ctx_ != Ctx::top_level) {
                diags_
                    .error(e.loc, "cannot assign to top-level variable '" + id->name +
                                      "' here; top-level bindings are immutable outside top-level code")
                    .notes.emplace_back(s->loc, "declared here");
                return nullptr;
            }
            return s->type;
        }
        if (auto* m = e.as<MemberExpr>()) {
            const Type* t = lvalue(*m->object);
            if (!t) return nullptr;
            if (t->kind != TypeKind::record) {
                diags_.error(m->member.loc, "cannot assign to member '" + m->member.name + "' of " + to_string(t));
                return nullptr;
            }
            const Type* f = t->field(m->member.name);
            if (!f) {
                diags_.error(m->member.loc, "type " + to_string(t) + " has no member '" + m->member.name + "'");
                return nullptr;
            }
            e.type = f;
            return f;
        }
        if (auto* ix = e.as<IndexExpr>()) {
            const Type* t = lvalue(*ix->object);
            expect(*ix->index, types_.int_(), "index");
            if (!t) return nullptr;
            const Type* r = nullptr;
            if (t->kind == TypeKind::list)
                r = t->elem;
            else if (t->kind == TypeKind::bytes)
                r = types_.int_(IntWidth::u8);
            else {
                diags_.error(e.loc, "type " + to_string(t) + " cannot be indexed");
                return nullptr;
            }
            e.type = r;
            return r;
        }
        diags_.error(e.loc, "invalid assignment target; expected a variable, a field or an index expression");
        return nullptr;
    }

    void check_assign(AssignStmt& a) {
        const Type* target = lvalue(*a.target);
        if (!target) {
            value_type(*a.value, "assigned value");
            return;
        }
        if (a.op == AssignOp::set) {
            expect(*a.value, target, "assignment");
            return;
        }
        const Type* v = value_type(*a.value, "assigned value");
        if (is_err(v) || is_err(target)) return;
        const Type* r = arith(compound_op(a.op), target, v);
        if (!r || !assignable(r, target))
            diags_.error(a.value->loc,
                         "compound assignment cannot combine " + to_string(target) + " with " + to_string(v));
    }

    void check_return(Stmt& s, ReturnStmt& r) {
        if (ctx_ != Ctx::function || !function_) {
            diags_.error(s.loc, std::string("'return' outside of a function (found ") + ctx_name(ctx_) + ")");
            if (r.value) value_type(*r.value, "return value");
            return;
        }
        const Type* want = function_->return_type;
        if (want->kind == TypeKind::void_) {
            if (r.value) {
                diags_.error(r.value->loc, "function '" + function_->name +
                                               "' does not declare a return type and cannot return a value");
                value_type(*r.value, "return value");
            }
            return;
        }
        if (!r.value) {
            diags_.error(s.loc, "function '" + function_->name + "' must return a value of type " + to_string(want));
            return;
        }
        expect(*r.value, want, "return value of '" + function_->name + "'");
    }

    static const char* symbol_kind_name(SymbolKind k) {
        switch (k) {
            case SymbolKind::variable: return "variable";
            case SymbolKind::keypair: return "keypair";
            case SymbolKind::source: return "source";
            case SymbolKind::function: return "function";
            case SymbolKind::schema: return "schema";
            case SymbolKind::module: return "module";
            case SymbolKind::parameter: return "parameter";
            case SymbolKind::loop_var: return "loop variable";
            case SymbolKind::tx_binding: return "transaction result";
        }
        return "symbol";
    }

    static const char* entity_keyword(EntityKind k) {
        switch (k) {
            case EntityKind::account: return "account";
            case EntityKind::program: return "program";
            case EntityKind::mint: return "mint";
            case EntityKind::token_account: return "token_account";
            case EntityKind::associated_token_account: return "associated_token_account";
            case EntityKind::lookup_table: return "lookup_table";
        }
        return "entity";
    }

    void check_entity(Stmt& s, EntityDecl& d) {
        const char* what = entity_keyword(d.kind);
        require_genesis(s, what);
        if (d.target) expect(*d.target, types_.pubkey(), std::string(what) + " address");
        check_entries(d.fields, entity_spec(d, what), s.loc);
        lint_entity_literals(d);
    }

    // Field and extension specs of an entity declaration; also rejects duplicate program labels.
    BlockSpec entity_spec(const EntityDecl& d, const char* what) {
        BlockSpec spec{what, {}};
        switch (d.kind) {
            case EntityKind::account: spec.fields = account_fields; break;
            case EntityKind::program:
                spec.fields = program_fields;
                check_unique(program_labels_, d.label.name, d.label.loc,
                             "duplicate program '" + d.label.name + "' in genesis");
                break;
            case EntityKind::mint:
                spec.fields = mint_fields;
                spec.extensions = mint_extensions;
                break;
            case EntityKind::token_account:
                spec.fields = token_account_fields;
                spec.extensions = token_account_extensions;
                break;
            case EntityKind::associated_token_account: spec.fields = ata_fields; break;
            case EntityKind::lookup_table: spec.fields = lookup_table_fields; break;
        }
        return spec;
    }

    static bool is_token_state(const std::string& v) { return v == "initialized" || v == "frozen"; }

    // Static lints over literal values.
    void lint_entity_literals(EntityDecl& d) {
        if (d.kind == EntityKind::token_account) {
            if (Expr* st = value_field(d, "state"))
                literal_check(*st, is_token_state, R"(token account state must be "initialized" or "frozen")");
        }
        if (d.kind == EntityKind::account) lint_executable_account(d);
        for (auto& e : d.fields) {
            if (e.kind != RecordEntry::Kind::extension) continue;
            for (auto& x : e.block->entries)
                if (x.key == "state" && x.value)
                    literal_check(*x.value, is_token_state,
                                  R"(default account state must be "initialized" or "frozen")");
        }
    }

    static Expr* value_field(EntityDecl& d, std::string_view name) {
        for (auto& e : d.fields)
            if (e.kind == RecordEntry::Kind::value && e.key == name) return e.value.get();
        return nullptr;
    }

    void lint_executable_account(EntityDecl& d) {
        const Expr* exe = value_field(d, "executable");
        const Expr* owner = value_field(d, "owner");
        const bool exec_true = (exe != nullptr) && (exe->as<BoolLit>() != nullptr) && exe->as<BoolLit>()->value;
        if (exec_true && (!owner || const_builtin(*owner) == BuiltinId::system_program))
            diags_.warning(exe->loc,
                           "executable account is owned by the system program; executable accounts are normally "
                           "owned by a loader");
    }

    void check_transaction(Stmt& s, TransactionStmt& t) {
        require_runtime(s, "transaction");
        std::map<std::string, Loc> seen;
        std::vector<std::string> keys;
        keys.reserve(tx_fields.size());
        for (const auto& f : tx_fields) keys.emplace_back(f.name);
        std::size_t instructions = 0;
        for (auto& e : t.entries) {
            if (e.is_instruction) {
                ++instructions;
                expect(*e.program, types_.pubkey(), "instruction program id");
                check_block(e.body, {"instruction", instruction_fields});
                continue;
            }
            check_unique(seen, e.key, e.loc, "duplicate transaction field '" + e.key + "'", "previous value is here");
            const FieldSpec* spec = nullptr;
            for (const auto& f : tx_fields)
                if (f.name == e.key) spec = &f;
            if (!spec) {
                diags_.error(e.loc, "unknown transaction field '" + e.key + "'" + suggest(e.key, keys));
                value_type(*e.value, "field '" + e.key + "'");
                continue;
            }
            expect(*e.value, spec_type(spec->type), "transaction field '" + e.key + "'");
        }
        if (instructions == 0) diags_.error(s.loc, "transaction has no instructions");
        if (t.binding) {
            t.sym = declare(SymbolKind::tx_binding, *t.binding, types_.tx_result());
        }
    }

    // ----------------------------------------------------- record blocks --

    const Type* spec_type(Ty t) {
        switch (t) {
            case Ty::any: return types_.any();
            case Ty::int_: return types_.int_();
            case Ty::u8: return types_.int_(IntWidth::u8);
            case Ty::u16: return types_.int_(IntWidth::u16);
            case Ty::u32: return types_.int_(IntWidth::u32);
            case Ty::u64: return types_.int_(IntWidth::u64);
            case Ty::float_:
            case Ty::number: return types_.float_();
            case Ty::bool_: return types_.bool_();
            case Ty::string: return types_.string();
            case Ty::bytes: return types_.bytes();
            case Ty::pubkey: return types_.pubkey();
            case Ty::opt_pubkey: return types_.option(types_.pubkey());
            case Ty::keypair: return types_.keypair();
            case Ty::source: return types_.source();
            case Ty::duration: return types_.duration();
            case Ty::list_pubkey: return types_.list(types_.pubkey());
            case Ty::list_keypair: return types_.list(types_.keypair());
            case Ty::list_meta: return types_.list(types_.account_meta());
            case Ty::record: return types_.record({});
        }
        return types_.any();
    }

    void check_block(RecordBlock& b, const BlockSpec& spec) { check_entries(b.entries, spec, b.loc); }

    void check_entries(std::vector<RecordEntry>& entries, const BlockSpec& spec, Loc block_loc) {
        std::map<std::string, Loc> seen;
        std::vector<std::string> names;
        for (const auto& f : spec.fields) names.emplace_back(f.name);
        for (auto& e : entries) {
            if (e.kind == RecordEntry::Kind::extension)
                check_extension_entry(e, spec, seen);
            else
                check_field_entry(e, spec, seen, names);
        }
        for (const auto& f : spec.fields)
            if (f.required && !seen.contains(std::string(f.name)))
                diags_.error(block_loc,
                             "missing required field '" + std::string(f.name) + "' in " + std::string(spec.what));
    }

    // `extension name { ... }` inside a block that supports extensions (mints, token accounts).
    void check_extension_entry(RecordEntry& e, const BlockSpec& spec, std::map<std::string, Loc>& seen) {
        if (spec.extensions.empty()) {
            diags_.error(e.loc, std::string("extensions are not supported in ") + std::string(spec.what));
            return;
        }
        const ExtSpec* ext = nullptr;
        std::vector<std::string> ext_names;
        for (const auto& x : spec.extensions) {
            ext_names.emplace_back(x.name);
            if (x.name == e.key) ext = &x;
        }
        if (!ext) {
            diags_.error(
                e.loc, "unknown " + std::string(spec.what) + " extension '" + e.key + "'" + suggest(e.key, ext_names));
            return;
        }
        check_unique(seen, "extension " + e.key, e.loc, "duplicate extension '" + e.key + "'");
        check_entries(e.block->entries, {ext->name, ext->fields}, e.block->loc);
    }

    // `key = value;` (or a misplaced nested block) checked against the block's field specs.
    void check_field_entry(RecordEntry& e, const BlockSpec& spec, std::map<std::string, Loc>& seen,
                           const std::vector<std::string>& names) {
        check_unique(seen, e.key, e.loc, "duplicate field '" + e.key + "' in " + std::string(spec.what),
                     "previous value is here");
        const FieldSpec* fs = nullptr;
        for (const auto& f : spec.fields)
            if (f.name == e.key) fs = &f;
        if (!fs) {
            diags_.error(e.loc, "unknown field '" + e.key + "' in " + std::string(spec.what) + suggest(e.key, names));
            if (e.value) value_type(*e.value, "field '" + e.key + "'");
            return;
        }
        if (e.kind == RecordEntry::Kind::nested) {
            diags_.error(e.loc, "field '" + e.key + "' expects a value: write '" + e.key + " = ...;'");
            return;
        }
        const std::string what = "field '" + e.key + "' of " + std::string(spec.what);
        if (fs->type == Ty::source) {
            expect_source(*e.value, what);
        } else if (fs->type == Ty::number) {
            const Type* t = value_type(*e.value, what);
            if (!is_err(t) && !is_numeric(t))
                diags_.error(e.value->loc, "type mismatch in " + what + ": expected a number, found " + to_string(t));
        } else {
            expect(*e.value, spec_type(fs->type), what);
        }
    }

    Program& prog_;
    TypeContext& types_;
    SourceManager& sm_;
    Diagnostics& diags_;
    Module* module_ = nullptr;
    Scope* scope_ = nullptr;
    Ctx ctx_ = Ctx::top_level;
    int loop_depth_ = 0;
    const Symbol* function_ = nullptr;
    std::map<std::string, Loc> program_labels_;
    std::map<std::filesystem::path, Module*> loaded_;
    std::set<std::filesystem::path> in_progress_;
};

}  // namespace

std::unique_ptr<Program> analyze(Document& root, SourceManager& sources, Diagnostics& diags) {
    auto prog = std::make_unique<Program>();
    const std::size_t before = diags.error_count();
    Checker checker(*prog, sources, diags);
    checker.load_root(root);
    // The root module must come last in initialization order; check_module
    // appended it after its imports already.
    if (diags.error_count() != before) return nullptr;
    return prog;
}

}  // namespace tide::sema
