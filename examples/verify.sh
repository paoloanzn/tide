#!/usr/bin/env bash
# Run from anywhere: bash examples/verify.sh [--e2e]
# --e2e requires the pinned upstream sources and compiled binaries from each README.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

command -v tide >/dev/null || { echo "tide is not on PATH" >&2; exit 1; }

for name in 01-simple-pda 02-subscriptions; do
  echo "==> Checking $name"
  tide check "examples/$name/network.tid"
done

if [[ "${1:-}" != "--e2e" ]]; then
  echo "Static checks complete. Use --e2e after building upstream programs."
  exit 0
fi

command -v solana-test-validator >/dev/null || {
  echo "solana-test-validator is required for --e2e" >&2
  exit 1
}

for name in 01-simple-pda 02-subscriptions; do
  script="examples/$name/network.tid"
  echo "==> Running $script"
  cleanup() { tide down "$script" >/dev/null 2>&1 || true; }
  trap cleanup EXIT INT TERM
  tide up "$script"
  tide down "$script"
  trap - EXIT INT TERM
done

echo "All selected examples completed without a Tide error."
