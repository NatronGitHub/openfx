#!/bin/bash
# Checks that a change to HostSupport's virtual functions also updates the Python bindings.
#
# The adapters in HostSupport/python/src override HostSupport's virtual functions and mark each
# override with `override`, so a changed or removed signature already breaks their build. A new
# virtual function with a default body doesn't: the adapter silently inherits it, and Python can't
# override it. So a change that adds or removes a line declaring a virtual function in
# HostSupport/include must also change HostSupport/python/src, or give the reason in a
# "Bindings:" line of the pull request description (e.g. "Bindings: not needed, a GUI hook").
# Not seen: removing an #ifdef that makes an existing virtual function visible (rare).
#
# Usage: check-bindings-sync.sh BASE [HEAD]  (default HEAD: HEAD)
# Environment: PR_BODY, the pull request description (may be empty).
set -euo pipefail

base=${1:?usage: $0 BASE [HEAD]}
head=${2:-HEAD}

# added or removed lines that declare a virtual function (not comments)
# (whitespace-only changes ignored; the diff runs on its own, so a bad ref fails the check.
# A trailing comment mentioning "virtual" on a non-virtual line also matches: give a reason then)
diff=$(git diff -w -U0 "$base" "$head" -- HostSupport/include)
changed=$(printf '%s\n' "$diff" | grep -E '^[+-][^+-]' | grep -E '\bvirtual\b' \
  | grep -vE '^[+-][[:space:]]*(//|/\*|\*)' || true)
if [ -z "$changed" ]; then
  echo "No virtual function of HostSupport/include changed."
  exit 0
fi
echo "Virtual functions changed in HostSupport/include:"
echo "$changed"

bindings=$(git diff --name-only "$base" "$head" -- HostSupport/python/src)
if [ -n "$bindings" ]; then
  echo "HostSupport/python/src changes too: the reviewers check that it matches."
  exit 0
fi
# a "Bindings:" line with a reason (optionally bold or a list item)
reason=$(printf '%s\n' "${PR_BODY:-}" | tr -d '\r' \
  | grep -E '^[[:space:]*_-]*Bindings:[[:space:]*_]*[^[:space:]*_]' | head -1 || true)
if [ -n "$reason" ]; then
  echo "The bindings are unchanged; the description says why: $reason"
  exit 0
fi
echo "::error::HostSupport's virtual functions changed, but the Python bindings (HostSupport/python/src) didn't." \
  "Update the adapters, or add a line \"Bindings: <why they don't need to change>\" to the pull request description."
exit 1
