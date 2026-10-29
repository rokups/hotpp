#!/usr/bin/env bash
# Deploy doc/ to the gh-pages branch of github.com/rokups/spp (https://rokups.github.io/spp).
# Usage: doc/deploy.sh [--build-only] [--yes]
#   --build-only  Build the site with Jekyll and do not push.
#   --yes         Push without a confirmation prompt.
set -euo pipefail

REPO="${SPP_PAGES_REPO:-https://github.com/rokups/spp.git}"
BRANCH="${SPP_PAGES_BRANCH:-gh-pages}"
DOC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"
# Use the gh credential helper for HTTPS pushes when gh is installed.
if command -v gh >/dev/null; then
  export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=credential.helper GIT_CONFIG_VALUE_0="!gh auth git-credential"
fi
trap 'rm -rf "$WORK"' EXIT

build_only=0; yes=0
for a in "$@"; do
  case "$a" in
    --build-only) build_only=1 ;;
    --yes) yes=1 ;;
    *) echo "Unknown option: $a" >&2; exit 2 ;;
  esac
done

if [ "$build_only" = 1 ]; then
  (cd "$DOC" && bundle install && bundle exec jekyll build -d "$WORK/_site")
  echo "Built to $WORK/_site (removed on exit)."
  exit 0
fi

git clone --depth 1 --branch "$BRANCH" "$REPO" "$WORK/site" 2>/dev/null \
  || { git init -q -b "$BRANCH" "$WORK/site"; git -C "$WORK/site" remote add origin "$REPO"; }

# GitHub Pages builds the Jekyll source itself. Copy the source, not the output.
find "$WORK/site" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
rsync -a --exclude 'deploy.sh' --exclude '_site' --exclude '.jekyll-cache' \
  --exclude 'Gemfile.lock' --exclude '.bundle' "$DOC"/ "$WORK/site"/
touch "$WORK/site/.nojekyll.keep" && rm "$WORK/site/.nojekyll.keep"

cd "$WORK/site"
git add -A
if git diff --cached --quiet && git rev-parse HEAD >/dev/null 2>&1; then
  echo "No change to deploy."; exit 0
fi
git status --short
if [ "$yes" != 1 ]; then
  read -r -p "Push to $REPO ($BRANCH)? [y/N] " r
  [ "$r" = y ] || { echo "Aborted."; exit 1; }
fi
git commit -q -m "Update docs from $(git -C "$DOC" rev-parse --short HEAD 2>/dev/null || echo local)"
git push origin "$BRANCH"
echo "Deployed. Site: https://rokups.github.io/spp"
