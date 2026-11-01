// lexy-based parser: source text -> ast::Document.
#pragma once

#include <optional>

#include "ast.hpp"
#include "diag.hpp"

namespace tide {

// Parses one source file. Syntax errors are reported to `diags`; returns
// std::nullopt if any were found.
std::optional<ast::Document> parse_document(const SourceFile& file, Diagnostics& diags);

// Reserved words; not usable as identifiers.
bool is_reserved_keyword(std::string_view word);

}  // namespace tide
