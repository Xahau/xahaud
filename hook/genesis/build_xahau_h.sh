#!/bin/bash

# build_xahau_h.sh
# Builds genesis hook WASMs and updates xahau.h with hex arrays

set -euo pipefail

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory and path constants
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XAHAU_H="${SCRIPT_DIR}/../../include/xrpl/hook/xahau.h"
TEMP_DIR="${SCRIPT_DIR}/.temp"

# Hook file mappings (space-separated: name:file)
HOOK_FILES=(
    "GovernanceHook:govern.wasm"
    "RewardHook:reward.wasm"
    # "MintHook:mint.wasm"
)

# Cleanup function
cleanup() {
    local exit_code=$?
    if [ ${exit_code} -eq 0 ] && [ -d "${TEMP_DIR}" ]; then
        rm -rf "${TEMP_DIR}"
    elif [ ${exit_code} -ne 0 ]; then
        echo -e "${RED}Error: Script failed with exit code ${exit_code}${NC}" >&2
        if [ -d "${TEMP_DIR}" ]; then
            echo -e "${YELLOW}Temp files preserved at: ${TEMP_DIR}${NC}" >&2
        fi
    fi
    exit ${exit_code}
}

trap cleanup EXIT INT TERM

# Tool verification
echo -e "${BLUE}==> Checking required tools...${NC}"
REQUIRED_TOOLS=("make" "xxd" "sed" "clang-format" "wasm-opt")
for tool in "${REQUIRED_TOOLS[@]}"; do
    if ! command -v "${tool}" &> /dev/null; then
        echo -e "${RED}Error: Required tool '${tool}' not found${NC}" >&2
        exit 1
    fi
    echo -e "${GREEN}    ✓ ${tool}${NC}"
done

# Verify wasm-opt version is exactly 100
WASM_OPT_VERSION=$(wasm-opt --version | grep -oE '[0-9]+' | head -1)
if [ "${WASM_OPT_VERSION}" != "100" ]; then
    echo -e "${RED}Error: wasm-opt version must be 100, but found ${WASM_OPT_VERSION}${NC}" >&2
    exit 1
fi
echo -e "${GREEN}    ✓ wasm-opt version 100${NC}"

# Verify xahau.h exists
if [ ! -f "${XAHAU_H}" ]; then
    echo -e "${RED}Error: xahau.h not found at ${XAHAU_H}${NC}" >&2
    exit 1
fi

# Create temp directory
mkdir -p "${TEMP_DIR}"

# Build all WASM files
echo -e "${BLUE}==> Building WASM files with 'make all'...${NC}"
cd "${SCRIPT_DIR}"
make all
echo -e "${GREEN}    Build completed successfully${NC}"

# Function to convert WASM to hex array
wasm_to_hex_array() {
    local wasm_file="$1"
    local indent="    "

    if [ ! -f "${wasm_file}" ]; then
        echo -e "${RED}Error: WASM file not found: ${wasm_file}${NC}" >&2
        return 1
    fi

    # Convert to hex with xxd, format with sed
    xxd -p -u -c 10 "${wasm_file}" | \
        sed 's/../0x&U,/g' | \
        sed "s/^/${indent}/g" | \
        sed '$ s/,$//'
}

# Function to update hook array in xahau.h
update_hook_array() {
    local hook_name="$1"
    local hex_array="$2"
    local temp_file="${TEMP_DIR}/xahau.h.tmp"

    echo -e "${BLUE}==> Updating ${hook_name}...${NC}"

    # Check if hook already exists
    if grep -q "static const std::vector<uint8_t> ${hook_name} = {" "${XAHAU_H}"; then
        echo -e "${YELLOW}    Replacing existing ${hook_name}${NC}"

        # Use awk to replace the array content
        awk -v hook="${hook_name}" -v hex="${hex_array}" '
        BEGIN { in_array=0 }
        {
            if ($0 ~ "static const std::vector<uint8_t> " hook " = {") {
                print $0
                print hex
                in_array=1
                next
            }
            if (in_array && $0 ~ /};/) {
                print "};"
                in_array=0
                next
            }
            if (!in_array) {
                print $0
            }
        }
        ' "${XAHAU_H}" > "${temp_file}"

        mv "${temp_file}" "${XAHAU_H}"
    else
        echo -e "${YELLOW}    Adding new ${hook_name}${NC}"

        # Find the position before #endif and add the new hook
        awk -v hook="${hook_name}" -v hex="${hex_array}" '
        {
            if ($0 ~ /#endif.*XAHAU_GENESIS_HOOKS/) {
                print ""
                print "static const std::vector<uint8_t> " hook " = {"
                print hex
                print "};"
                print ""
                print $0
            } else {
                print $0
            }
        }
        ' "${XAHAU_H}" > "${temp_file}"

        mv "${temp_file}" "${XAHAU_H}"
    fi

    echo -e "${GREEN}    ✓ ${hook_name} updated${NC}"
}

# Process each hook
for hook_entry in "${HOOK_FILES[@]}"; do
    hook_name="${hook_entry%%:*}"
    wasm_file="${SCRIPT_DIR}/${hook_entry##*:}"

    echo -e "${BLUE}==> Converting ${wasm_file} to hex array...${NC}"
    hex_array=$(wasm_to_hex_array "${wasm_file}")

    if [ $? -ne 0 ]; then
        echo -e "${RED}Error: Failed to convert ${wasm_file}${NC}" >&2
        exit 1
    fi

    echo -e "${GREEN}    Conversion successful ($(echo "${hex_array}" | wc -l) lines)${NC}"

    update_hook_array "${hook_name}" "${hex_array}"
done

# Format with clang-format
echo -e "${BLUE}==> Formatting with clang-format...${NC}"
cp "${XAHAU_H}" "${TEMP_DIR}/xahau.h.before_format"
clang-format -i "${XAHAU_H}"
echo -e "${GREEN}    Formatting completed${NC}"

# Verification
echo -e "${BLUE}==> Verifying changes...${NC}"
for hook_entry in "${HOOK_FILES[@]}"; do
    hook_name="${hook_entry%%:*}"
    if grep -q "static const std::vector<uint8_t> ${hook_name} = {" "${XAHAU_H}"; then
        echo -e "${GREEN}    ✓ ${hook_name} found in xahau.h${NC}"
    else
        echo -e "${RED}    ✗ ${hook_name} NOT found in xahau.h${NC}" >&2
        exit 1
    fi
done

# Show summary
echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Successfully updated xahau.h${NC}"
echo -e "${GREEN}========================================${NC}"
echo -e "Updated hooks:"
for hook_entry in "${HOOK_FILES[@]}"; do
    hook_name="${hook_entry%%:*}"
    wasm_file="${SCRIPT_DIR}/${hook_entry##*:}"
    size=$(wc -c < "${wasm_file}" | tr -d ' ')
    echo -e "  - ${hook_name}: ${size} bytes"
done
echo ""
echo -e "File location: ${XAHAU_H}"
echo ""
