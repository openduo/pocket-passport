#!/usr/bin/env bash
# Copyright 2026 openduo
# SPDX-License-Identifier: FSL-1.1-Apache-2.0
#
# Run tools/validate.sh on macOS without editing it. Two host-tool
# differences break the upstream gate there:
#   - /sbin/sha256sum (BSD) lacks --status: /sbin is dropped from PATH so the
#     GNU-compatible tool or the gate's fallback is used;
#   - Apple ld rejects -Wl,--gc-sections: the demo runtime tests are linked
#     with the equivalent -Wl,-dead_strip.
# Usage: tools/pocket/validate_macos.sh [--all|--static|--firmware]
set -euo pipefail

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ "$(uname -s)" != "Darwin" ]]; then
    exec "${repo}/tools/validate.sh" "$@"
fi
PATH="$(printf '%s' "${PATH}" | tr ':' '\n' | grep -vx '/sbin' | paste -sd: -)"
export PATH
gate="$(mktemp "${repo}/tools/.validate-macos.XXXXXX")"
trap 'rm -f "${gate}"' EXIT
sed 's/-Wl,--gc-sections/-Wl,-dead_strip/' "${repo}/tools/validate.sh" > "${gate}"
chmod +x "${gate}"
bash "${gate}" "$@"
