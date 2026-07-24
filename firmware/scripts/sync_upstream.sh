#!/usr/bin/env bash
# Sync this fork onto upstream via rebase. See FORK_NOTES.md for the full workflow and rationale.
#
# What it does:
#   1. Verifies the `upstream` remote exists (adds nothing — just tells you how if missing).
#   2. Fetches upstream.
#   3. Shows the incoming commits (what upstream/main has that your branch doesn't).
#   4. Rebases the current branch onto upstream/main.
#
# What it deliberately does NOT do: push. Rebasing rewrites the branch tip, so after a clean rebase
# you push with `git push --force-with-lease origin <branch>` yourself (see the printed hint).
#
# Usage: bash firmware/scripts/sync_upstream.sh [upstream-ref]   (default ref: upstream/main)
set -uo pipefail

UPSTREAM_REMOTE="upstream"
UPSTREAM_REF="${1:-upstream/main}"

# Must be inside the repo.
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "error: not inside a git repository" >&2
    exit 1
fi
cd "$(git rev-parse --show-toplevel)" || exit 1

# The upstream remote must be configured.
if ! git remote get-url "$UPSTREAM_REMOTE" >/dev/null 2>&1; then
    echo "error: no '$UPSTREAM_REMOTE' remote configured." >&2
    echo "  add it with:" >&2
    echo "    git remote add $UPSTREAM_REMOTE https://github.com/CoolNamesAllTaken/adsbee.git" >&2
    exit 1
fi

# Refuse to rebase on a dirty tree — a rebase would otherwise fail partway or stash silently.
if ! git diff-index --quiet HEAD -- 2>/dev/null || [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "error: working tree has uncommitted changes. Commit or stash them first." >&2
    git status --short
    exit 1
fi

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
echo "==> Fetching '$UPSTREAM_REMOTE'..."
git fetch "$UPSTREAM_REMOTE" || exit 1

echo
echo "==> Incoming commits ($UPSTREAM_REF not yet in '$BRANCH'):"
if ! git log --oneline "HEAD..$UPSTREAM_REF" 2>/dev/null | head -50; then
    echo "  (could not resolve $UPSTREAM_REF)"
    exit 1
fi
if [ -z "$(git log --oneline "HEAD..$UPSTREAM_REF" 2>/dev/null)" ]; then
    echo "  (none — already up to date with $UPSTREAM_REF)"
    echo
    echo "Nothing to rebase."
    exit 0
fi

echo
echo "==> Rebasing '$BRANCH' onto $UPSTREAM_REF..."
if git rebase "$UPSTREAM_REF"; then
    echo
    echo "Rebase complete. Review, then publish with:"
    echo "    git push --force-with-lease origin $BRANCH"
else
    echo
    echo "Rebase hit conflicts. Resolve them (see FORK_NOTES.md 'Expected conflict surface'), then:"
    echo "    git rebase --continue    # after fixing + git add"
    echo "    git rebase --abort       # to back out entirely"
    exit 1
fi
