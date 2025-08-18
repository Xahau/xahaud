#!/bin/bash
set -eu

SCRIPT_DIR=$(dirname "$0")
SCRIPT_DIR=$(cd "$SCRIPT_DIR" && pwd)

ENUM_FILE="$SCRIPT_DIR/../include/xrpl/hook/Enum.h"

echo '// For documentation please see: https://xrpl-hooks.readme.io/reference/'
echo '// Generated using generate_error.sh'
echo '#ifndef HOOK_ERROR_CODES'
sed -n '/enum hook_return_code/,/};/p' "$ENUM_FILE" |
    awk '
        function ltrim(s) { sub(/^[[:space:]]+/, "", s); return s }
        function rtrim(s) { sub(/[[:space:]]+$/, "", s); return s }
        function trim(s) { return rtrim(ltrim(s)) }
        function emit(entry) {
            entry = trim(entry)
            if (entry == "")
                return
            gsub(/,[[:space:]]*$/, "", entry)
            split(entry, parts, "=")
            if (length(parts) < 2)
                return
            name = trim(parts[1])
            value = trim(parts[2])
            if (name == "" || value == "")
                return
            printf "#define %s %s\n", name, value
        }

        {
            line = $0
            if (line ~ /enum[[:space:]]+hook_return_code/)
                next
            if (line ~ /^[[:space:]]*\{/)
                next

            sub(/\/\/.*$/, "", line)

            if (line ~ /^[[:space:]]*\};/) {
                emit(buffer)
                exit
            }

            if (line ~ /^[[:space:]]*$/)
                next

            buffer = buffer line " "

            if (line ~ /,[[:space:]]*$/) {
                emit(buffer)
                buffer = ""
            }
        }
    '
echo '#define HOOK_ERROR_CODES'
echo '#endif //HOOK_ERROR_CODES'
