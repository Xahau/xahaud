#!/bin/bash

# Get the repository's root directory
repo_root=$(git rev-parse --show-toplevel)

# Get a list of files changed in the last commit with their relative paths
files_changed=$(git diff --name-only --relative HEAD~1 HEAD)

# Loop through each file and search for the patterns
for file in $files_changed; do
  # Construct the absolute path
  absolute_path="$repo_root/$file"

  # Check if the file exists (it might have been deleted)
  if [ -f "$absolute_path" ]; then
    # Search the file for the given patterns
    grep_output=$(grep -n -E '(([^rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz]|^)(s|p)[rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz]{25,60}([^(]|$)))|([^A-Fa-f0-9](02|03|ED)[A-Fa-f0-9]{64})' "$absolute_path")

    # Check if grep found any matches
    if [ ! -z "$grep_output" ]; then
      # Suspicious patterns were found
      echo "Error: Suspicious patterns were found in $absolute_path."
      echo "$grep_output"
      exit 1
    fi
  fi
done

# If the loop completes without finding any suspicious patterns
echo "Success: No suspicious patterns found in the diff."
exit 0