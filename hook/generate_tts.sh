#!/bin/bash
set -eu

SCRIPT_DIR=$(dirname "$0")
SCRIPT_DIR=$(cd "$SCRIPT_DIR" && pwd)

RIPPLED_ROOT="$SCRIPT_DIR/../include/xrpl"
TX_FORMATS="$RIPPLED_ROOT/protocol/detail/transactions.macro"

echo '// For documentation please see: https://xrpl-hooks.readme.io/reference/'
echo '// Generated using generate_tts.sh'
cat "$TX_FORMATS" |
    awk '
        function ltrim(s) { sub(/^[[:space:]]+/, "", s); return s }
        function rtrim(s) { sub(/[[:space:]]+$/, "", s); return s }
        function trim(s) { return rtrim(ltrim(s)) }

        /^TRANSACTION\(tt/ {
            line = $0
            sub(/^TRANSACTION\(/, "", line)

            split(line, parts, ",")
            if (length(parts) < 2)
                next

            name = trim(parts[1])
            value = trim(parts[2])
            if (name == "" || value == "")
                next

            prefix = deprecated ? "// " : ""
            postfix = deprecated ? " // deprecated" : ""
            printf "%s#define %s %s%s\n", prefix, name, value, postfix
        }
    '
