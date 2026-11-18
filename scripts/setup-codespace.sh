#!/usr/bin/env bash
# Sets up a fresh GitHub Codespace for Tide development: installs the build
# tools, bootstraps vcpkg, and configures the `default` CMake preset so that
# build/debug/compile_commands.json exists for clangd (see .clangd).
# Safe to re-run: every step is skipped when already done.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
export VCPKG_ROOT

log() { printf '\n==> %s\n' "$*"; }

log "Installing system packages"
# Ninja is the preset's generator; the rest is what vcpkg needs to build ports.
sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  ninja-build pkg-config zip unzip tar curl git autoconf automake libtool linux-libc-dev

log "Bootstrapping vcpkg in $VCPKG_ROOT"
if [[ ! -d "$VCPKG_ROOT/.git" ]]; then
  git clone -q https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
fi
if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
  "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi
# Persist VCPKG_ROOT so later shells (and `cmake --preset`) find it.
if ! grep -q 'export VCPKG_ROOT=' "$HOME/.bashrc"; then
  echo "export VCPKG_ROOT=\"$VCPKG_ROOT\"" >> "$HOME/.bashrc"
fi

log "Disabling the Microsoft C/C++ IntelliSense engine (clangd replaces it)"
# The cpptools extension ships with Codespaces and reports its own, wrong
# diagnostics next to clangd's. The setting lives in the codespace's machine settings.
MACHINE_SETTINGS="$HOME/.vscode-remote/data/Machine/settings.json"
mkdir -p "$(dirname "$MACHINE_SETTINGS")"
python3 - "$MACHINE_SETTINGS" <<'PY'
import json, os, sys
path = sys.argv[1]
settings = json.load(open(path)) if os.path.exists(path) and os.path.getsize(path) else {}
settings["C_Cpp.intelliSenseEngine"] = "disabled"
json.dump(settings, open(path, "w"), indent="\t")
PY

log "Configuring CMake (first run builds all vcpkg dependencies: slow)"
cd "$REPO_ROOT"
cmake --preset default

log "Done: build/debug/compile_commands.json is ready."
echo "In VS Code run 'clangd: Restart language server' to pick it up."
echo "Open a new terminal (or 'source ~/.bashrc') to get VCPKG_ROOT in your shell."
