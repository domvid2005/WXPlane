#!/usr/bin/env bash
# Releases WXPlane to SkunkCrafts Updater users: takes the plugin bundle that the GitHub build made for a commit on main, turns it
# into the updater's depot (tools/skunkcrafts_depot.py) and commits that to the repo's "release" branch, which the updater reads at
#   https://raw.githubusercontent.com/domvid2005/WXPlane/refs/heads/release/WXPlane/
#
#   tools/skunkcrafts_release.sh <version> [commit]            dry run: builds and checks the depot, changes nothing
#   tools/skunkcrafts_release.sh <version> [commit] --publish  ...and pushes it (users see the update in the updater app)
#   ... --bundle <dir>/WXPlane --run <id>                      use a bundle already downloaded (the release workflow does this)
#
# Usually run by the "SkunkCrafts release" workflow (.github/workflows/skunkcrafts-release.yml, started by hand:
#   gh workflow run skunkcrafts-release.yml -f version=5.1 -f publish=true), since GitHub's artifact storage may not be reachable
# from where this is run.
#
# <version> must be the WX_VERSION in src/version.h at that commit (default: the newest commit on main), and that commit's GitHub
# build must have passed. Each release needs a new version string: the updater shows it, and users compare it with theirs.
# Needs git and gh (logged in). WX_COMMIT_TRAILER (optional) is appended to the release commit's message. The release branch only ever gets new commits (no force pushes); main is never touched.
set -euo pipefail
VER="${1:?usage: skunkcrafts_release.sh <version> [commit] [--publish]}"; shift
PUBLISH=0; COMMIT=""; BUNDLE=""; RUN=""
while [ $# -gt 0 ]; do
  case "$1" in
    --publish) PUBLISH=1 ;;
    --bundle) BUNDLE="$2"; shift ;;
    --run) RUN="$2"; shift ;;
    *) COMMIT="$1" ;;
  esac
  shift
done
REPO=domvid2005/WXPlane
cd "$(git rev-parse --show-toplevel)"
git fetch -q origin main
git fetch -q origin release 2>/dev/null || true
SHA=$(git rev-parse "${COMMIT:-origin/main}")
git merge-base --is-ancestor "$SHA" origin/main || { echo "$SHA is not on main"; exit 1; }
HAVE=$(git show "$SHA:src/version.h" | sed -n 's/^#define WX_VERSION "\(.*\)"/\1/p')
[ "$HAVE" = "$VER" ] || { echo "src/version.h at ${SHA:0:7} says $HAVE, not $VER: bump it on main first"; exit 1; }
if git rev-parse -q --verify origin/release >/dev/null; then
  OLD=$(git show origin/release:WXPlane/skunkcrafts_updater.cfg 2>/dev/null | sed -n 's/^version|//p')
  [ "$OLD" != "$VER" ] || { echo "version $VER is already released: bump src/version.h"; exit 1; }
fi
TMPB="skunkcrafts-release-tmp-$$"   # a throw-away local branch (any local "release" branch is left alone)
WORK=$(mktemp -d); trap 'git worktree remove --force "$WORK/rel" 2>/dev/null || true; git branch -D -q "$TMPB" 2>/dev/null || true; rm -rf "$WORK"' EXIT
if [ -z "$BUNDLE" ]; then
  RUN=$(gh run list -R "$REPO" --commit "$SHA" --workflow build.yml --json databaseId,conclusion,status \
        -q '[.[] | select(.status=="completed" and .conclusion=="success")][0].databaseId')
  [ -n "$RUN" ] || { echo "no passed GitHub build for ${SHA:0:7} (still running, or it failed)"; exit 1; }
  gh run download "$RUN" -R "$REPO" -n WXPlane -D "$WORK/bundle"
  BUNDLE="$WORK/bundle/WXPlane"
fi
python3 tools/skunkcrafts_depot.py "$BUNDLE" "$WORK/depot/WXPlane" --version "$VER"
[ -n "${DEPOT_OUT:-}" ] && { mkdir -p "$DEPOT_OUT"; cp -r "$WORK/depot/WXPlane" "$DEPOT_OUT/"; }   # (for the workflow's review artifact)
if git rev-parse -q --verify origin/release >/dev/null; then
  git worktree add -q --detach "$WORK/rel" origin/release
  git -C "$WORK/rel" checkout -q -B "$TMPB"
else
  git worktree add -q --detach "$WORK/rel" "$SHA"
  git -C "$WORK/rel" checkout -q --orphan "$TMPB"
  git -C "$WORK/rel" rm -rq --cached . ; find "$WORK/rel" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
fi
rm -rf "$WORK/rel/WXPlane"; cp -r "$WORK/depot/WXPlane" "$WORK/rel/WXPlane"
cat > "$WORK/rel/README.md" <<EOF
# WXPlane update depot

This branch is read by SkunkCrafts Updater to update
WXPlane. It is made by \`tools/skunkcrafts_release.sh\` on main; do not edit it by hand. Current: **WXPlane $VER**
(main ${SHA:0:7}). Source, issues and the full download: https://github.com/$REPO
EOF
git -C "$WORK/rel" add -A
if git -C "$WORK/rel" diff --cached --quiet; then echo "nothing changed against the current release"; exit 1; fi
git -C "$WORK/rel" diff --cached --stat | tail -5
git -C "$WORK/rel" commit -q -F - <<EOF
WXPlane $VER (main ${SHA:0:7}, build $RUN)
${WX_COMMIT_TRAILER:+
$WX_COMMIT_TRAILER}
EOF
if [ "$PUBLISH" = 1 ]; then
  git -C "$WORK/rel" push -q origin "HEAD:refs/heads/release"
  echo "released WXPlane $VER to SkunkCrafts Updater (release branch $(git -C "$WORK/rel" rev-parse --short HEAD))"
else
  echo "dry run OK: WXPlane $VER from ${SHA:0:7} (build $RUN) is ready; run again with --publish to release it"
fi
