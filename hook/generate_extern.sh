#!/bin/bash
set -eu

SCRIPT_DIR=$(dirname "$0")
SCRIPT_DIR=$(cd "$SCRIPT_DIR" && pwd)

APPLY_HOOK="$SCRIPT_DIR/../src/ripple/app/hook/hook_api.macro"

{
    echo '// For documentation please see: https://xrpl-hooks.readme.io/reference/'
    echo '// Generated using generate_extern.sh'
    echo '#include <stdint.h>'
    echo '#ifndef HOOK_EXTERN'
    echo
    awk '
        function trim(s) {
            sub(/^[[:space:]]+/, "", s);
            sub(/[[:space:]]+$/, "", s);
            return s;
        }

        {
            line = $0;
            
            # Skip block comments
            if (line ~ /\/\*/) {
                next;
            }
            
            # Look for comment lines that start with // and contain function signature
            if (line ~ /^[[:space:]]*\/\/[[:space:]]*[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\(/) {
                # Remove leading // and trim
                sub(/^[[:space:]]*\/\/[[:space:]]*/, "", line);
                line = trim(line);
                
                # Check if function name is "_g" to add attribute
                if (line ~ /[[:space:]]+_g[[:space:]]*\(/) {
                    # Insert __attribute__((noduplicate)) before _g
                    sub(/[[:space:]]+_g/, " __attribute__((noduplicate)) _g", line);
                }
                
                # printf("\n");
                
                printf("extern %s\n\n", line);
            }
        }
    ' "$APPLY_HOOK"

    echo '#define HOOK_EXTERN'
    echo '#endif  // HOOK_EXTERN'
} | (
    cd "$SCRIPT_DIR/.."
    clang-format --style=file -
)
