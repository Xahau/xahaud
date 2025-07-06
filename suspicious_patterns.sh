#!/bin/bash

# Exit on error, undefined variables, and pipe failures
set -euo pipefail

# Enable debug mode if DEBUG environment variable is set
[[ "${DEBUG:-}" == "1" ]] && set -x

# This script prevents accidental commits of XRPL/Ripple cryptographic keys
# It searches for:
#   - Secret seeds (s...) - can derive keypairs
#   - Private keys (p...) - validator private keys! 
#   - Raw key material (02/03/ED + hex) - compressed public keys or Ed25519 keys
#
# Usage:
#   suspicious_patterns.sh           # Check last commit (for CI)
#   suspicious_patterns.sh --pre-commit  # Check staged files (for pre-commit hook)
#
# WARNING: If this catches a real key in CI, that key is already compromised!
# The key has been pushed to the git history and must be immediately decommissioned.
# 
# To mark keys as safe, add comment: // not-suspicious
# Files excluded: See exclude_files array below
# Lines excluded: Matching exclude_pattern regex below

# Pattern for lines to exclude from checking
exclude_pattern="public_key|not-suspicious"

# Array of files to exclude from checking (paths relative to repo root)
exclude_files=(
  "src/test/app/Import_test.cpp"
  "cfg/validators-example.txt"
)

# Get the repository's root directory
repo_root=$(git rev-parse --show-toplevel)

# Determine which files to check based on context:
# 1. Pre-commit hook: Check staged files before commit
# 2. GitHub PR: Check all files changed in the PR (HEAD is a synthetic merge commit)
# 3. Regular push/CI: Check files in the last real commit
if [[ "${1:-}" == "--pre-commit" ]]; then
  # Pre-commit mode: Check what's about to be committed
  files_changed=$(git diff --cached --name-only --relative)
  mode="staged files"
else
  # CI mode - need to handle two different scenarios
  if [[ "${GITHUB_EVENT_NAME:-}" == "pull_request" ]]; then
    # GitHub PR event: HEAD is a synthetic merge commit created by GitHub
    # that merges PR branch into base. Must diff against base to get only PR files.
    base_ref="${GITHUB_BASE_REF:-dev}"
    
    # Ensure we have the base branch (GitHub Actions shallow clone might not have it)
    if ! git rev-parse --verify "origin/$base_ref" >/dev/null 2>&1; then
      echo "Fetching base branch origin/$base_ref..."
      git fetch --depth=1 origin "$base_ref"
    fi
    
    # Since there's no merge base in shallow clones, we need to be creative
    # Save current HEAD, switch to base, then diff the trees
    current_head=$(git rev-parse HEAD)
    echo "Comparing against base branch origin/$base_ref..."
    
    # Get the tree objects to compare (this works even without shared history)
    base_tree=$(git rev-parse "origin/$base_ref^{tree}")
    head_tree=$(git rev-parse "$current_head^{tree}")
    
    # Compare the two trees directly
    files_changed=$(git diff --name-only "$base_tree" "$head_tree")
    mode="PR changes"
  else
    # Regular push event: Check the actual commit that was pushed
    # Using 'git show' works even with shallow clones (no HEAD~1 needed)
    # See: https://github.com/Xahau/xahaud/actions/runs/15492442104/job/43620965462#step:3:11
    files_changed=$(git show --name-only --pretty=format:'' HEAD)
    mode="last commit"
  fi
fi

echo "Checking $mode for suspicious patterns..."

# Show additional info in CI or when verbose mode is enabled
if [[ -n "${CI:-}" ]] || [[ "${VERBOSE:-}" == "1" ]]; then
  if [[ "${1:-}" != "--pre-commit" ]]; then
    echo "Commit: $(git rev-parse HEAD)"
  fi
  echo "Files to check:"
  if [[ -n "$files_changed" ]]; then
    echo "$files_changed" | nl
  else
    echo "  (none)"
  fi
fi

# Loop through each file and search for the patterns
for file in $files_changed; do
  # Check if file should be excluded (exact path match)
  for excluded in "${exclude_files[@]}"; do
    if [[ "$file" == "$excluded" ]]; then
      continue 2  # Continue outer loop
    fi
  done

  # Construct the absolute path
  absolute_path="$repo_root/$file"

  # Check if the file exists (it might have been deleted)
  if [ -f "$absolute_path" ]; then
    # Get file content based on mode
    if [[ "${1:-}" == "--pre-commit" ]]; then
      # For staged files, use git show with the staging area
      file_content=$(git show ":$file" 2>/dev/null)
    else
      # For committed files, read from disk
      file_content=$(cat "$absolute_path")
    fi
    
    # Search the file content for the given patterns, but exclude lines matching the exclusion pattern
    # Use || true to prevent grep from failing the script when no matches are found
    grep_output=$(echo "$file_content" | grep -n -E '(([^rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz]|^)(s|p)[rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz]{25,60}([^(]|$)))|([^A-Fa-f0-9](02|03|ED)[A-Fa-f0-9]{64})' | grep -vE "$exclude_pattern" || true)

    # Check if grep found any matches
    if [ ! -z "$grep_output" ]; then
      # Suspicious patterns were found
      echo "Error: Suspicious patterns were found in $absolute_path."
      echo "$grep_output"
      exit 1
    fi
  fi
done
