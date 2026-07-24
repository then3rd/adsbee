#!/usr/bin/env bash
# Cut a fork release by creating an annotated tag. See FORK_NOTES.md for the full workflow and rationale.
#
# Why this exists: the fork branch (GC9A01-radar) is rebased onto upstream and force-pushed, which
# rewrites the branch tip. Tags are separate, immutable refs — a force-push never moves or deletes
# them — so a tag is the durable, checkout-able snapshot of "what I shipped as release N".
#
# What it does:
#   1. Normalizes the version you pass into the `gc9a01-vX.Y.Z` tag name (fork-owned namespace;
#      never collides with upstream's `adsbee_1090-*` tags).
#   2. Refuses to run on a dirty tree or a tag that already exists.
#   3. Shows the release delta (the fork's commit stack on top of upstream).
#   4. Creates an ANNOTATED tag at HEAD (carries author/date/message).
#
# What it deliberately does NOT do: push. Review the tag first, then publish with the printed
# `git push origin <tag>` command (a plain push — tags are additive, no --force needed).
#
# Usage: bash firmware/scripts/tag_release.sh <version>    e.g. 1.0.0  or  v1.0.0  or  gc9a01-v1.0.0
set -uo pipefail

TAG_PREFIX="gc9a01-v"
RELEASE_BRANCH="GC9A01-radar"

if [ $# -lt 1 ] || [ -z "${1:-}" ]; then
    echo "usage: bash firmware/scripts/tag_release.sh <version>   (e.g. 1.0.0)" >&2
    exit 1
fi

# Must be inside the repo.
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "error: not inside a git repository" >&2
    exit 1
fi
cd "$(git rev-parse --show-toplevel)" || exit 1

# Normalize the version into the gc9a01-vX.Y.Z tag name. Accept 1.0.0, v1.0.0, or the full tag.
VERSION="$1"
VERSION="${VERSION#gc9a01-}"   # strip a leading gc9a01- if the full tag was passed
VERSION="${VERSION#v}"          # strip a leading v
TAG="${TAG_PREFIX}${VERSION}"

# Refuse to overwrite an existing tag — releases are immutable.
if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    echo "error: tag '$TAG' already exists. Releases are immutable; pick a new version." >&2
    echo "  (to inspect it:  git show $TAG)" >&2
    exit 1
fi

# Refuse to tag a dirty tree — the tag should point at a clean, reproducible state.
if ! git diff-index --quiet HEAD -- 2>/dev/null || [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "error: working tree has uncommitted changes. Commit or stash them first." >&2
    git status --short
    exit 1
fi

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if [ "$BRANCH" != "$RELEASE_BRANCH" ]; then
    echo "warning: on branch '$BRANCH', not '$RELEASE_BRANCH'. Tagging HEAD anyway." >&2
    echo
fi

echo "==> Release delta (fork commits on top of upstream/main):"
if git rev-parse -q --verify upstream/main >/dev/null; then
    git log --oneline upstream/main..HEAD | head -50
else
    echo "  (no 'upstream/main' ref — run 'git fetch upstream' for an accurate delta)"
    git log --oneline -20
fi

echo
echo "==> Creating annotated tag '$TAG' at $(git rev-parse --short HEAD)..."
if git tag -a "$TAG" -m "Release $TAG"; then
    echo
    echo "Tag created. Review it, then publish (this triggers the release CI):"
    echo "    git show $TAG"
    echo "    git push origin $TAG"
    echo
    echo "Note: this is a plain push, NOT a force-push. The tag is unaffected by the branch's"
    echo "rebase/force-push cycle and will remain checkout-able (git checkout $TAG) forever."
else
    echo "error: failed to create tag '$TAG'." >&2
    exit 1
fi
