#!/bin/bash
set -eu

SCRIPT_DIR=$(dirname "$0")
SCRIPT_DIR=$(cd "$SCRIPT_DIR" && pwd)

APPLY_HOOK="$SCRIPT_DIR/../src/ripple/app/hook/applyHook.h"

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

        function emit(ret, name, argc, argt, argn) {
            attr = (name == "_g") ? " __attribute__((noduplicate))" : "";
            if (!first)
                printf("\n");
            first = 0;
            printf("extern %s%s\n", ret, attr);
            if (argc == 0) {
                printf("%s(void);\n", name);
                return;
            }
            if (argc <= 3) {
                line = argt[1] " " argn[1];
                for (i = 2; i <= argc; ++i)
                    line = line ", " argt[i] " " argn[i];
                printf("%s(%s);\n", name, line);
                return;
            }
            printf("%s(\n", name);
            for (i = 1; i <= argc; ++i) {
                sep = (i < argc) ? "," : ");";
                printf("    %s %s%s\n", argt[i], argn[i], sep);
            }
        }

        function process(buffer, kind,    payload, parts, n, i, arg, tokens, argc, argt, argn) {
            if (kind == "func")
                sub(/^DECLARE_HOOK_FUNCTION[[:space:]]*\(/, "", buffer);
            else
                sub(/^DECLARE_HOOK_FUNCNARG[[:space:]]*\(/, "", buffer);
            buffer = trim(buffer);
            sub(/\)[[:space:]]*$/, "", buffer);
            n = split(buffer, parts, ",");
            for (i = 1; i <= n; ++i)
                parts[i] = trim(parts[i]);
            ret = parts[1];
            name = parts[2];
            argc = 0;
            delete argt;
            delete argn;
            for (i = 3; i <= n; ++i) {
                arg = parts[i];
                if (arg == "")
                    continue;
                split(arg, tokens, /[[:space:]]+/);
                if (length(tokens) < 2)
                    continue;
                ++argc;
                argt[argc] = tokens[1];
                argn[argc] = tokens[2];
            }
            emit(ret, name, argc, argt, argn);
        }

        BEGIN {
            first = 1;
            in_block = 0;
            in_macro = 0;
        }

        {
            line = $0;
            if (in_block) {
                if (line ~ /\*\//) {
                    sub(/.*\*\//, "", line);
                    in_block = 0;
                }
                else
                    next;
            }
            while (line ~ /\/\*/) {
                if (line ~ /\/\*.*\*\//) {
                    gsub(/\/\*.*\*\//, "", line);
                }
                else {
                    sub(/\/\*.*/, "", line);
                    in_block = 1;
                    break;
                }
            }
            sub(/\/\/.*$/, "", line);
            line = trim(line);
            if (line == "")
                next;

            if (!in_macro && line ~ /^DECLARE_HOOK_FUNCTION\(/) {
                buffer = line;
                kind = "func";
                if (line ~ /\);[[:space:]]*$/) {
                    sub(/\);[[:space:]]*$/, "", buffer);
                    process(buffer, kind);
                }
                else
                    in_macro = 1;
                next;
            }
            if (!in_macro && line ~ /^DECLARE_HOOK_FUNCNARG\(/) {
                buffer = line;
                kind = "narg";
                if (line ~ /\);[[:space:]]*$/) {
                    sub(/\);[[:space:]]*$/, "", buffer);
                    process(buffer, kind);
                }
                else
                    in_macro = 1;
                next;
            }
            if (in_macro) {
                buffer = buffer " " line;
                if (line ~ /\);[[:space:]]*$/) {
                    sub(/\);[[:space:]]*$/, "", buffer);
                    process(buffer, kind);
                    in_macro = 0;
                }
            }
        }

        END {
            printf("\n");
        }
    ' "$APPLY_HOOK"

    echo '#define HOOK_EXTERN'
    echo '#endif  // HOOK_EXTERN'
}
