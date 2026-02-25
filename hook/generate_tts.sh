#!/bin/bash
set -eu

SCRIPT_DIR=$(dirname "$0")
SCRIPT_DIR=$(cd "$SCRIPT_DIR" && pwd)

RIPPLED_ROOT="$SCRIPT_DIR/../src/ripple"
TX_FORMATS="$RIPPLED_ROOT/protocol/TxFormats.h"

echo '// For documentation please see: https://xrpl-hooks.readme.io/reference/'
echo '// Generated using generate_tts.sh'
sed -n '/enum TxType/,/};/p' "$TX_FORMATS" |
    awk '
        function ltrim(s) { sub(/^[[:space:]]+/, "", s); return s }
        function rtrim(s) { sub(/[[:space:]]+$/, "", s); return s }
        function trim(s) { return rtrim(ltrim(s)) }

        /^[ \t]*tt[A-Z0-9_]+/ {
            line = $0
            deprecated = (line ~ /\[\[deprecated/)
            gsub(/\[\[deprecated[^]]*\]\]/, "", line)
            sub(/\/\/.*$/, "", line)
            gsub(/,[[:space:]]*$/, "", line)

            split(line, parts, "=")
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
