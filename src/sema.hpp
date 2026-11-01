// Semantic analysis: symbol resolution, scoping, static typing, context rules
// and declaration validation. Runs to completion before anything executes;
// the interpreter relies on the decorations it leaves on the AST.
//
// The full language semantics are documented in README.md.
#pragma once

#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ast.hpp"
#include "diag.hpp"

namespace tide::sema {

// ---------------------------------------------------------------- types --

enum class TypeKind {
    error,         // poison: suppresses cascading diagnostics
    any,           // only as an expected type (builtins, config keys, empty lists)
    void_, null, bool_, int_, float_, string, bytes, duration, pubkey, keypair, source, range,
    list, option, record, tuple, account_meta, schema, module,
};

// Integer refinements used by annotations and schemas. Values are always
// 128-bit at runtime; a refined slot is range-checked on store/serialize.
enum class IntWidth { any, u8, u16, u32, u64, u128, i8, i16, i32, i64, i128 };

struct Type {
    TypeKind kind = TypeKind::error;
    IntWidth width = IntWidth::any;                          // int_; float_ uses u32 for f32
    const Type* elem = nullptr;                              // list, option
    std::optional<std::uint64_t> fixed_len;                  // list from `[T; N]`
    std::vector<std::pair<std::string, const Type*>> fields; // record (ordered)
    std::vector<const Type*> elems;                          // tuple
    const struct Symbol* schema = nullptr;                   // schema; record decoded from a schema
    const struct Module* module = nullptr;                   // module

    const Type* field(std::string_view name) const {
        for (const auto& [n, t] : fields)
            if (n == name) return t;
        return nullptr;
    }
};

std::string to_string(const Type* t);
std::string to_string(IntWidth w);
// Inclusive bounds for a refined integer width.
std::pair<__int128, __int128> int_bounds(IntWidth w);

class TypeContext {
public:
    TypeContext();
    const Type* error() const { return error_; }
    const Type* any() const { return any_; }
    const Type* void_() const { return void_t_; }
    const Type* null() const { return null_; }
    const Type* bool_() const { return bool_t_; }
    const Type* int_(IntWidth w = IntWidth::any) const { return ints_[static_cast<int>(w)]; }
    const Type* float_() const { return float_t_; }
    const Type* float32() const { return float32_; }
    const Type* string() const { return string_; }
    const Type* bytes() const { return bytes_; }
    const Type* duration() const { return duration_; }
    const Type* pubkey() const { return pubkey_; }
    const Type* keypair() const { return keypair_; }
    const Type* source() const { return source_; }
    const Type* range() const { return range_; }
    const Type* account_meta() const { return meta_; }
    const Type* list(const Type* elem, std::optional<std::uint64_t> len = std::nullopt);
    const Type* option(const Type* inner);
    const Type* record(std::vector<std::pair<std::string, const Type*>> fields, const Symbol* schema = nullptr);
    const Type* tuple(std::vector<const Type*> elems);
    const Type* schema(const Symbol* s);
    const Type* module(const Module* m);
    // Record shapes produced by the standard library.
    const Type* account_info() const { return account_info_; }
    const Type* tx_result() const { return tx_result_; }

private:
    const Type* make(Type t);
    std::deque<Type> arena_;
    const Type *error_, *any_, *float32_, *void_t_, *null_, *bool_t_, *float_t_, *string_, *bytes_, *duration_, *pubkey_,
        *keypair_, *source_, *range_, *meta_, *account_info_, *tx_result_;
    const Type* ints_[11];
};

// -------------------------------------------------------------- symbols --

enum class SymbolKind { variable, keypair, source, function, schema, module, parameter, loop_var, tx_binding };

struct Module;

struct Symbol {
    SymbolKind kind = SymbolKind::variable;
    std::string name;
    Loc loc;
    const Type* type = nullptr;
    bool is_mutable = false;
    bool is_global = false;  // module-level binding (evaluated once, before networks)
    const Module* owner = nullptr;
    // function
    const ast::FunctionDecl* function = nullptr;
    std::vector<const Type*> param_types;
    const Type* return_type = nullptr;
    // schema
    const ast::SchemaDecl* schema = nullptr;
    std::string schema_format;            // "borsh" | "anchor"
    const Type* schema_record = nullptr;  // record type of values described by the schema
    // module
    const Module* module = nullptr;
};

struct Module {
    ast::Document* doc = nullptr;
    std::filesystem::path dir;  // base for relative paths in this module
    std::map<std::string, Symbol*, std::less<>> exports;
};

// Result of analysis; owns everything the interpreter needs besides the AST.
struct Program {
    TypeContext types;
    std::deque<Symbol> symbols;
    std::vector<std::unique_ptr<ast::Document>> imported_docs;
    std::vector<std::unique_ptr<Module>> modules;  // modules[0] is the root
    // Modules in dependency order (imports before importers, root last).
    std::vector<const Module*> init_order;
    const Module* root() const { return modules.front().get(); }
    const ast::NetworkDecl* find_network(std::string_view name) const;
    std::vector<const ast::NetworkDecl*> networks() const;
};

// Analyzes `root` (and transitively every import, which is parsed on demand).
// Returns nullptr if any error was reported.
std::unique_ptr<Program> analyze(ast::Document& root, SourceManager& sources, Diagnostics& diags);

// Builtin metadata shared with the interpreter.
const char* builtin_name(ast::BuiltinId id);

}  // namespace tide::sema
