#!/bin/bash

# Get the repository's root directory
repo_root=$(git rev-parse --show-toplevel)

# Get a list of files changed in the last commit with their relative paths
files_changed=$(git diff --name-only --relative HEAD~1 HEAD)

# Loop through each file and search for the patterns
for file in $files_changed; do
  # Skip if the file is Import_test.cpp (exact filename match regardless of path)
  if [[ "$(basename "$file")" == "Import_test.cpp" ]]; then
    continue
  fi

  # Construct the absolute path
  absolute_path="$repo_root/$file"

  # Check if the file exists (it might have been deleted)
  if [ -f "$absolute_path" ]; then
    # Search the file for the given patterns, but exclude lines containing 'public_key'
    grep_output=$(grep -n -E '(([^rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz]|^)(s|p)[rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz]{25,60}([^(]|$)))|([^A-Fa-f0-9](02|03|ED)[A-Fa-f0-9]{64})' "$absolute_path" | grep -v "public_key")

    # Check if grep found any matches
    if [ ! -z "$grep_output" ]; then
      # Suspicious patterns were found
      echo "Error: Suspicious patterns were found in $absolute_path."
      echo "$grep_output"
      exit 1
    fi
  fi
done
