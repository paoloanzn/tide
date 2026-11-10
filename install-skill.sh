#!/bin/sh
# Install the Tide Agent Skill and its documentation from one repository snapshot.
# No developer tools are installed and no project configuration is edited.
# Keep side effects inside main so a truncated piped download cannot start setup.
set -eu

# Maintainer-selected default: advance this after publishing skill/docs changes.
# Users only need TIDE_SKILL_COMMIT when explicitly choosing another snapshot.
PINNED_COMMIT='eb310a2b468ff591c5354a80ae2e63e3833546fc'

say() { printf 'tide-skill: %s\n' "$*" >&2; }
fail() { say "error: $*"; exit 1; }
has() { command -v "$1" >/dev/null 2>&1; }
valid_commit() {
    [ "${#1}" -eq 40 ] || return 1
    case "$1" in *[!0-9a-f]*) return 1 ;; esac
}

usage() {
    cat <<'EOF'
Usage: sh install-skill.sh [--agent NAME] [--skills-dir ABSOLUTE_PATH]

Default: shared ~/.agents/skills plus detected agents' personal skill directories.
Repeat --agent or --skills-dir to select destinations explicitly.
Agents: shared, codex, cursor, gemini, opencode, amp, claude, copilot, cline,
        kiro, antigravity, all.
--skills-dir selects a skills parent directory; the skill is installed as tide/.
The default source commit is maintained by Tide; no user configuration is needed.
TIDE_SKILL_COMMIT optionally overrides it (40 lowercase hex characters).
An absent/unavailable pin falls back to main, with a warning.
Run this installer again to update. Reload skills or restart your agent afterward.
EOF
}

fetch() {
    curl --proto '=https' --proto-redir '=https' -fsSL \
        --connect-timeout 10 --max-time 120 --retry 2 --retry-max-time 180 \
        -o "$2" "$1"
}

# Inspect names and types before extracting. Reject links, special files, path
# traversal, and ambiguous/escaped names; no downloaded code is executed.
snapshot() {
    fetch "https://github.com/paoloanzn/tide/archive/$1.tar.gz" "$tmp/source.tar.gz" || return 1
    tar -tzf "$tmp/source.tar.gz" > "$tmp/members" || return 1
    tar -tvzf "$tmp/source.tar.gz" > "$tmp/types" || return 1
    root=$(sed -n '1s,/.*,,p' "$tmp/members")
    case "$root" in tide-*) ;; *) return 1 ;; esac
    awk -v root="$root/" '
        $0 !~ /^[A-Za-z0-9_.+@\/-]+$/ { exit 1 }
        $0 ~ /(^|\/)\.\.?($|\/)/ { exit 1 }
        index($0, root) != 1 { exit 1 }
    ' "$tmp/members" || return 1
    awk 'substr($0, 1, 1) != "-" && substr($0, 1, 1) != "d" { exit 1 }' "$tmp/types" || return 1
    rm -rf "$tmp/unpacked"
    mkdir "$tmp/unpacked" || return 1
    tar -xzf "$tmp/source.tar.gz" --no-same-owner --no-same-permissions \
        -C "$tmp/unpacked" -- "$root/skills/tide" "$root/docs" \
        "$root/README.md" "$root/LICENSE" "$root/NOTICE" "$root/CMakeLists.txt" || return 1
    source_dir="$tmp/unpacked/$root"
    for required in skills/tide/SKILL.md docs/README.md docs/getting-started/tutorial.md \
        docs/reference/cli.md docs/backends/solana-test-validator.md README.md LICENSE NOTICE; do
        [ -s "$source_dir/$required" ] || return 1
    done
    grep -qx 'name: tide' "$source_dir/skills/tide/SKILL.md" || return 1
    tide_version=$(sed -n 's/^project(tide VERSION \([0-9][0-9.]*\).*/\1/p' "$source_dir/CMakeLists.txt")
    [ -n "$tide_version" ] || return 1
}

add_target() {
    case "$1" in /*) ;; *) fail "skills directories must be absolute: $1" ;; esac
    case "$1" in *'
'*) fail 'newlines in installation paths are unsupported' ;; esac
    printf '%s/tide\n' "${1%/}" >> "$tmp/targets"
}

agent_target() {
    case "$1" in
        shared | codex | cursor | gemini | opencode | amp) add_target "$HOME/.agents/skills" ;;
        claude) add_target "${CLAUDE_CONFIG_DIR:-$HOME/.claude}/skills" ;;
        copilot) add_target "$HOME/.copilot/skills" ;;
        cline) add_target "$HOME/.cline/skills" ;;
        kiro) add_target "$HOME/.kiro/skills" ;;
        antigravity) add_target "$HOME/.gemini/config/skills" ;;
        all)
            for agent in shared claude copilot cline kiro antigravity; do agent_target "$agent"; done
            ;;
        *) fail "unknown agent '$1'; use --help for supported names" ;;
    esac
}

cleanup() {
    # Every destination has its own backup on the same filesystem. If any
    # activation fails, restore all destinations changed during this invocation.
    while IFS= read -r target; do
        lock="${target%/tide}/.tide-install.lock"
        if [ "$activated" = 0 ]; then
            if [ -d "$lock/old" ]; then
                rm -rf "$target"
                if ! mv "$rename_flags" "$lock/old" "$target"; then
                    say "could not restore $target; recover the backup at $lock/old"
                    continue
                fi
            elif [ -f "$lock/created" ]; then
                rm -rf "$target"
            fi
        fi
        rm -rf "$lock"
    done < "$tmp/locked"
    rm -rf "$tmp"
}

main() {
    [ "${1:-}" != --help ] || { usage; return; }
    for tool in curl tar mktemp diff; do has "$tool" || fail "need $tool"; done
    case "$(uname -s)" in
        Linux) rename_flags=-fT ;;
        Darwin) rename_flags=-fh ;;
        *) fail 'this installer supports Linux (including WSL) and macOS' ;;
    esac
    : "${HOME:?HOME is not set}"
    umask 077
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/tide-skill.XXXXXXXX")
    activated=0
    : > "$tmp/locked"
    : > "$tmp/targets"
    trap cleanup 0
    trap 'exit 130' INT
    trap 'exit 143' TERM
    trap 'exit 129' HUP
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --agent | --skills-dir)
                [ "$#" -ge 2 ] || fail "$1 needs a value"
                if [ "$1" = --agent ]; then agent_target "$2"; else add_target "$2"; fi
                shift 2
                ;;
            *) fail "unknown option '$1'; use --help" ;;
        esac
    done
    if [ ! -s "$tmp/targets" ]; then
        agent_target shared
        if [ -d "${CLAUDE_CONFIG_DIR:-$HOME/.claude}" ] || has claude; then agent_target claude; fi
        if [ -d "$HOME/.copilot" ] || has copilot; then agent_target copilot; fi
        if [ -d "$HOME/.cline" ] || has cline; then agent_target cline; fi
        if [ -d "$HOME/.kiro" ] || has kiro; then agent_target kiro; fi
        if [ -d "$HOME/.gemini/config" ] || has agy || has antigravity; then agent_target antigravity; fi
    fi
    awk '!seen[$0]++' "$tmp/targets" > "$tmp/unique-targets"

    pin="${TIDE_SKILL_COMMIT-$PINNED_COMMIT}"
    [ -z "$pin" ] || valid_commit "$pin" || fail 'the commit pin must be 40 lowercase hexadecimal characters'
    fallback=false
    if [ -n "$pin" ] && snapshot "$pin"; then
        resolved="$pin"
    else
        fallback=true
        say "warning: pin '${pin:-absent}' is unavailable or incomplete; falling back to main for both skill and docs"
        resolved='main (unresolved)'
        ref=refs/heads/main
        if curl --proto '=https' --proto-redir '=https' -fsSL \
            --connect-timeout 10 --max-time 20 --retry 1 --retry-max-time 30 \
            -H 'Accept: application/vnd.github.sha' \
            https://api.github.com/repos/paoloanzn/tide/commits/main -o "$tmp/main-sha"; then
            main_sha=$(cat "$tmp/main-sha")
            if valid_commit "$main_sha"; then resolved="$main_sha"; ref="$main_sha"; fi
        fi
        snapshot "$ref" || fail 'could not download a complete skill/docs snapshot; existing installation preserved'
    fi

    package="$tmp/package"
    mkdir "$package"
    cp -R "$source_dir/skills/tide" "$package/tide"
    mkdir -p "$package/tide/references"
    rm -rf "$package/tide/references/docs"
    cp -R "$source_dir/docs" "$package/tide/references/docs"
    cp "$source_dir/README.md" "$source_dir/LICENSE" "$source_dir/NOTICE" "$package/tide/references/"
    printf 'repository=paoloanzn/tide\ncommit=%s\nrequested_commit=%s\nfallback=%s\ntide_version=%s\n' \
        "$resolved" "$pin" "$fallback" "$tide_version" > "$package/tide/SOURCE"

    # Real directories are intentional: not every skill client follows symlinks.
    # The marker distinguishes our replaceable bundles from unrelated skills.
    marker='paoloanzn/tide skill installer v1'
    printf '%s\n' "$marker" > "$package/tide/.tide-managed"
    while IFS= read -r target; do
        mkdir -p "${target%/tide}"
        lock="${target%/tide}/.tide-install.lock"
        mkdir "$lock" 2>/dev/null || fail "another install may be running; lock: $lock"
        printf '%s\n' "$target" >> "$tmp/locked"
        [ ! -L "$target" ] || fail "refusing symlinked skill: $target"
        if [ -e "$target" ]; then
            if [ ! -d "$target" ] || [ -L "$target/.tide-managed" ] ||
                [ "$(cat "$target/.tide-managed" 2>/dev/null || true)" != "$marker" ]; then
                fail "unrelated skill already exists: $target"
            fi
        fi
    done < "$tmp/unique-targets"

    # Prepare every destination before moving any installed content.
    changed=0
    while IFS= read -r target; do
        lock="${target%/tide}/.tide-install.lock"
        if [ ! -d "$target" ] || ! diff -r "$target/" "$package/tide/" >/dev/null 2>&1; then
            cp -R "$package/tide" "$lock/new"
            changed=1
        fi
    done < "$tmp/unique-targets"
    while IFS= read -r target; do
        lock="${target%/tide}/.tide-install.lock"
        if [ -d "$lock/new" ]; then
            if [ -d "$target" ]; then
                mv "$rename_flags" "$target" "$lock/old"
            else
                : > "$lock/created"
            fi
            mv "$rename_flags" "$lock/new" "$target"
        fi
    done < "$tmp/unique-targets"
    activated=1
    if [ "$changed" = 1 ]; then say "installed skill and docs from $resolved"; else say 'skill and docs are already up to date'; fi
    while IFS= read -r target; do say "available at $target"; done < "$tmp/unique-targets"
    say 'reload skills or restart your agent, then ask: Use the Tide skill to set up this project.'
}

main "$@"
