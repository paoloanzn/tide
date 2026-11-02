// Source management and positioned diagnostics shared by every pipeline stage,
// plus a couple of tiny utilities every stage uses.
//
// A `Loc` is a raw pointer into a buffer owned by the `SourceManager`. This
// keeps AST nodes small and lets any stage (parser, checker, interpreter,
// backend) point at source text without knowing which file it came from.
#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tide {

// Tide's `int` is a signed 128-bit integer. `__int128` is a GCC/Clang
// extension; `__extension__` keeps -Wpedantic quiet about it.
__extension__ using i128 = __int128;

struct Loc {
    const char* ptr = nullptr;
    std::uint32_t len = 0;  // 0 = point location
    explicit operator bool() const { return ptr != nullptr; }
};

struct SourceFile {
    std::filesystem::path path;
    std::string text;
};

struct LineCol {
    const SourceFile* file = nullptr;
    std::uint32_t line = 0, col = 0;  // 1-based
    std::string_view line_text;
};

class SourceManager {
public:
    // Returns nullptr when the file cannot be read.
    const SourceFile* load(const std::filesystem::path& path) {
        const std::ifstream in(path, std::ios::binary);
        if (!in) return nullptr;
        std::ostringstream ss;
        ss << in.rdbuf();
        return add(path, ss.str());
    }

    const SourceFile* add(std::filesystem::path path, std::string text) {
        files_.push_back(std::make_unique<SourceFile>(SourceFile{std::move(path), std::move(text)}));
        return files_.back().get();
    }

    LineCol locate(const char* p) const {
        // std::less_equal is a total order over pointers into unrelated buffers, unlike raw `<=`.
        const std::less_equal<> le;
        for (const auto& f : files_) {
            const std::string_view text = f->text;
            if (!le(text.data(), p) || !le(p, std::to_address(text.end()))) continue;
            const std::string_view before = text.substr(0, static_cast<std::size_t>(p - text.data()));
            const std::size_t last_newline = before.rfind('\n');
            const std::size_t line_start = last_newline == std::string_view::npos ? 0 : last_newline + 1;
            const std::size_t line_end = std::min(text.find('\n', line_start), text.size());
            LineCol lc{f.get(), 1, 1, {}};
            lc.line += static_cast<std::uint32_t>(std::ranges::count(before, '\n'));
            lc.col = static_cast<std::uint32_t>(before.size() - line_start) + 1;
            lc.line_text = text.substr(line_start, line_end - line_start);
            return lc;
        }
        return {};
    }

private:
    std::vector<std::unique_ptr<SourceFile>> files_;
};

enum class Severity : std::uint8_t { error, warning, note };

struct Diagnostic {
    Severity severity = Severity::error;
    Loc loc;
    std::string message;
    std::vector<std::pair<Loc, std::string>> notes;
};

class Diagnostics {
public:
    explicit Diagnostics(const SourceManager& sm) : sm_(&sm) {}

    Diagnostic& error(Loc loc, std::string msg) { return add(Severity::error, loc, std::move(msg)); }
    Diagnostic& warning(Loc loc, std::string msg) { return add(Severity::warning, loc, std::move(msg)); }

    Diagnostic& add(Severity s, Loc loc, std::string msg) {
        if (s == Severity::error) ++errors_;
        if (s == Severity::warning) ++warnings_;
        list_.push_back({s, loc, std::move(msg), {}});
        return list_.back();
    }

    [[nodiscard]] bool has_errors() const { return errors_ > 0; }
    [[nodiscard]] std::size_t error_count() const { return errors_; }
    [[nodiscard]] std::size_t warning_count() const { return warnings_; }
    [[nodiscard]] const std::vector<Diagnostic>& all() const { return list_; }
    void clear() {
        list_.clear();
        errors_ = warnings_ = 0;
    }

    // Renders `file:line:col: severity: message`, the source line and a caret.
    [[nodiscard]] std::string format(const Diagnostic& d, bool color = false) const {
        std::string out = format_one(d.severity, d.loc, d.message, color);
        for (const auto& [loc, msg] : d.notes) out += format_one(Severity::note, loc, msg, color);
        return out;
    }

    void print(std::ostream& os, bool color = false) const {
        for (const auto& d : list_) os << format(d, color);
    }

    [[nodiscard]] const SourceManager& sources() const { return *sm_; }

private:
    static std::string_view severity_name(Severity s) {
        switch (s) {
            case Severity::error: return "error";
            case Severity::warning: return "warning";
            case Severity::note: break;
        }
        return "note";
    }

    static std::string_view severity_color(Severity s) {
        switch (s) {
            case Severity::error: return "\033[1;31m";
            case Severity::warning: return "\033[1;35m";
            case Severity::note: break;
        }
        return "\033[1;36m";
    }

    [[nodiscard]] std::string format_one(Severity s, Loc loc, const std::string& msg, bool color) const {
        const std::string_view bold = color ? "\033[1m" : "";
        const std::string_view reset = color ? "\033[0m" : "";
        const std::string_view sev_color = color ? severity_color(s) : "";
        const std::string_view sev_name = severity_name(s);
        std::string out;
        const LineCol lc = loc ? sm_->locate(loc.ptr) : LineCol{};
        out += bold;
        if (lc.file)
            out += lc.file->path.string() + ":" + std::to_string(lc.line) + ":" + std::to_string(lc.col) + ": ";
        else
            out += "tide: ";
        out += reset;
        out += sev_color;
        out += sev_name;
        out += ": ";
        out += reset;
        out += bold;
        out += msg;
        out += reset;
        out += "\n";
        if (lc.file) {
            const std::string gutter = std::to_string(lc.line);
            out += " " + gutter + " | " + std::string(lc.line_text) + "\n";
            out += " " + std::string(gutter.size(), ' ') + " | ";
            for (std::uint32_t i = 1; i < lc.col; ++i)
                out += (i - 1 < lc.line_text.size() && lc.line_text[i - 1] == '\t') ? '\t' : ' ';
            const std::size_t avail = lc.line_text.size() >= lc.col ? lc.line_text.size() - lc.col + 1 : 1;
            const std::size_t len = std::clamp<std::size_t>(loc.len, 1, std::max<std::size_t>(avail, 1));
            out += sev_color;
            out += "^" + std::string(len - 1, '~');
            out += reset;
            out += "\n";
        }
        return out;
    }

    const SourceManager* sm_;
    std::vector<Diagnostic> list_;
    std::size_t errors_ = 0, warnings_ = 0;
};

// Thrown by later stages (interpreter, backend) for errors that abort the
// lifecycle. Carries the source location of the construct responsible.
struct TideError : std::runtime_error {
    Loc loc;
    std::vector<std::pair<Loc, std::string>> notes;
    TideError(Loc l, const std::string& msg) : std::runtime_error(msg), loc(l) {}
};

// ------------------------------------------- small shared utilities --

// Visitor built from lambdas for std::visit.
template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

// Sets `ref` to `value` for the guard's lifetime and restores the previous
// value on scope exit, including when an exception propagates.
template <class T>
class Restore {
public:
    Restore(T& ref, T value) : ref_(ref), saved_(std::exchange(ref, std::move(value))) {}
    ~Restore() { ref_ = std::move(saved_); }
    Restore(const Restore&) = delete;
    Restore& operator=(const Restore&) = delete;
    Restore(Restore&&) = delete;
    Restore& operator=(Restore&&) = delete;

private:
    T& ref_;
    T saved_;
};

// Edit distance for "did you mean" suggestions.
inline std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> prev(b.size() + 1);
    std::vector<std::size_t> cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

template <typename Range>
std::string suggest(std::string_view name, const Range& candidates) {
    std::string best;
    std::size_t best_d = std::max<std::size_t>(2, name.size() / 3) + 1;
    for (const auto& c : candidates) {
        const std::size_t d = edit_distance(name, c);
        if (d < best_d) {
            best_d = d;
            best = std::string(c);
        }
    }
    return best.empty() ? std::string{} : "; did you mean '" + best + "'?";
}

}  // namespace tide
