#!/bin/bash
set -eu

SCRIPT_DIR=$(dirname "$0")
SCRIPT_DIR=$(cd "$SCRIPT_DIR" && pwd)

RIPPLED_ROOT="$SCRIPT_DIR/../include/xrpl"
LEDGER_FORMATS="$RIPPLED_ROOT/protocol/LedgerFormats.h"

echo '// Generated using generate_lsflags.sh'
echo ''
echo '#ifndef HOOKLSFLAGS_INCLUDED'
echo '#define HOOKLSFLAGS_INCLUDED 1'
echo ''
awk '
    function ltrim(s) { sub(/^[[:space:]]+/, "", s); return s }
    function rtrim(s) { sub(/[[:space:]]+$/, "", s); return s }
    function trim(s) { return rtrim(ltrim(s)) }

    function flush_group() {
        if (entry_count > 0 && group != "") {
            printf "enum %s {\n", group
            for (i = 1; i <= entry_count; i++) {
                printf "    %s,\n", entries[i]
            }
            printf "};\n"
        }
        delete entries
        entry_count = 0
    }

    /enum LedgerSpecificFlags \{/ { inside = 1; next }
    inside && /^\};/ { inside = 0; flush_group(); next }
    !inside { next }

    # Group header comments: // ltFOO or // remarks
    /^[[:space:]]*\/\/[[:space:]]*(lt[A-Z_]+|remarks)[[:space:]]*$/ {
        flush_group()
        line = $0
        sub(/.*\/\/[[:space:]]*/, "", line)
        group = trim(line)
        next
    }

    # Skip pure comment lines (not group headers)
    /^[[:space:]]*\/\// { next }

    # Skip blank lines
    /^[[:space:]]*$/ { next }

    # Accumulate flag lines (handle multi-line values)
    {
        line = $0
        # Strip inline comments
        sub(/\/\/.*/, "", line)
        line = trim(line)
        if (line == "") next

        if (pending != "") {
            pending = pending " " line
        } else {
            pending = line
        }

        # If line ends with comma, the entry is complete
        if (pending ~ /,$/) {
            # Remove trailing comma
            sub(/,$/, "", pending)
            entries[++entry_count] = pending
            pending = ""
        }
    }

    BEGIN {
        inside = 0
        group = ""
        pending = ""
        entry_count = 0
    }
' "$LEDGER_FORMATS"
echo ''
echo '#endif // HOOKLSFLAGS_INCLUDED'
