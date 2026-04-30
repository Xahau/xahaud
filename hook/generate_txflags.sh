#!/bin/bash
set -eu

SCRIPT_DIR=$(dirname "$0")
SCRIPT_DIR=$(cd "$SCRIPT_DIR" && pwd)

RIPPLED_ROOT="$SCRIPT_DIR/../include/xrpl"
TX_FLAGS="$RIPPLED_ROOT/protocol/TxFlags.h"

echo '// Generated using generate_txflags.sh'
echo '#include "ls_flags.h"'
echo '#include <stdint.h>'
echo ''
cat "$TX_FLAGS" |
    awk '
        /^[[:space:]]*enum / {
            if (count > 0) print ""
            inside = 1
            count++
        }
        inside {
            print
            if (/};/) inside = 0
        }
    '
