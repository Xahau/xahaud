#!/bin/bash
set -eu

SCRIPT_DIR=$(dirname "$0")
SCRIPT_DIR=$(cd "$SCRIPT_DIR" && pwd)

APPLY_HOOK="$SCRIPT_DIR/../include/xrpl/hook/hook_api.macro"

{
    echo '// For documentation please see: https://xrpl-hooks.readme.io/reference/'
    echo '// Generated using generate_extern.sh'
    echo '#include <stdint.h>'
    echo '#ifndef HOOK_EXTERN'
    echo '#ifdef __cplusplus'
    echo 'extern "C" {'
    echo '#endif'
    echo
    awk '
        function trim(s) {
            sub(/^[[:space:]]+/, "", s);
            sub(/[[:space:]]+$/, "", s);
            return s;
        }

        # Each entry is HOOK_API_DEFINITION(RET, NAME, (PARAMS), AMENDMENT),
        # possibly spread over several lines. Collect until the parentheses
        # balance, then split on the first two commas; the parameter tuple
        # is the first parenthesised group.
        function emit(s,    i, ret, name, p, q, params) {
            sub(/^[[:space:]]*HOOK_API_DEFINITION[[:space:]]*\(/, "", s);
            gsub(/[[:space:]]+/, " ", s);
            i = index(s, ","); ret = trim(substr(s, 1, i - 1)); s = substr(s, i + 1);
            i = index(s, ","); name = trim(substr(s, 1, i - 1)); s = substr(s, i + 1);
            p = index(s, "("); q = index(s, ")");
            params = trim(substr(s, p + 1, q - p - 1));
            if (name == "_g")
                name = "__attribute__((noduplicate)) _g";
            printf("extern %s %s(%s);\n\n", ret, name, params);
        }

        /^[[:space:]]*HOOK_API_DEFINITION[[:space:]]*\(/ { buf = ""; collecting = 1; }

        collecting {
            buf = buf " " $0;
            if (gsub(/\(/, "(", buf) == gsub(/\)/, ")", buf)) {
                collecting = 0;
                emit(buf);
            }
        }
    ' "$APPLY_HOOK"

    echo '#ifdef __cplusplus'
    echo '}'
    echo '#endif'
    echo '#define HOOK_EXTERN'
    echo '#endif  // HOOK_EXTERN'
} | (
    cd "$SCRIPT_DIR/.."
    clang-format --style=file -
)
