#!/usr/bin/env bash
# Usage: open_update_pr.sh <branch-prefix> <branch> <pr-title> <pr-body-file> <commit-message-file>
# Commits every modified tracked file on a new branch, opens a pull request and starts the Build
# workflow on it.
# When RELEASE_LABEL is set, the pull request gets that label (created if missing).
# Skips when a pull request from the same prefix is still open, or when the branch already exists
# (a pull request closed without merging; delete the branch to get it again).
set -euo pipefail
prefix=$1 branch=$2 title=$3 body=$4 message=$5

open=$(PREFIX="$prefix" gh pr list --state open --json headRefName \
  --jq 'map(select(.headRefName | startswith(env.PREFIX))) | length')
if [ "$open" != "0" ]; then
  echo "A pull request from $prefix* is already open; merge or close it first."
  exit 0
fi
if git ls-remote --exit-code --heads origin "$branch" >/dev/null; then
  echo "Branch $branch already exists (pull request closed without merging); delete it to retry."
  exit 0
fi

base=$(git rev-parse --abbrev-ref HEAD)
git config user.name "github-actions[bot]"
git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
git switch -c "$branch"
git add -u
git commit -F "$message"
git push origin "$branch"
git switch "$base"
label_args=()
if [ -n "${RELEASE_LABEL:-}" ]; then
  gh label create "$RELEASE_LABEL" --color D93F0B \
    --description "Reaches servers only with a new plugin release" --force >/dev/null
  label_args=(--label "$RELEASE_LABEL")
fi
gh pr create --base "$base" --head "$branch" --title "$title" --body-file "$body" "${label_args[@]}"
# Pull requests opened with GITHUB_TOKEN do not trigger workflows; dispatch the build explicitly.
gh workflow run build.yml --ref "$branch"
