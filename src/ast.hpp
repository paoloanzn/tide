// Tide abstract syntax tree.
//
// The parser produces these nodes; the semantic checker decorates them in
// place (the `// sema:` fields) with resolved symbols, builtin ids, static
// types and folded literal values; the interpreter only reads them.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "diag.hpp"

namespace tide::sema {
struct Symbol;
struct Type;
struct Module;
}  // namespace tide::sema

namespace tide::ast {

template <typename T>
using Box = std::unique_ptr<T>;

struct Expr;
struct Stmt;
using ExprPtr = Box<Expr>;
using StmtPtr = Box<Stmt>;
using Block = std::vector<StmtPtr>;

struct Ident {
    Loc loc;
    std::string name;
};

// ---------------------------------------------------------------- types --

struct TypeExpr {
    enum class Kind { named, list, array, tuple };
    Loc loc;
    Kind kind = Kind::named;
    std::string name;            // named
    std::vector<TypeExpr> args;  // named<args...>, [elem], [elem; n], (elems...)
    ExprPtr size;                // array length
};

// ---------------------------------------------------------- expressions --

enum class Unit { none, sol, lamports, ms, s, slots };

enum class BinaryOp { or_, and_, eq, ne, lt, le, gt, ge, range, range_incl, add, sub, mul, div, mod };
enum class UnaryOp { not_, neg };

// Identifies a standard-library function; resolved by sema, executed by the
// interpreter (see sema.cpp `builtin_table` and interp.cpp `call_builtin`).
enum class BuiltinId {
    none,
    // keys & addresses
    seed, keypair_file, pubkey, pda, pda_bump, ata,
    system_program, token_program, token_2022_program, associated_token_program,
    bpf_loader, bpf_loader_deprecated, bpf_upgradeable_loader, loader_v4,
    memo_program, compute_budget_program, address_lookup_table_program,
    // bytes & encodings
    hex, base64, base58, utf8, sha256, file, u8, u16_le, u32_le, u64_le, i64_le, borsh, anchor, decode,
    // misc
    len, str, print, rent_exempt, rpc, readonly, writable, signer, sol,
    // instruction data helpers
    system_transfer, spl_transfer, spl_transfer_checked, spl_mint_to, memo,
    // state queries
    balance, token_balance, account, exists, slot, epoch,
};

struct NumberLit {
    std::string text;
    bool is_float = false;
    Unit unit = Unit::none;
    Loc unit_loc;
    // sema: folded value. A float with a lamport unit folds to an integer.
    bool folded_int = true;
    __int128 int_value = 0;
    double float_value = 0;
};
struct StringLit { std::string value; };
struct BoolLit { bool value; };
struct NullLit {};
struct IdentExpr {
    std::string name;
    const sema::Symbol* sym = nullptr;  // sema
};
struct ListLit { std::vector<ExprPtr> elems; };
struct ObjectField {
    Loc loc;
    std::string key;
    ExprPtr value;
};
struct ObjectLit { std::vector<ObjectField> fields; };
struct UnaryExpr {
    UnaryOp op;
    ExprPtr operand;
};
struct BinaryExpr {
    BinaryOp op;
    Loc op_loc;
    ExprPtr lhs, rhs;
};
struct MemberExpr {
    ExprPtr object;
    Ident member;
    const sema::Symbol* module_member = nullptr;  // sema: set for `module.name`
};
struct IndexExpr {
    ExprPtr object, index;
};
struct CallExpr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    BuiltinId builtin = BuiltinId::none;       // sema
    const sema::Symbol* function = nullptr;    // sema: user function
};

struct Expr {
    Loc loc;
    std::variant<NumberLit, StringLit, BoolLit, NullLit, IdentExpr, ListLit, ObjectLit, UnaryExpr, BinaryExpr,
                 MemberExpr, IndexExpr, CallExpr>
        node;
    const sema::Type* type = nullptr;  // sema

    template <typename T>
    T* as() { return std::get_if<T>(&node); }
    template <typename T>
    const T* as() const { return std::get_if<T>(&node); }
};

// --------------------------------------------------------------- records --

struct RecordBlock;

// `key = value;`, `key { ... }` or `extension name { ... }`.
struct RecordEntry {
    enum class Kind { value, nested, extension };
    Loc loc;
    Kind kind = Kind::value;
    std::string key;
    ExprPtr value;
    Box<RecordBlock> block;
};

struct RecordBlock {
    Loc loc;
    std::vector<RecordEntry> entries;
};

// ------------------------------------------------------------ statements --

enum class BindingKind { let, source, keypair };

struct LetStmt {
    BindingKind kind = BindingKind::let;
    Ident name;
    std::optional<TypeExpr> annotation;
    ExprPtr init;
    sema::Symbol* sym = nullptr;  // sema
};

enum class AssignOp { set, add, sub, mul, div, mod };
struct AssignStmt {
    ExprPtr target;
    AssignOp op = AssignOp::set;
    ExprPtr value;
};
struct IfStmt {
    ExprPtr cond;
    Block then_block;
    std::optional<Block> else_block;
};
struct ForStmt {
    Ident var;
    ExprPtr iterable;
    Block body;
    sema::Symbol* sym = nullptr;  // sema
};
struct WhileStmt {
    ExprPtr cond;
    Block body;
};
struct AssertStmt {
    ExprPtr cond;
    std::optional<std::string> message;
};
struct ExprStmt { ExprPtr expr; };
struct BreakStmt {};
struct ContinueStmt {};
struct ReturnStmt { ExprPtr value; };

// Genesis state declarations whose body is a field list.
enum class EntityKind { account, program, mint, token_account, associated_token_account, lookup_table };
struct EntityDecl {
    EntityKind kind;
    Ident label;     // program name (program only)
    ExprPtr target;  // address expression (all but program)
    std::vector<RecordEntry> fields;
};
struct SysvarDecl {
    Ident name;
    RecordBlock body;
};
struct FeatureDecl {
    ExprPtr feature;
    bool enabled = true;
};
struct ForkDecl {
    ExprPtr source;
    ExprPtr at_slot;
};
struct CloneDecl {
    bool program = false;
    ExprPtr target, source, at_slot;
    std::optional<RecordBlock> overrides;
};
struct DeleteDecl {
    bool program = false;
    ExprPtr target;
};

// Runtime statements.
struct TxEntry {
    Loc loc;
    bool is_instruction = false;
    std::string key;     // key = value
    ExprPtr value;
    ExprPtr program;     // instruction <program> { ... }
    RecordBlock body;
};
struct TransactionStmt {
    std::optional<Ident> binding;
    std::vector<TxEntry> entries;
    sema::Symbol* sym = nullptr;  // sema
};
struct InvokeStmt {
    ExprPtr program;
    RecordBlock with;
};
struct DeployStmt { ExprPtr program, from; };
struct AirdropStmt { ExprPtr to, amount; };
enum class WarpKind { to_slot, to_epoch, to_time, by_slots };
struct WarpStmt {
    WarpKind kind;
    ExprPtr value;
};
struct SnapshotStmt { ExprPtr name; };
struct RestoreStmt { ExprPtr name; };
struct AwaitStmt { ExprPtr cond; };

struct Stmt {
    Loc loc;
    std::variant<LetStmt, AssignStmt, IfStmt, ForStmt, WhileStmt, AssertStmt, ExprStmt, BreakStmt, ContinueStmt,
                 ReturnStmt, EntityDecl, SysvarDecl, FeatureDecl, ForkDecl, CloneDecl, DeleteDecl, TransactionStmt,
                 InvokeStmt, DeployStmt, AirdropStmt, WarpStmt, SnapshotStmt, RestoreStmt, AwaitStmt>
        node;

    template <typename T>
    T* as() { return std::get_if<T>(&node); }
    template <typename T>
    const T* as() const { return std::get_if<T>(&node); }
};

// ---------------------------------------------------------- declarations --

struct ImportDecl {
    Loc path_loc;
    std::string path;
    Ident alias;
    const sema::Module* module = nullptr;  // sema
    sema::Symbol* sym = nullptr;           // sema
};

struct SchemaField {
    Ident name;
    TypeExpr type;
};
struct SchemaDecl {
    Ident name;
    Ident format;  // borsh | anchor
    std::vector<SchemaField> fields;
    sema::Symbol* sym = nullptr;  // sema
};

struct Param {
    Ident name;
    TypeExpr type;
    sema::Symbol* sym = nullptr;  // sema
};
struct FunctionDecl {
    Ident name;
    std::vector<Param> params;
    std::optional<TypeExpr> ret;
    Block body;
    sema::Symbol* sym = nullptr;  // sema
};

struct OptionsEntry { RecordBlock body; };
struct ValidatorEntry {
    Ident name;
    RecordBlock body;
};
struct GenesisEntry { Block body; };
struct BootstrapEntry { Block body; };
struct ScenarioEntry {
    Ident name;
    Block body;
};

struct NetworkEntry {
    Loc loc;
    std::variant<OptionsEntry, ValidatorEntry, LetStmt, GenesisEntry, BootstrapEntry, ScenarioEntry> node;
};

struct NetworkDecl {
    Ident name;
    std::vector<NetworkEntry> entries;
};

struct Decl {
    Loc loc;
    std::variant<ImportDecl, LetStmt, SchemaDecl, FunctionDecl, NetworkDecl> node;
};

struct Document {
    const SourceFile* file = nullptr;
    std::vector<Decl> decls;
};

}  // namespace tide::ast
