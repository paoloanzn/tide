#!/bin/sh
# Installs or updates Tide from its official GitHub releases.
#
#   curl -fsSL https://raw.githubusercontent.com/paoloanzn/tide/main/install.sh | sh
#
# Running it again updates Tide to the latest release. Environment variables:
#
#   TIDE_VERSION       release to install, e.g. v0.1.0 (default: the latest release)
#   TIDE_INSTALL_DIR   directory for the tide binary (default: $HOME/.local/bin)
#   TIDE_FORCE=1       reinstall even when that version is already installed
#   TIDE_RELEASES_URL  where releases are downloaded from (default: GitHub; for mirrors and testing)
#
# Everything lives in functions and runs from the last line, so a download cut
# short by the network executes nothing.

set -eu

releases_url="${TIDE_RELEASES_URL:-https://github.com/paoloanzn/tide/releases}"
install_dir="${TIDE_INSTALL_DIR:-${HOME:?HOME is not set}/.local/bin}"

say() { printf 'tide-install: %s\n' "$*" >&2; }
fail() {
    say "error: $*"
    exit 1
}
has() { command -v "$1" >/dev/null 2>&1; }

# fetch <url> <file>
fetch() {
    if has curl; then
        curl -fsSL --retry 3 -o "$2" "$1"
    else
        wget -q -O "$2" "$1"
    fi
}

# The tag GitHub's "latest release" link redirects to. It skips pre-releases
# and needs neither an API token nor a JSON parser.
latest_tag() {
    if has curl; then
        url=$(curl -fsSIL --retry 3 -o /dev/null -w '%{url_effective}' "$releases_url/latest") || return 1
    else
        url=$(wget -q -S --spider "$releases_url/latest" 2>&1 | sed -n 's/^ *[Ll]ocation: *//p' | tail -n 1 | tr -d '\r')
    fi
    case "$url" in
        */releases/tag/*) printf '%s\n' "${url##*/releases/tag/}" ;;
        *) return 1 ;;
    esac
}

# The release asset for this machine: linux-x86_64, linux-arm64 or macos-arm64.
detect_target() {
    os=$(uname -s)
    arch=$(uname -m)
    case "$arch" in
        x86_64 | amd64) arch=x86_64 ;;
        aarch64 | arm64) arch=arm64 ;;
        *) fail "no prebuilt Tide for $arch CPUs; build it from source: https://github.com/paoloanzn/tide#installation" ;;
    esac
    case "$os" in
        Linux)
            if ldd --version 2>&1 | grep -qi musl; then
                fail "the prebuilt Tide needs glibc; on musl systems (such as Alpine) build it from source"
            fi
            printf 'linux-%s\n' "$arch"
            ;;
        Darwin)
            # A shell under Rosetta reports x86_64 on Apple silicon.
            if [ "$arch" = x86_64 ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" = 1 ]; then
                arch=arm64
            fi
            [ "$arch" = arm64 ] || fail "no prebuilt Tide for Intel Macs; build it from source: https://github.com/paoloanzn/tide#installation"
            printf 'macos-arm64\n'
            ;;
        *) fail "no prebuilt Tide for $os; build it from source: https://github.com/paoloanzn/tide#installation" ;;
    esac
}

sha256_of() {
    if has sha256sum; then
        sha256sum "$1" | cut -d ' ' -f 1
    elif has shasum; then
        shasum -a 256 "$1" | cut -d ' ' -f 1
    else
        fail "need sha256sum or shasum to verify the download"
    fi
}

main() {
    has curl || has wget || fail "need curl or wget"
    has tar || fail "need tar"
    target=$(detect_target)

    if [ -n "${TIDE_VERSION:-}" ]; then
        tag="v${TIDE_VERSION#v}"
    else
        tag=$(latest_tag) || fail "could not find the latest Tide release at $releases_url"
    fi
    version="${tag#v}"

    current=""
    if [ -x "$install_dir/tide" ]; then
        current=$("$install_dir/tide" --version 2>/dev/null | sed -n 's/^tide //p') || current=""
    fi
    if [ "$current" = "$version" ] && [ "${TIDE_FORCE:-0}" != 1 ]; then
        say "tide $version is already installed in $install_dir; set TIDE_FORCE=1 to reinstall"
        exit 0
    fi

    tmp=$(mktemp -d 2>/dev/null || mktemp -d -t tide-install)
    trap 'rm -rf "$tmp"' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM

    asset="tide-$target.tar.gz"
    say "downloading tide $version for $target"
    fetch "$releases_url/download/$tag/$asset" "$tmp/$asset" ||
        fail "could not download $asset for $tag; is $tag a published release?"
    fetch "$releases_url/download/$tag/$asset.sha256" "$tmp/$asset.sha256" ||
        fail "could not download the checksum for $asset"

    expected=$(cut -d ' ' -f 1 <"$tmp/$asset.sha256")
    actual=$(sha256_of "$tmp/$asset")
    if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
        fail "checksum mismatch for $asset (expected $expected, got $actual); nothing was installed"
    fi

    mkdir "$tmp/unpacked"
    tar -xzf "$tmp/$asset" -C "$tmp/unpacked"
    [ -f "$tmp/unpacked/tide" ] || fail "$asset does not contain a tide binary"
    "$tmp/unpacked/tide" --version >/dev/null 2>&1 || fail "the downloaded tide does not run on this machine"

    # Copy next to the destination, then rename over it: the swap is atomic,
    # so an interrupted update never leaves a half-written binary behind.
    mkdir -p "$install_dir"
    cp "$tmp/unpacked/tide" "$install_dir/.tide.new.$$"
    chmod 755 "$install_dir/.tide.new.$$"
    mv -f "$install_dir/.tide.new.$$" "$install_dir/tide"

    if [ "$current" = "$version" ]; then
        say "reinstalled tide $version in $install_dir"
    elif [ -n "$current" ]; then
        say "updated tide $current -> $version in $install_dir"
    else
        say "installed tide $version in $install_dir"
    fi

    case ":${PATH:-}:" in
        *":$install_dir:"*)
            found=$(command -v tide 2>/dev/null || true)
            if [ -n "$found" ] && [ "$found" != "$install_dir/tide" ]; then
                say "warning: $found comes first on PATH and shadows the tide just installed"
            fi
            ;;
        *) say "add $install_dir to your PATH to run tide, e.g.: export PATH=\"$install_dir:\$PATH\"" ;;
    esac
    if ! has solana-test-validator; then
        say "note: 'tide up' needs solana-test-validator from the Solana CLI: https://docs.anza.xyz/cli/install"
    fi
}

main "$@"
