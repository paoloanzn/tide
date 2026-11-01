// Tide grammar (see docs/language/) expressed as lexy productions.
//
// Deliberate leniencies, all re-checked with better diagnostics in sema:
//  * Statement bodies share one statement grammar; which statements are legal
//    in genesis / bootstrap / scenario / fn bodies is enforced by the checker.
//  * Record keys, entity field names, member names and object keys accept any
//    word (including reserved ones such as `mint`), validated per context.
//  * `else if` is accepted as sugar for `else { if ... }`.
//  * Trailing commas are accepted in list, object and argument lists.
#include "parser.hpp"

#include <lexy/action/parse.hpp>
#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>
#include <lexy/input/string_input.hpp>

#include <array>
#include <cctype>

namespace tide {
namespace {

namespace dsl = lexy::dsl;
using namespace ast;
using Pos = const char*;

// ------------------------------------------------------------- lexical --

constexpr auto ws = dsl::ascii::space | LEXY_LIT("//") >> dsl::until(dsl::newline).or_eof() |
                    LEXY_LIT("/*") >> dsl::until(LEXY_LIT("*/"));

constexpr auto id = dsl::identifier(dsl::ascii::alpha_underscore, dsl::ascii::alpha_digit_underscore);

// Single source of truth for keywords. Reserved words can never be
// identifiers; contextual ones are keywords only where the grammar expects them.
#define TIDE_RESERVED_KEYWORDS(X)                                                                         \
    X(import) X(as) X(source) X(keypair) X(let) X(schema) X(fn) X(network) X(options) X(validator)        \
    X(genesis) X(bootstrap) X(scenario) X(if) X(else) X(for) X(in) X(while) X(break) X(continue)          \
    X(return) X(true) X(false) X(null) X(transaction) X(instruction) X(invoke) X(deploy) X(airdrop)       \
    X(warp) X(snapshot) X(restore) X(assert) X(await) X(account) X(program) X(mint) X(token_account)      \
    X(associated_token_account) X(lookup_table) X(sysvar) X(feature) X(fork) X(clone) X(delete)           \
    X(extension)
#define TIDE_CONTEXTUAL_KEYWORDS(X)                                                                       \
    X(using) X(from) X(with) X(at) X(slot) X(to) X(by) X(slots) X(epoch) X(time) X(amount) X(enabled)     \
    X(disabled) X(SOL) X(lamports) X(ms) X(s)

#define TIDE_KW_DEFINE(name) constexpr auto kw_##name = LEXY_KEYWORD(#name, id);
TIDE_RESERVED_KEYWORDS(TIDE_KW_DEFINE)
TIDE_CONTEXTUAL_KEYWORDS(TIDE_KW_DEFINE)
#undef TIDE_KW_DEFINE

#define TIDE_KW_STRING(name) #name,
constexpr std::array reserved_words = {TIDE_RESERVED_KEYWORDS(TIDE_KW_STRING)};
#undef TIDE_KW_STRING

#define TIDE_KW_SET_ELEMENT(name) / LEXY_LIT(#name)
constexpr auto reserved_ident = id.reserve(dsl::literal_set() TIDE_RESERVED_KEYWORDS(TIDE_KW_SET_ELEMENT));
#undef TIDE_KW_SET_ELEMENT

// --------------------------------------------------------- error tags --

#define TIDE_ERR(tag, msg) \
    struct tag {           \
        static constexpr auto name = msg; \
    }
TIDE_ERR(expected_expression, "expected an expression");
TIDE_ERR(expected_declaration,
         "expected a declaration (import, source, keypair, let, schema, fn or network)");
TIDE_ERR(expected_network_entry,
         "expected a network entry (options, validator, source, keypair, let, genesis, bootstrap or scenario)");
TIDE_ERR(expected_feature_status, "expected 'enabled' or 'disabled'");
TIDE_ERR(expected_clone_kind, "expected 'account' or 'program'");
TIDE_ERR(expected_warp_target, "expected 'to slot', 'to epoch', 'to time' or 'by slots'");
TIDE_ERR(invalid_string_char, "invalid character in string literal");
#undef TIDE_ERR

// ------------------------------------------------------------ helpers --

template <typename T>
constexpr auto as_opt = lexy::callback<std::optional<T>>([](lexy::nullopt) { return std::optional<T>{}; },
                                                         [](T&& v) { return std::optional<T>(std::move(v)); });

template <typename Lexeme>
Loc loc_of(const Lexeme& lex) {
    return Loc{lex.begin(), static_cast<std::uint32_t>(lex.size())};
}

template <typename N>
ExprPtr mk_expr(Loc loc, N&& n) {
    auto e = std::make_unique<Expr>();
    e->loc = loc;
    e->node = std::forward<N>(n);
    return e;
}

constexpr auto make_ident = lexy::callback<Ident>(
    [](auto lex) { return Ident{loc_of(lex), std::string(lex.begin(), lex.end())}; });

struct expr;
struct stmt;
struct type_p;
struct record_block_p;

// -------------------------------------------------------------- names --

struct ident_p {
    static constexpr auto name = "identifier";
    static constexpr auto rule = reserved_ident;
    static constexpr auto value = make_ident;
};

// Any identifier-shaped word, reserved or not (record keys, fields, members).
struct word_p {
    static constexpr auto name = "name";
    static constexpr auto rule = id;
    static constexpr auto value = make_ident;
};

// ----------------------------------------------------------- literals --

struct string_tok : lexy::token_production {
    static constexpr auto name = "string literal";
    static constexpr auto escaped = lexy::symbol_table<char>
                                        .map<'"'>('"')
                                        .map<'\\'>('\\')
                                        .map<'/'>('/')
                                        .map<'b'>('\b')
                                        .map<'f'>('\f')
                                        .map<'n'>('\n')
                                        .map<'r'>('\r')
                                        .map<'t'>('\t');
    static constexpr auto rule = [] {
        auto cp = (-dsl::unicode::control).error<invalid_string_char>;
        auto esc = dsl::backslash_escape.symbol<escaped>().rule(dsl::lit_c<'u'> >> dsl::code_point_id<4>);
        return dsl::quoted.limit(dsl::ascii::newline)(cp, esc);
    }();
    static constexpr auto value = lexy::as_string<std::string, lexy::utf8_char_encoding>;
};

struct string_expr {
    static constexpr auto rule = dsl::position + dsl::p<string_tok>;
    static constexpr auto value = lexy::callback<ExprPtr>([](Pos p, std::string s) {
        return mk_expr(Loc{p, static_cast<std::uint32_t>(s.size() + 2)}, StringLit{std::move(s)});
    });
};

struct unit_p {
    static constexpr auto name = "unit";
    static constexpr auto rule = dsl::capture(kw_SOL) | dsl::capture(kw_lamports) | dsl::capture(kw_ms) |
                                 dsl::capture(kw_s) | dsl::capture(kw_slots);
    static constexpr auto value = lexy::callback<std::pair<Unit, Loc>>([](auto lex) {
        std::string_view t(lex.begin(), lex.size());
        Unit u = t == "SOL" ? Unit::sol : t == "lamports" ? Unit::lamports : t == "ms" ? Unit::ms
               : t == "s"   ? Unit::s
                            : Unit::slots;
        return std::pair{u, loc_of(lex)};
    });
};

struct number_expr {
    static constexpr auto name = "number";
    static constexpr auto rule =
        dsl::capture(dsl::token(dsl::digits<> + dsl::opt(dsl::peek(dsl::period + dsl::digit<>) >>
                                                         dsl::period + dsl::digits<>))) +
        dsl::opt(dsl::p<unit_p>);
    static constexpr auto value = lexy::callback<ExprPtr>(
        [](auto lex, lexy::nullopt) {
            std::string text(lex.begin(), lex.end());
            bool is_float = text.find('.') != std::string::npos;
            return mk_expr(loc_of(lex), NumberLit{std::move(text), is_float, Unit::none, {}});
        },
        [](auto lex, std::pair<Unit, Loc> unit) {
            std::string text(lex.begin(), lex.end());
            bool is_float = text.find('.') != std::string::npos;
            Loc loc{lex.begin(), static_cast<std::uint32_t>(unit.second.ptr + unit.second.len - lex.begin())};
            return mk_expr(loc, NumberLit{std::move(text), is_float, unit.first, unit.second});
        });
};

struct bool_expr {
    static constexpr auto rule = dsl::capture(kw_true) | dsl::capture(kw_false);
    static constexpr auto value = lexy::callback<ExprPtr>(
        [](auto lex) { return mk_expr(loc_of(lex), BoolLit{*lex.begin() == 't'}); });
};

struct null_expr {
    static constexpr auto rule = dsl::capture(kw_null);
    static constexpr auto value = lexy::callback<ExprPtr>([](auto lex) { return mk_expr(loc_of(lex), NullLit{}); });
};

struct ident_expr {
    static constexpr auto rule = dsl::p<ident_p>;
    static constexpr auto value =
        lexy::callback<ExprPtr>([](Ident i) { return mk_expr(i.loc, IdentExpr{std::move(i.name), nullptr}); });
};

struct paren_expr {
    static constexpr auto rule = dsl::lit_c<'('> >> dsl::recurse<expr> + dsl::lit_c<')'>;
    static constexpr auto value = lexy::forward<ExprPtr>;
};

struct expr_list_until_rbracket {
    static constexpr auto rule =
        dsl::terminator(dsl::lit_c<']'>).opt_list(dsl::recurse<expr>, dsl::trailing_sep(dsl::comma));
    static constexpr auto value = lexy::as_list<std::vector<ExprPtr>>;
};

struct expr_list_until_rparen {
    static constexpr auto rule =
        dsl::terminator(dsl::lit_c<')'>).opt_list(dsl::recurse<expr>, dsl::trailing_sep(dsl::comma));
    static constexpr auto value = lexy::as_list<std::vector<ExprPtr>>;
};

struct list_expr {
    static constexpr auto rule = dsl::capture(dsl::lit_c<'['>) >> dsl::p<expr_list_until_rbracket>;
    static constexpr auto value = lexy::callback<ExprPtr>(
        [](auto lex, std::vector<ExprPtr> elems) { return mk_expr(loc_of(lex), ListLit{std::move(elems)}); });
};

struct object_key_p {
    static constexpr auto name = "object key";
    static constexpr auto rule =
        dsl::peek(dsl::lit_c<'"'>) >> dsl::position + dsl::p<string_tok> | dsl::else_ >> dsl::p<word_p>;
    static constexpr auto value = lexy::callback<Ident>(
        [](Pos p, std::string s) { return Ident{Loc{p, static_cast<std::uint32_t>(s.size() + 2)}, std::move(s)}; },
        [](Ident i) { return i; });
};

struct object_field_p {
    static constexpr auto name = "object field";
    static constexpr auto rule = dsl::p<object_key_p> + dsl::colon + dsl::recurse<expr>;
    static constexpr auto value = lexy::callback<ObjectField>(
        [](Ident k, ExprPtr v) { return ObjectField{k.loc, std::move(k.name), std::move(v)}; });
};

struct object_fields_until_rbrace {
    static constexpr auto rule =
        dsl::terminator(dsl::lit_c<'}'>).opt_list(dsl::p<object_field_p>, dsl::trailing_sep(dsl::comma));
    static constexpr auto value = lexy::as_list<std::vector<ObjectField>>;
};

struct object_expr {
    static constexpr auto rule = dsl::capture(dsl::lit_c<'{'>) >> dsl::p<object_fields_until_rbrace>;
    static constexpr auto value = lexy::callback<ExprPtr>([](auto lex, std::vector<ObjectField> fields) {
        return mk_expr(loc_of(lex), ObjectLit{std::move(fields)});
    });
};

struct atom_p {
    static constexpr auto name = "expression";
    static constexpr auto rule = dsl::peek(dsl::ascii::digit) >> dsl::p<number_expr> |
                                 dsl::peek(dsl::lit_c<'"'>) >> dsl::p<string_expr> | dsl::p<bool_expr> |
                                 dsl::p<null_expr> | dsl::p<paren_expr> | dsl::p<list_expr> |
                                 dsl::p<object_expr> | dsl::peek(dsl::ascii::alpha_underscore) >> dsl::p<ident_expr> |
                                 dsl::error<expected_expression>;
    static constexpr auto value = lexy::forward<ExprPtr>;
};

// --------------------------------------------------------- expressions --

template <BinaryOp Op>
struct bin_tag {
    Pos pos;
    constexpr explicit bin_tag(Pos p) : pos(p) {}
};
template <UnaryOp Op>
struct un_tag {
    Pos pos;
    constexpr explicit un_tag(Pos p) : pos(p) {}
};
struct member_tag {
    Pos pos;
    constexpr explicit member_tag(Pos p) : pos(p) {}
};
struct index_tag {
    Pos pos;
    constexpr explicit index_tag(Pos p) : pos(p) {}
};
struct call_tag {
    Pos pos;
    constexpr explicit call_tag(Pos p) : pos(p) {}
};

constexpr std::uint32_t op_len(BinaryOp op) {
    switch (op) {
        case BinaryOp::or_: case BinaryOp::and_: case BinaryOp::eq: case BinaryOp::ne: case BinaryOp::le:
        case BinaryOp::ge: case BinaryOp::range: return 2;
        case BinaryOp::range_incl: return 3;
        default: return 1;
    }
}

template <char C>
constexpr auto op_no_eq = dsl::not_followed_by(dsl::lit_c<C>, dsl::lit_c<'='>);

struct expr : lexy::expression_production {
    static constexpr auto name = "expression";
    static constexpr auto atom = dsl::p<atom_p>;

    struct postfix : dsl::postfix_op {
        static constexpr auto op =
            dsl::op<member_tag>(dsl::not_followed_by(dsl::lit_c<'.'>, dsl::lit_c<'.'>) >> dsl::p<word_p>) /
            dsl::op<index_tag>(dsl::lit_c<'['> >> dsl::recurse<expr> + dsl::lit_c<']'>) /
            dsl::op<call_tag>(dsl::lit_c<'('> >> dsl::p<expr_list_until_rparen>);
        using operand = dsl::atom;
    };
    struct prefix : dsl::prefix_op {
        static constexpr auto op = dsl::op<un_tag<UnaryOp::neg>>(op_no_eq<'-'>) /
                                   dsl::op<un_tag<UnaryOp::not_>>(op_no_eq<'!'>);
        using operand = postfix;
    };
    struct multiplicative : dsl::infix_op_left {
        static constexpr auto op = dsl::op<bin_tag<BinaryOp::mul>>(op_no_eq<'*'>) /
                                   dsl::op<bin_tag<BinaryOp::div>>(op_no_eq<'/'>) /
                                   dsl::op<bin_tag<BinaryOp::mod>>(op_no_eq<'%'>);
        using operand = prefix;
    };
    struct additive : dsl::infix_op_left {
        static constexpr auto op = dsl::op<bin_tag<BinaryOp::add>>(op_no_eq<'+'>) /
                                   dsl::op<bin_tag<BinaryOp::sub>>(op_no_eq<'-'>);
        using operand = multiplicative;
    };
    struct range : dsl::infix_op_single {
        static constexpr auto op = dsl::op<bin_tag<BinaryOp::range_incl>>(LEXY_LIT("..=")) /
                                   dsl::op<bin_tag<BinaryOp::range>>(LEXY_LIT(".."));
        using operand = additive;
    };
    struct comparison : dsl::infix_op_left {
        static constexpr auto op = dsl::op<bin_tag<BinaryOp::le>>(LEXY_LIT("<=")) /
                                   dsl::op<bin_tag<BinaryOp::ge>>(LEXY_LIT(">=")) /
                                   dsl::op<bin_tag<BinaryOp::lt>>(op_no_eq<'<'>) /
                                   dsl::op<bin_tag<BinaryOp::gt>>(op_no_eq<'>'>);
        using operand = range;
    };
    struct equality : dsl::infix_op_left {
        static constexpr auto op = dsl::op<bin_tag<BinaryOp::eq>>(LEXY_LIT("==")) /
                                   dsl::op<bin_tag<BinaryOp::ne>>(LEXY_LIT("!="));
        using operand = comparison;
    };
    struct logical_and : dsl::infix_op_left {
        static constexpr auto op = dsl::op<bin_tag<BinaryOp::and_>>(LEXY_LIT("&&"));
        using operand = equality;
    };
    struct logical_or : dsl::infix_op_left {
        static constexpr auto op = dsl::op<bin_tag<BinaryOp::or_>>(LEXY_LIT("||"));
        using operand = logical_and;
    };
    using operation = logical_or;

    static constexpr auto value = lexy::callback<ExprPtr>(
        [](ExprPtr e) { return e; },
        []<BinaryOp Op>(ExprPtr l, bin_tag<Op> t, ExprPtr r) {
            Loc loc = l->loc;
            return mk_expr(loc, BinaryExpr{Op, Loc{t.pos, op_len(Op)}, std::move(l), std::move(r)});
        },
        []<UnaryOp Op>(un_tag<Op> t, ExprPtr x) { return mk_expr(Loc{t.pos, 1}, UnaryExpr{Op, std::move(x)}); },
        [](ExprPtr o, member_tag, Ident m) {
            Loc loc = o->loc;
            return mk_expr(loc, MemberExpr{std::move(o), std::move(m), nullptr});
        },
        [](ExprPtr o, index_tag t, ExprPtr i) {
            return mk_expr(Loc{t.pos, 1}, IndexExpr{std::move(o), std::move(i)});
        },
        [](ExprPtr o, call_tag, std::vector<ExprPtr> args) {
            Loc loc = o->loc;
            return mk_expr(loc, CallExpr{std::move(o), std::move(args), BuiltinId::none, nullptr});
        });
};

// --------------------------------------------------------------- types --

struct type_list_until_rparen {
    static constexpr auto rule = dsl::terminator(dsl::lit_c<')'>).opt_list(dsl::recurse<type_p>, dsl::sep(dsl::comma));
    static constexpr auto value = lexy::as_list<std::vector<TypeExpr>>;
};
struct type_list_until_gt {
    static constexpr auto rule = dsl::terminator(dsl::lit_c<'>'>).opt_list(dsl::recurse<type_p>, dsl::sep(dsl::comma));
    static constexpr auto value = lexy::as_list<std::vector<TypeExpr>>;
};

struct type_p {
    static constexpr auto name = "type";
    static constexpr auto rule =
        dsl::capture(dsl::lit_c<'['>) >> dsl::recurse<type_p> + dsl::opt(dsl::semicolon >> dsl::p<expr>) +
                                             dsl::lit_c<']'> |
        dsl::capture(dsl::lit_c<'('>) >> dsl::p<type_list_until_rparen> |
        dsl::else_ >> dsl::p<word_p> + dsl::opt(dsl::lit_c<'<'> >> dsl::p<type_list_until_gt>);
    static constexpr auto value = lexy::callback<TypeExpr>(
        [](auto lex, TypeExpr elem, lexy::nullopt) {
            TypeExpr t{loc_of(lex), TypeExpr::Kind::list, {}, {}, nullptr};
            t.args.push_back(std::move(elem));
            return t;
        },
        [](auto lex, TypeExpr elem, ExprPtr size) {
            TypeExpr t{loc_of(lex), TypeExpr::Kind::array, {}, {}, std::move(size)};
            t.args.push_back(std::move(elem));
            return t;
        },
        [](auto lex, std::vector<TypeExpr> elems) {
            return TypeExpr{loc_of(lex), TypeExpr::Kind::tuple, {}, std::move(elems), nullptr};
        },
        [](Ident n, lexy::nullopt) { return TypeExpr{n.loc, TypeExpr::Kind::named, std::move(n.name), {}, nullptr}; },
        [](Ident n, std::vector<TypeExpr> args) {
            return TypeExpr{n.loc, TypeExpr::Kind::named, std::move(n.name), std::move(args), nullptr};
        });
};

struct opt_type_annotation {
    static constexpr auto rule = dsl::opt(dsl::colon >> dsl::p<type_p>);
    static constexpr auto value = as_opt<TypeExpr>;
};

// ------------------------------------------------------------- records --

struct record_entry_p;

struct record_entries_until_rbrace {
    static constexpr auto rule = dsl::terminator(dsl::lit_c<'}'>).opt_list(dsl::recurse<record_entry_p>);
    static constexpr auto value = lexy::as_list<std::vector<RecordEntry>>;
};

struct record_block_p {
    static constexpr auto name = "record block";
    static constexpr auto rule = dsl::capture(dsl::lit_c<'{'>) >> dsl::p<record_entries_until_rbrace>;
    static constexpr auto value = lexy::callback<RecordBlock>(
        [](auto lex, std::vector<RecordEntry> es) { return RecordBlock{loc_of(lex), std::move(es)}; });
};

struct record_entry_p {
    static constexpr auto name = "record entry";
    static constexpr auto rule =
        dsl::capture(kw_extension) >> dsl::p<word_p> + dsl::recurse<record_block_p> |
        dsl::else_ >> dsl::p<word_p> + (dsl::lit_c<'='> >> dsl::p<expr> + dsl::semicolon |
                                        dsl::else_ >> dsl::recurse<record_block_p>);
    static constexpr auto value = lexy::callback<RecordEntry>(
        [](auto, Ident name, RecordBlock b) {
            return RecordEntry{name.loc, RecordEntry::Kind::extension, std::move(name.name), nullptr,
                               std::make_unique<RecordBlock>(std::move(b))};
        },
        [](Ident key, ExprPtr v) {
            return RecordEntry{key.loc, RecordEntry::Kind::value, std::move(key.name), std::move(v), nullptr};
        },
        [](Ident key, RecordBlock b) {
            return RecordEntry{key.loc, RecordEntry::Kind::nested, std::move(key.name), nullptr,
                               std::make_unique<RecordBlock>(std::move(b))};
        });
};

// ---------------------------------------------------------- statements --

struct block_p {
    static constexpr auto name = "block";
    static constexpr auto rule = dsl::curly_bracketed.opt_list(dsl::recurse<stmt>);
    static constexpr auto value = lexy::as_list<Block>;
};

constexpr auto opt_semicolon = dsl::if_(dsl::semicolon);

struct let_p {
    static constexpr auto rule =
        kw_let >> dsl::p<ident_p> + dsl::p<opt_type_annotation> + dsl::lit_c<'='> + dsl::p<expr> + dsl::semicolon;
    static constexpr auto value = lexy::callback<LetStmt>([](Ident n, std::optional<TypeExpr> t, ExprPtr e) {
        return LetStmt{BindingKind::let, std::move(n), std::move(t), std::move(e), nullptr};
    });
};
struct source_p {
    static constexpr auto rule = kw_source >> dsl::p<ident_p> + dsl::lit_c<'='> + dsl::p<expr> + dsl::semicolon;
    static constexpr auto value = lexy::callback<LetStmt>([](Ident n, ExprPtr e) {
        return LetStmt{BindingKind::source, std::move(n), std::nullopt, std::move(e), nullptr};
    });
};
struct keypair_p {
    static constexpr auto rule = kw_keypair >> dsl::p<ident_p> + dsl::lit_c<'='> + dsl::p<expr> + dsl::semicolon;
    static constexpr auto value = lexy::callback<LetStmt>([](Ident n, ExprPtr e) {
        return LetStmt{BindingKind::keypair, std::move(n), std::nullopt, std::move(e), nullptr};
    });
};

struct else_p {
    static constexpr auto rule =
        dsl::opt(kw_else >> (dsl::peek(kw_if) >> dsl::recurse<stmt> | dsl::else_ >> dsl::p<block_p>));
    static constexpr auto value = lexy::callback<std::optional<Block>>(
        [](lexy::nullopt) { return std::optional<Block>{}; },
        [](Block b) { return std::optional<Block>(std::move(b)); },
        [](StmtPtr s) {
            Block b;
            b.push_back(std::move(s));
            return std::optional<Block>(std::move(b));
        });
};

struct if_p {
    static constexpr auto rule = kw_if >> dsl::p<expr> + dsl::p<block_p> + dsl::p<else_p>;
    static constexpr auto value = lexy::callback<IfStmt>([](ExprPtr c, Block t, std::optional<Block> e) {
        return IfStmt{std::move(c), std::move(t), std::move(e)};
    });
};

struct for_p {
    static constexpr auto rule = kw_for >> dsl::p<ident_p> + kw_in + dsl::p<expr> + dsl::p<block_p>;
    static constexpr auto value = lexy::callback<ForStmt>(
        [](Ident v, ExprPtr it, Block b) { return ForStmt{std::move(v), std::move(it), std::move(b), nullptr}; });
};

struct while_p {
    static constexpr auto rule = kw_while >> dsl::p<expr> + dsl::p<block_p>;
    static constexpr auto value =
        lexy::callback<WhileStmt>([](ExprPtr c, Block b) { return WhileStmt{std::move(c), std::move(b)}; });
};

struct break_p {
    static constexpr auto rule = kw_break >> dsl::semicolon;
    static constexpr auto value = lexy::callback<BreakStmt>([] { return BreakStmt{}; });
};
struct continue_p {
    static constexpr auto rule = kw_continue >> dsl::semicolon;
    static constexpr auto value = lexy::callback<ContinueStmt>([] { return ContinueStmt{}; });
};

struct return_p {
    static constexpr auto rule = kw_return >> dsl::opt(dsl::peek_not(dsl::semicolon) >> dsl::p<expr>) + dsl::semicolon;
    static constexpr auto value = lexy::callback<ReturnStmt>([](lexy::nullopt) { return ReturnStmt{nullptr}; },
                                                             [](ExprPtr e) { return ReturnStmt{std::move(e)}; });
};

struct assert_p {
    static constexpr auto rule =
        kw_assert >> dsl::p<expr> + dsl::opt(dsl::comma >> dsl::p<string_tok>) + dsl::semicolon;
    static constexpr auto value = lexy::callback<AssertStmt>(
        [](ExprPtr c, lexy::nullopt) { return AssertStmt{std::move(c), std::nullopt}; },
        [](ExprPtr c, std::string m) { return AssertStmt{std::move(c), std::move(m)}; });
};

struct await_p {
    static constexpr auto rule = kw_await >> dsl::p<expr> + dsl::semicolon;
    static constexpr auto value = lexy::callback<AwaitStmt>([](ExprPtr e) { return AwaitStmt{std::move(e)}; });
};
struct snapshot_p {
    static constexpr auto rule = kw_snapshot >> dsl::p<expr> + dsl::semicolon;
    static constexpr auto value = lexy::callback<SnapshotStmt>([](ExprPtr e) { return SnapshotStmt{std::move(e)}; });
};
struct restore_p {
    static constexpr auto rule = kw_restore >> dsl::p<expr> + dsl::semicolon;
    static constexpr auto value = lexy::callback<RestoreStmt>([](ExprPtr e) { return RestoreStmt{std::move(e)}; });
};

template <EntityKind K>
constexpr auto entity_value = lexy::callback<EntityDecl>([](ExprPtr target, RecordBlock fields) {
    return EntityDecl{K, {}, std::move(target), std::move(fields.entries)};
});

struct account_p {
    static constexpr auto rule = kw_account >> dsl::p<expr> + dsl::p<record_block_p>;
    static constexpr auto value = entity_value<EntityKind::account>;
};
struct mint_p {
    static constexpr auto rule = kw_mint >> dsl::p<expr> + dsl::p<record_block_p>;
    static constexpr auto value = entity_value<EntityKind::mint>;
};
struct token_account_p {
    static constexpr auto rule = kw_token_account >> dsl::p<expr> + dsl::p<record_block_p>;
    static constexpr auto value = entity_value<EntityKind::token_account>;
};
struct ata_p {
    static constexpr auto rule = kw_associated_token_account >> dsl::p<expr> + dsl::p<record_block_p>;
    static constexpr auto value = entity_value<EntityKind::associated_token_account>;
};
struct lookup_table_p {
    static constexpr auto rule = kw_lookup_table >> dsl::p<expr> + dsl::p<record_block_p>;
    static constexpr auto value = entity_value<EntityKind::lookup_table>;
};
struct program_p {
    static constexpr auto rule = kw_program >> dsl::p<ident_p> + dsl::p<record_block_p>;
    static constexpr auto value = lexy::callback<EntityDecl>([](Ident label, RecordBlock fields) {
        return EntityDecl{EntityKind::program, std::move(label), nullptr, std::move(fields.entries)};
    });
};

struct sysvar_p {
    static constexpr auto rule = kw_sysvar >> dsl::p<ident_p> + dsl::p<record_block_p>;
    static constexpr auto value =
        lexy::callback<SysvarDecl>([](Ident n, RecordBlock b) { return SysvarDecl{std::move(n), std::move(b)}; });
};

struct feature_p {
    static constexpr auto rule =
        kw_feature >> dsl::p<expr> + dsl::lit_c<'='> +
                          (dsl::capture(kw_enabled) | dsl::capture(kw_disabled) |
                           dsl::error<expected_feature_status>) +
                          dsl::semicolon;
    static constexpr auto value = lexy::callback<FeatureDecl>(
        [](ExprPtr f, auto status) { return FeatureDecl{std::move(f), *status.begin() == 'e'}; });
};

struct at_slot_p {
    static constexpr auto rule = dsl::opt(kw_at >> kw_slot + dsl::p<expr>);
    static constexpr auto value = lexy::callback<ExprPtr>([](lexy::nullopt) { return ExprPtr{}; },
                                                          [](ExprPtr e) { return e; });
};

struct fork_p {
    static constexpr auto rule = kw_fork >> kw_from + dsl::p<expr> + dsl::p<at_slot_p> + dsl::semicolon;
    static constexpr auto value =
        lexy::callback<ForkDecl>([](ExprPtr src, ExprPtr at) { return ForkDecl{std::move(src), std::move(at)}; });
};

struct clone_kind_p {
    static constexpr auto rule =
        dsl::capture(kw_account) | dsl::capture(kw_program) | dsl::error<expected_clone_kind>;
    static constexpr auto value = lexy::callback<bool>([](auto lex) { return *lex.begin() == 'p'; });
};

struct opt_record_block_p {
    static constexpr auto rule = dsl::opt(dsl::p<record_block_p>);
    static constexpr auto value = as_opt<RecordBlock>;
};

struct clone_p {
    static constexpr auto rule = kw_clone >> dsl::p<clone_kind_p> + dsl::p<expr> + kw_from + dsl::p<expr> +
                                                 dsl::p<at_slot_p> + dsl::p<opt_record_block_p> + dsl::semicolon;
    static constexpr auto value = lexy::callback<CloneDecl>(
        [](bool program, ExprPtr target, ExprPtr src, ExprPtr at, std::optional<RecordBlock> overrides) {
            return CloneDecl{program, std::move(target), std::move(src), std::move(at), std::move(overrides)};
        });
};

struct delete_p {
    static constexpr auto rule = kw_delete >> dsl::p<clone_kind_p> + dsl::p<expr> + dsl::semicolon;
    static constexpr auto value =
        lexy::callback<DeleteDecl>([](bool program, ExprPtr t) { return DeleteDecl{program, std::move(t)}; });
};

struct tx_entry_p {
    static constexpr auto name = "transaction entry";
    static constexpr auto rule =
        dsl::capture(kw_instruction) >> dsl::p<expr> + dsl::p<record_block_p> + opt_semicolon |
        dsl::else_ >> dsl::p<word_p> + dsl::lit_c<'='> + dsl::p<expr> + dsl::semicolon;
    static constexpr auto value = lexy::callback<TxEntry>(
        [](auto lex, ExprPtr program, RecordBlock body) {
            return TxEntry{loc_of(lex), true, {}, nullptr, std::move(program), std::move(body)};
        },
        [](Ident key, ExprPtr v) { return TxEntry{key.loc, false, std::move(key.name), std::move(v), nullptr, {}}; });
};

struct tx_entries_p {
    static constexpr auto rule = dsl::curly_bracketed.opt_list(dsl::p<tx_entry_p>);
    static constexpr auto value = lexy::as_list<std::vector<TxEntry>>;
};

struct tx_binding_p {
    static constexpr auto rule = dsl::opt(kw_as >> dsl::p<ident_p>);
    static constexpr auto value = as_opt<Ident>;
};

struct transaction_p {
    static constexpr auto rule = kw_transaction >> dsl::p<tx_binding_p> + dsl::p<tx_entries_p>;
    static constexpr auto value = lexy::callback<TransactionStmt>(
        [](std::optional<Ident> b, std::vector<TxEntry> es) { return TransactionStmt{std::move(b), std::move(es), nullptr}; });
};

struct invoke_p {
    static constexpr auto rule = kw_invoke >> dsl::p<expr> + kw_with + dsl::p<record_block_p> + dsl::semicolon;
    static constexpr auto value =
        lexy::callback<InvokeStmt>([](ExprPtr p, RecordBlock w) { return InvokeStmt{std::move(p), std::move(w)}; });
};
struct deploy_p {
    static constexpr auto rule = kw_deploy >> dsl::p<expr> + kw_from + dsl::p<expr> + dsl::semicolon;
    static constexpr auto value =
        lexy::callback<DeployStmt>([](ExprPtr p, ExprPtr f) { return DeployStmt{std::move(p), std::move(f)}; });
};
struct airdrop_p {
    static constexpr auto rule = kw_airdrop >> dsl::p<expr> + kw_amount + dsl::p<expr> + dsl::semicolon;
    static constexpr auto value =
        lexy::callback<AirdropStmt>([](ExprPtr t, ExprPtr a) { return AirdropStmt{std::move(t), std::move(a)}; });
};

struct warp_kind_p {
    static constexpr auto rule =
        kw_to >> (dsl::capture(kw_slot) | dsl::capture(kw_epoch) | dsl::capture(kw_time) |
                  dsl::error<expected_warp_target>) |
        dsl::capture(kw_by) >> kw_slots | dsl::error<expected_warp_target>;
    static constexpr auto value = lexy::callback<WarpKind>([](auto lex) {
        std::string_view t(lex.begin(), lex.size());
        return t == "slot" ? WarpKind::to_slot : t == "epoch" ? WarpKind::to_epoch
             : t == "time" ? WarpKind::to_time : WarpKind::by_slots;
    });
};
struct warp_p {
    static constexpr auto rule = kw_warp >> dsl::p<warp_kind_p> + dsl::p<expr> + dsl::semicolon;
    static constexpr auto value =
        lexy::callback<WarpStmt>([](WarpKind k, ExprPtr v) { return WarpStmt{k, std::move(v)}; });
};

struct assign_op_p {
    static constexpr auto rule =
        dsl::capture(LEXY_LIT("+=")) | dsl::capture(LEXY_LIT("-=")) | dsl::capture(LEXY_LIT("*=")) |
        dsl::capture(LEXY_LIT("/=")) | dsl::capture(LEXY_LIT("%=")) | dsl::capture(op_no_eq<'='>);
    static constexpr auto value = lexy::callback<AssignOp>([](auto lex) {
        switch (*lex.begin()) {
            case '+': return AssignOp::add;
            case '-': return AssignOp::sub;
            case '*': return AssignOp::mul;
            case '/': return AssignOp::div;
            case '%': return AssignOp::mod;
            default: return AssignOp::set;
        }
    });
};

using StmtNode = decltype(Stmt::node);

struct expr_stmt_p {
    static constexpr auto rule = dsl::p<expr> + dsl::opt(dsl::p<assign_op_p> >> dsl::p<expr>) + dsl::semicolon;
    static constexpr auto value = lexy::callback<StmtNode>(
        [](ExprPtr e, lexy::nullopt) { return StmtNode(ExprStmt{std::move(e)}); },
        [](ExprPtr target, AssignOp op, ExprPtr v) { return StmtNode(AssignStmt{std::move(target), op, std::move(v)}); });
};

struct stmt {
    static constexpr auto name = "statement";
    static constexpr auto rule =
        dsl::position +
        (dsl::p<let_p> | dsl::p<source_p> | dsl::p<keypair_p> | dsl::p<if_p> | dsl::p<for_p> | dsl::p<while_p> |
         dsl::p<break_p> | dsl::p<continue_p> | dsl::p<return_p> | dsl::p<assert_p> | dsl::p<await_p> |
         dsl::p<account_p> | dsl::p<program_p> | dsl::p<mint_p> | dsl::p<token_account_p> | dsl::p<ata_p> |
         dsl::p<lookup_table_p> | dsl::p<sysvar_p> | dsl::p<feature_p> | dsl::p<fork_p> | dsl::p<clone_p> |
         dsl::p<delete_p> | dsl::p<transaction_p> | dsl::p<invoke_p> | dsl::p<deploy_p> | dsl::p<airdrop_p> |
         dsl::p<warp_p> | dsl::p<snapshot_p> | dsl::p<restore_p> | dsl::else_ >> dsl::p<expr_stmt_p>);
    static constexpr auto value = lexy::callback<StmtPtr>([](Pos p, auto&& node) {
        auto s = std::make_unique<Stmt>();
        s->loc = Loc{p, 0};
        s->node = std::move(node);
        return s;
    });
};

// -------------------------------------------------------- declarations --

struct import_p {
    static constexpr auto rule =
        kw_import >> dsl::position + dsl::p<string_tok> + kw_as + dsl::p<ident_p> + dsl::semicolon;
    static constexpr auto value = lexy::callback<ImportDecl>([](Pos p, std::string path, Ident alias) {
        return ImportDecl{Loc{p, static_cast<std::uint32_t>(path.size() + 2)}, std::move(path), std::move(alias)};
    });
};

struct schema_field_p {
    static constexpr auto name = "schema field";
    static constexpr auto rule = dsl::p<word_p> + dsl::colon + dsl::p<type_p> + dsl::semicolon;
    static constexpr auto value =
        lexy::callback<SchemaField>([](Ident n, TypeExpr t) { return SchemaField{std::move(n), std::move(t)}; });
};
struct schema_fields_p {
    static constexpr auto rule = dsl::curly_bracketed.opt_list(dsl::p<schema_field_p>);
    static constexpr auto value = lexy::as_list<std::vector<SchemaField>>;
};
struct schema_p {
    static constexpr auto rule = kw_schema >> dsl::p<ident_p> + kw_using + dsl::p<ident_p> + dsl::p<schema_fields_p>;
    static constexpr auto value = lexy::callback<SchemaDecl>([](Ident n, Ident fmt, std::vector<SchemaField> fs) {
        return SchemaDecl{std::move(n), std::move(fmt), std::move(fs), nullptr};
    });
};

struct param_p {
    static constexpr auto name = "parameter";
    static constexpr auto rule = dsl::p<ident_p> + dsl::colon + dsl::p<type_p>;
    static constexpr auto value =
        lexy::callback<Param>([](Ident n, TypeExpr t) { return Param{std::move(n), std::move(t), nullptr}; });
};
struct params_p {
    static constexpr auto rule = dsl::parenthesized.opt_list(dsl::p<param_p>, dsl::sep(dsl::comma));
    static constexpr auto value = lexy::as_list<std::vector<Param>>;
};
struct return_type_p {
    static constexpr auto rule = dsl::opt(LEXY_LIT("->") >> dsl::p<type_p>);
    static constexpr auto value = as_opt<TypeExpr>;
};
struct fn_p {
    static constexpr auto rule = kw_fn >> dsl::p<ident_p> + dsl::p<params_p> + dsl::p<return_type_p> + dsl::p<block_p>;
    static constexpr auto value =
        lexy::callback<FunctionDecl>([](Ident n, std::vector<Param> ps, std::optional<TypeExpr> r, Block b) {
            return FunctionDecl{std::move(n), std::move(ps), std::move(r), std::move(b), nullptr};
        });
};

struct options_entry_p {
    static constexpr auto rule = kw_options >> dsl::p<record_block_p>;
    static constexpr auto value = lexy::callback<OptionsEntry>([](RecordBlock b) { return OptionsEntry{std::move(b)}; });
};
struct validator_entry_p {
    static constexpr auto rule = kw_validator >> dsl::p<ident_p> + dsl::p<record_block_p>;
    static constexpr auto value = lexy::callback<ValidatorEntry>(
        [](Ident n, RecordBlock b) { return ValidatorEntry{std::move(n), std::move(b)}; });
};
struct genesis_entry_p {
    static constexpr auto rule = kw_genesis >> dsl::p<block_p>;
    static constexpr auto value = lexy::callback<GenesisEntry>([](Block b) { return GenesisEntry{std::move(b)}; });
};
struct bootstrap_entry_p {
    static constexpr auto rule = kw_bootstrap >> dsl::p<block_p>;
    static constexpr auto value = lexy::callback<BootstrapEntry>([](Block b) { return BootstrapEntry{std::move(b)}; });
};
struct scenario_entry_p {
    static constexpr auto rule = kw_scenario >> dsl::p<ident_p> + dsl::p<block_p>;
    static constexpr auto value = lexy::callback<ScenarioEntry>(
        [](Ident n, Block b) { return ScenarioEntry{std::move(n), std::move(b)}; });
};

struct network_entry_p {
    static constexpr auto name = "network entry";
    static constexpr auto rule =
        dsl::position + (dsl::p<options_entry_p> | dsl::p<validator_entry_p> | dsl::p<genesis_entry_p> |
                         dsl::p<bootstrap_entry_p> | dsl::p<scenario_entry_p> | dsl::p<let_p> | dsl::p<source_p> |
                         dsl::p<keypair_p> | dsl::error<expected_network_entry>);
    static constexpr auto value = lexy::callback<NetworkEntry>(
        [](Pos p, auto&& n) { return NetworkEntry{Loc{p, 0}, std::move(n)}; });
};
struct network_entries_p {
    static constexpr auto rule = dsl::curly_bracketed.opt_list(dsl::p<network_entry_p>);
    static constexpr auto value = lexy::as_list<std::vector<NetworkEntry>>;
};
struct network_p {
    static constexpr auto rule = kw_network >> dsl::p<ident_p> + dsl::p<network_entries_p>;
    static constexpr auto value = lexy::callback<NetworkDecl>(
        [](Ident n, std::vector<NetworkEntry> es) { return NetworkDecl{std::move(n), std::move(es)}; });
};

struct decl_p {
    static constexpr auto name = "declaration";
    static constexpr auto rule =
        dsl::position + (dsl::p<import_p> | dsl::p<let_p> | dsl::p<source_p> | dsl::p<keypair_p> |
                         dsl::p<schema_p> | dsl::p<fn_p> | dsl::p<network_p> | dsl::error<expected_declaration>);
    static constexpr auto value =
        lexy::callback<Decl>([](Pos p, auto&& n) { return Decl{Loc{p, 0}, std::move(n)}; });
};

struct document_p {
    static constexpr auto name = "document";
    static constexpr auto whitespace = ws;
    static constexpr auto rule = dsl::terminator(dsl::eof).opt_list(dsl::p<decl_p>);
    static constexpr auto value = lexy::as_list<std::vector<Decl>>;
};

// ------------------------------------------------------ error reporting --

std::string quote(std::string_view s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + "'";
}

struct ErrorSink {
    Diagnostics* diags;
    const char** last_pos;  // suppresses cascades reported at the same position

    template <typename Context, typename Reader, typename Tag>
    void operator()(const Context& ctx, const lexy::error<Reader, Tag>& e) const {
        const auto& input = ctx.input();
        const char* begin = input.data();
        const char* end = begin + input.size();
        std::string msg;
        Loc loc{e.position(), 1};
        if constexpr (std::is_same_v<Tag, lexy::expected_literal>) {
            std::string_view lit(e.string(), e.length());
            msg = "expected " + quote(lit);
            // A missing terminator belongs to the end of the previous token.
            if (lit == ";" || lit == ")" || lit == "]" || lit == "}") {
                const char* p = e.position();
                while (p > begin && std::isspace(static_cast<unsigned char>(p[-1]))) --p;
                if (p != e.position()) loc = Loc{p, 1};
            }
        } else if constexpr (std::is_same_v<Tag, lexy::expected_keyword>) {
            msg = "expected keyword " + quote(std::string_view(e.string(), e.length()));
        } else if constexpr (std::is_same_v<Tag, lexy::expected_char_class>) {
            std::string_view name = e.name();
            if (name.find("alpha") != std::string_view::npos) msg = "expected an identifier";
            else if (name.find("digit") != std::string_view::npos) msg = "expected a digit";
            else msg = std::string("expected ") + e.name();
        } else {
            msg = e.message();
            if (e.end() > e.begin()) loc = Loc{e.begin(), static_cast<std::uint32_t>(e.end() - e.begin())};
            if (msg == "reserved identifier")
                msg = quote(std::string_view(e.begin(), static_cast<std::size_t>(e.end() - e.begin()))) +
                      " is a reserved keyword and cannot be used as an identifier";
            else if (msg == "missing delimiter")
                msg = "unterminated string literal";
        }
        // Errors at or before the previous one are recovery cascades.
        if (*last_pos && loc.ptr <= *last_pos) return;
        *last_pos = loc.ptr;
        if (loc.ptr >= end) msg += " but reached end of file";
        // Only explicitly named productions are worth mentioning.
        std::string_view prod = ctx.production();
        if (prod.find("::") == std::string_view::npos && msg.find(prod) == std::string::npos && prod != "document" && prod != "declaration" &&
            prod != "expression" && prod != "identifier" && prod != "statement")
            msg += " in " + std::string(prod);
        diags->error(loc, std::move(msg));
    }
};

}  // namespace

bool is_reserved_keyword(std::string_view word) {
    for (const char* r : reserved_words)
        if (word == r) return true;
    return false;
}

std::optional<ast::Document> parse_document(const SourceFile& file, Diagnostics& diags) {
    auto input = lexy::string_input<lexy::utf8_char_encoding>(file.text.data(), file.text.size());
    std::size_t before = diags.error_count();
    const char* last_pos = nullptr;
    auto result = lexy::parse<document_p>(input, lexy::collect(lexy::callback<void>(ErrorSink{&diags, &last_pos})));
    if (!result.has_value() || diags.error_count() != before) {
        if (diags.error_count() == before) diags.error(Loc{}, "failed to parse " + file.path.string());
        return std::nullopt;
    }
    ast::Document doc;
    doc.file = &file;
    doc.decls = std::move(result).value();
    return doc;
}

}  // namespace tide
