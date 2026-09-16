#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Public-tree hygiene gate (BLOCKING): scans the tracked tree for the
# patterns in .github/hygiene-rules.txt and fails on ANY hit, printing
# path:line for each match. This repository is public, so the tree must
# stay free of private-infrastructure and private-process vocabulary
# (internal decision-record references, the private CI runner-pool label
# scheme, pre-publication development repository slugs, internal work-item
# identifiers, private build-fleet topology words). The rules file
# documents each class and holds only public-safe patterns.
#
# Excluded from the scan: .git (git grep visits tracked files only, so it
# is never scanned) and the rules file itself (it necessarily contains
# every blocked pattern).
#
# Run locally:
#   bash tools/ci/check-public-hygiene.sh

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"

rules=".github/hygiene-rules.txt"
if [ ! -f "$rules" ]; then
  echo "::error::$rules not found" >&2
  exit 1
fi

excludes=(":(exclude)$rules")

status=0
lineno=0
while IFS=$'\t' read -r flags pattern || [ -n "$flags" ]; do
  lineno=$((lineno + 1))
  case "$flags" in
    ''|\#*) continue ;;
  esac
  case "$flags" in
    *E*) ;;
    *)
      echo "::error::$rules:$lineno: only extended-regex (E) rules are supported" >&2
      exit 1
      ;;
  esac
  if [ -z "$pattern" ]; then
    echo "::error::$rules:$lineno: rule has flags but no pattern" >&2
    exit 1
  fi
  grep_args=(-n -I -E)
  case "$flags" in
    *i*) grep_args+=(-i) ;;
  esac
  set +e
  out="$(git grep "${grep_args[@]}" -e "$pattern" -- . "${excludes[@]}")"
  rc=$?
  set -e
  if [ "$rc" -gt 1 ]; then
    echo "::error::$rules:$lineno: git grep failed (status $rc): $out" >&2
    exit 1
  fi
  if [ "$rc" -eq 0 ]; then
    echo "::error::hygiene rule $lineno matched ($pattern):" >&2
    echo "$out" >&2
    status=1
  fi
done < "$rules"

if [ "$status" -ne 0 ]; then
  echo "::error::public-tree hygiene gate FAILED -- see the matched paths above" >&2
  exit 1
fi

echo "Public-tree hygiene gate passed: no rule matched the tracked tree."
