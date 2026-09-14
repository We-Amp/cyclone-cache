#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# CI license audit (BLOCKING): Apache RAT over the whole tree.
#
# Fetches the pinned Apache RAT jar (or reuses $RAT_JAR), verifies its SHA-1
# before running it, scans the repository root with hidden directories
# included and .rat-excludes applied (that file also excludes .git: RAT 0.16
# honours either -e flags or the -E file, never both), and fails unless the
# report summary says exactly "0 Unknown Licenses". RAT itself exits 0 whatever
# it finds, so the verdict is read from the report; the unapproved-file list is
# printed on failure. RAT's --addLicense mode is deliberately not used: headers
# are added by hand, exclusions are declared with a reason in .rat-excludes.
#
# Run locally (needs Java 8 or newer and curl):
#   bash tools/ci/rat.sh
#
# Environment:
#   RAT_JAR       an already-downloaded apache-rat-0.16.1.jar (still SHA-1 checked)
#   RAT_WORK_DIR  where the jar and rat-report.txt are kept; defaults to
#                 $RUNNER_TEMP or a fresh mktemp directory. Keep it OUTSIDE the
#                 repository so the audit never scans its own output.

set -euo pipefail

RAT_VERSION="0.16.1"
RAT_SHA1="7a35d6881c9430c51ecb346bae662ee9832fe59a"
RAT_URL="https://repo1.maven.org/maven2/org/apache/rat/apache-rat/${RAT_VERSION}/apache-rat-${RAT_VERSION}.jar"

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
work_dir="${RAT_WORK_DIR:-${RUNNER_TEMP:-$(mktemp -d)}}"
mkdir -p "$work_dir"
jar="${RAT_JAR:-$work_dir/apache-rat-${RAT_VERSION}.jar}"
report="$work_dir/rat-report.txt"

sha1_of() {
  if command -v sha1sum >/dev/null 2>&1; then
    sha1sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 1 "$1" | awk '{print $1}'
  else
    echo "rat.sh: neither sha1sum nor shasum found; cannot verify the RAT jar" >&2
    exit 2
  fi
}

# A download is fetched to a .part file and only takes the final name once its
# SHA-1 matches, so a truncated or tampered fetch can never leave a permanently
# failing cached jar behind. A caller-provided $RAT_JAR is checked in place and
# left alone.
downloaded=0
candidate="$jar"
if [ ! -f "$jar" ]; then
  echo "Downloading Apache RAT ${RAT_VERSION} from ${RAT_URL}"
  candidate="$jar.part"
  curl -fsSL --retry 3 --retry-delay 2 -o "$candidate" "$RAT_URL"
  downloaded=1
fi

actual="$(sha1_of "$candidate")"
if [ "$actual" != "$RAT_SHA1" ]; then
  echo "rat.sh: SHA-1 mismatch for $candidate" >&2
  echo "  expected $RAT_SHA1" >&2
  echo "  actual   $actual" >&2
  if [ "$downloaded" -eq 1 ]; then
    rm -f "$candidate"
  fi
  exit 1
fi
if [ "$downloaded" -eq 1 ]; then
  mv "$candidate" "$jar"
fi
echo "Apache RAT ${RAT_VERSION} jar verified (sha1 ${RAT_SHA1})"

cd "$repo_root"
rm -f "$report"
java -jar "$jar" --dir . --scan-hidden-directories -E .rat-excludes -o "$report"

if [ ! -s "$report" ]; then
  echo "rat.sh: RAT produced no report at $report" >&2
  exit 1
fi

echo
echo "=== Apache RAT summary ($report) ==="
awk '{print} /Unknown Licenses/{exit}' "$report"

unknown="$(grep -E -o '^[[:space:]]*[0-9]+ Unknown Licenses' "$report" | grep -E -o '[0-9]+' | head -n1 || true)"
if [ -z "$unknown" ]; then
  echo "rat.sh: could not find the 'Unknown Licenses' line in the report; failing closed" >&2
  exit 1
fi

if [ "$unknown" -ne 0 ]; then
  echo
  echo "=== $unknown file(s) with unapproved licenses ==="
  awk '/^Files with unapproved licenses:/{p=1;next} /^\*\*\*\*/{if(p)exit} p' "$report"
  echo
  echo "Every file must carry the Apache-2.0 SPDX header, or be listed with a reason in .rat-excludes." >&2
  exit 1
fi

echo "License audit passed: 0 Unknown Licenses."
