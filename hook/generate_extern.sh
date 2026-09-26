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

                if (line ~ /[[:space:]]+xport[[:space:]]*\(/) {
                    print "// callback_fee_drops: 0 omits third-party delivery permission; otherwise the";
                    print "// exact Import fee authorized by the emitted Export intent, in native drops.";
                }

                if (line ~ /[[:space:]]+entropy_cr_dice[[:space:]]*\(/) {
                    print "/*";
                    print "    Consensus entropy APIs.";
                    print "";
                    print "    min_tier is a required fail-closed floor:";
                    print "      1 = consensus_fallback, 2 = participant_aligned,";
                    print "      3 = validator_quorum, 4 = validator_full.";
                    print "";
                    print "    entropy_cr_status returns a packed non-negative value:";
                    print "      bits 32..39 tier, 16..31 count, 0..15 denominator.";
                    print "    Check for a negative error before using the ENTROPY_* macros.";
                    print "";
                    print "    Classify tier before count/denominator arithmetic: fallback is tier 1";
                    print "    with count=denominator=0. Common policies are denominator-count <= 1,";
                    print "    5*count >= 4*denominator (use widened arithmetic), or count >= floor.";
                    print "";
                    print "    In strong execution, flags=0 requires the last eligible strong Hook.";
                    print "    Otherwise: LATER_STRONG_HOOK before drawing or changing output.";
                    print "    ENTROPY_ALLOW_SAME_ACCOUNT_STRONG_VETO permits later strong Hooks";
                    print "    only on the installed account of the drawing Hook.";
                    print "    ENTROPY_ALLOW_ANY_STRONG_VETO permits them on any account, including";
                    print "    the same account. Both bits mean ANY; unknown bits are invalid.";
                    print "    Allowed Hooks retain their ordinary veto authority. An opt-in must";
                    print "    account for downstream code and whoever can replace it.";
                    print "";
                    print "    Draws return TOO_LITTLE_ENTROPY if the visible input is missing,";
                    print "    stale, or below min_tier, independently of composition flags.";
                    print "    Closed execution requires current-ledger input; open-ledger and";
                    print "    simulate execution can use previous-ledger input and are provisional";
                    print "    previews. Final ordered execution may see different entropy.";
                    print "*/";
                }
                
                # printf("\n");
                
                printf("extern %s\n\n", line);
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
