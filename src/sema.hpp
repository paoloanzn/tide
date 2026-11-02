// Semantic analysis: symbol resolution, scoping, static typing, context rules
// and declaration validation. Runs to completion before anything executes;
// the interpreter relies on the decorations it leaves on the AST.
//
// The language semantics are documented in docs/language/.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ast.hpp"
#include "diag.hpp"

namespace tide::sema {

// ---------------------------------------------------------------- types --

enum class TypeKind : std::uint8_t {
    error,  // poison: suppresses cascading diagnostics
    any,    // only as an expected type (builtins, config keys, empty lists)
    void_,
    null,
    bool_,
    int_,
    float_,
    string,
    bytes,
    duration,
    pubkey,
    keypair,
    source,
    range,
    list,
    option,
    record,
    tuple,
    account_meta,
    schema,
    module,
};

// Integer refinements used by annotations and schemas. Values are always
// 128-bit at runtime; a refined slot is range-checked on store/serialize.
enum class IntWidth : std::uint8_t { any, u8, u16, u32, u64, u128, i8, i16, i32, i64, i128 };
inline constexpr std::array all_int_widths = {IntWidth::any, IntWidth::u8,   IntWidth::u16, IntWidth::u32,
                                              IntWidth::u64, IntWidth::u128, IntWidth::i8,  IntWidth::i16,
                                              IntWidth::i32, IntWidth::i64,  IntWidth::i128};
static_assert(std::size(all_int_widths) == static_cast<std::size_t>(IntWidth::i128) + 1,
              "all_int_widths must list every IntWidth");

struct Type {
    TypeKind kind = TypeKind::error;
    IntWidth width = IntWidth::any;                           // int_ only
    bool is_f32 = false;                                      // float_: 32-bit in schemas (f64 otherwise)
    const Type* elem = nullptr;                               // list, option
    std::optional<std::uint64_t> fixed_len;                   // list from `[T; N]`
    std::vector<std::pair<std::string, const Type*>> fields;  // record (ordered)
    std::vector<const Type*> elems;                           // tuple
    const struct Symbol* schema = nullptr;                    // schema; record decoded from a schema
    const struct Module* module = nullptr;                    // module

    [[nodiscard]] const Type* field(std::string_view name) const {
        for (const auto& [n, t] : fields)
            if (n == name) return t;
        return nullptr;
    }
};

std::string to_string(const Type* t);
std::string to_string(IntWidth w);
// Inclusive bounds for a refined integer width.
std::pair<i128, i128> int_bounds(IntWidth w);

class TypeContext {
public:
    TypeContext();
    [[nodiscard]] const Type* error() const { return error_; }
    [[nodiscard]] const Type* any() const { return any_; }
    [[nodiscard]] const Type* void_() const { return void_t_; }
    [[nodiscard]] const Type* null() const { return null_; }
    [[nodiscard]] const Type* bool_() const { return bool_t_; }
    [[nodiscard]] const Type* int_(IntWidth w = IntWidth::any) const { return ints_.at(static_cast<std::size_t>(w)); }
    [[nodiscard]] const Type* float_() const { return float_t_; }
    [[nodiscard]] const Type* float32() const { return float32_; }
    [[nodiscard]] const Type* string() const { return string_; }
    [[nodiscard]] const Type* bytes() const { return bytes_; }
    [[nodiscard]] const Type* duration() const { return duration_; }
    [[nodiscard]] const Type* pubkey() const { return pubkey_; }
    [[nodiscard]] const Type* keypair() const { return keypair_; }
    [[nodiscard]] const Type* source() const { return source_; }
    [[nodiscard]] const Type* range() const { return range_; }
    [[nodiscard]] const Type* account_meta() const { return meta_; }
    const Type* list(const Type* elem, std::optional<std::uint64_t> len = std::nullopt);
    const Type* option(const Type* inner);
    const Type* record(std::vector<std::pair<std::string, const Type*>> fields, const Symbol* schema = nullptr);
    const Type* tuple(std::vector<const Type*> elems);
    const Type* schema(const Symbol* s);
    const Type* module(const Module* m);
    // Record shapes produced by the standard library.
    [[nodiscard]] const Type* account_info() const { return account_info_; }
    [[nodiscard]] const Type* tx_result() const { return tx_result_; }

private:
    const Type* make(Type t);
    std::deque<Type> arena_;
    const Type *error_, *any_, *float32_, *void_t_, *null_, *bool_t_, *float_t_, *string_, *bytes_, *duration_,
        *pubkey_, *keypair_, *source_, *range_, *meta_, *account_info_, *tx_result_;
    std::array<const Type*, std::size(all_int_widths)> ints_{};
};

// -------------------------------------------------------------- symbols --

// Binary format of a `schema ... using <format>` declaration.
enum class SchemaFormat : std::uint8_t { borsh, anchor };

enum class SymbolKind : std::uint8_t {
    variable,
    keypair,
    source,
    function,
    schema,
    module,
    parameter,
    loop_var,
    tx_binding
};

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
    SchemaFormat schema_format = SchemaFormat::borsh;
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
    [[nodiscard]] const Module* root() const { return modules.front().get(); }
    [[nodiscard]] const ast::NetworkDecl* find_network(std::string_view name) const;
    [[nodiscard]] std::vector<const ast::NetworkDecl*> networks() const;
};

// Analyzes `root` (and transitively every import, which is parsed on demand).
// Returns nullptr if any error was reported.
std::unique_ptr<Program> analyze(ast::Document& root, SourceManager& sources, Diagnostics& diags);

// Builtin metadata shared with the interpreter.
const char* builtin_name(ast::BuiltinId id);

}  // namespace tide::sema
