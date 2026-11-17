//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <xrpld/app/ledger/BuildLedger.h>
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/ledger/LedgerReplay.h>
#include <xrpld/app/misc/CanonicalTXSet.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/ledger/ApplyViewImpl.h>
#include <xrpld/ledger/OpenView.h>
#include <xrpl/hook/Enum.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>
#include <algorithm>
#include <functional>
#include <set>

namespace ripple {
namespace test {

namespace {

// Test Hooks for featureRNG. Each one draws, then accept()s with the API's
// return value as its exit code. Built from the WAT shown with wat2wasm
// (wabt 1.0.36) and checked with guard_checker.

// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "dice" (func $dice (param i32) (result i64)))
//   (memory 1)
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $accept (i32.const 0) (i32.const 0) (call $dice (i32.const 6))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const dice6Wasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x13U, 0x03U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x02U, 0x22U, 0x03U, 0x03U,
    0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU,
    0x76U, 0x06U, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U,
    0x65U, 0x6EU, 0x76U, 0x04U, 0x64U, 0x69U, 0x63U, 0x65U, 0x00U, 0x02U, 0x03U,
    0x02U, 0x01U, 0x02U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U, 0x11U, 0x02U,
    0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U,
    0x6FU, 0x6FU, 0x6BU, 0x00U, 0x03U, 0x0AU, 0x15U, 0x01U, 0x13U, 0x00U, 0x41U,
    0x01U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x41U,
    0x06U, 0x10U, 0x02U, 0x10U, 0x01U, 0x0BU,
};

// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "dice" (func $dice (param i32) (result i64)))
//   (memory 1)
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $accept (i32.const 0) (i32.const 0) (call $dice (i32.const 0))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const dice0Wasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x13U, 0x03U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x02U, 0x22U, 0x03U, 0x03U,
    0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU,
    0x76U, 0x06U, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U,
    0x65U, 0x6EU, 0x76U, 0x04U, 0x64U, 0x69U, 0x63U, 0x65U, 0x00U, 0x02U, 0x03U,
    0x02U, 0x01U, 0x02U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U, 0x11U, 0x02U,
    0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U,
    0x6FU, 0x6FU, 0x6BU, 0x00U, 0x03U, 0x0AU, 0x15U, 0x01U, 0x13U, 0x00U, 0x41U,
    0x01U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x41U,
    0x00U, 0x10U, 0x02U, 0x10U, 0x01U, 0x0BU,
};

// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "dice" (func $dice (param i32) (result i64)))
//   (memory 1)
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $accept (i32.const 0) (i32.const 0) (call $dice (i32.const -1))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const diceMaxWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x13U, 0x03U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x02U, 0x22U, 0x03U, 0x03U,
    0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU,
    0x76U, 0x06U, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U,
    0x65U, 0x6EU, 0x76U, 0x04U, 0x64U, 0x69U, 0x63U, 0x65U, 0x00U, 0x02U, 0x03U,
    0x02U, 0x01U, 0x02U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U, 0x11U, 0x02U,
    0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U,
    0x6FU, 0x6FU, 0x6BU, 0x00U, 0x03U, 0x0AU, 0x15U, 0x01U, 0x13U, 0x00U, 0x41U,
    0x01U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x41U,
    0x7FU, 0x10U, 0x02U, 0x10U, 0x01U, 0x0BU,
};

// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "dice" (func $dice (param i32) (result i64)))
//   (memory 1)
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $accept (i32.const 0) (i32.const 0) (i64.extend_i32_u (i64.eq (call
//     $dice (i32.const -1)) (call $dice (i32.const -1))))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const diceTwiceWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x13U, 0x03U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x02U, 0x22U, 0x03U, 0x03U,
    0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU,
    0x76U, 0x06U, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U,
    0x65U, 0x6EU, 0x76U, 0x04U, 0x64U, 0x69U, 0x63U, 0x65U, 0x00U, 0x02U, 0x03U,
    0x02U, 0x01U, 0x02U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U, 0x11U, 0x02U,
    0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U,
    0x6FU, 0x6FU, 0x6BU, 0x00U, 0x03U, 0x0AU, 0x1BU, 0x01U, 0x19U, 0x00U, 0x41U,
    0x01U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x41U,
    0x7FU, 0x10U, 0x02U, 0x41U, 0x7FU, 0x10U, 0x02U, 0x51U, 0xADU, 0x10U, 0x01U,
    0x0BU,
};

// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "rollback" (func $rollback (param i32 i32 i64) (result i64)))
//   (import "env" "dice" (func $dice (param i32) (result i64)))
//   (memory 1)
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $rollback (i32.const 0) (i32.const 0) (call $dice (i32.const -1))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const diceRollbackWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x13U, 0x03U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x02U, 0x24U, 0x03U, 0x03U,
    0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU,
    0x76U, 0x08U, 0x72U, 0x6FU, 0x6CU, 0x6CU, 0x62U, 0x61U, 0x63U, 0x6BU, 0x00U,
    0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x04U, 0x64U, 0x69U, 0x63U, 0x65U, 0x00U,
    0x02U, 0x03U, 0x02U, 0x01U, 0x02U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U,
    0x11U, 0x02U, 0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U,
    0x04U, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x00U, 0x03U, 0x0AU, 0x15U, 0x01U, 0x13U,
    0x00U, 0x41U, 0x01U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x00U, 0x41U,
    0x00U, 0x41U, 0x7FU, 0x10U, 0x02U, 0x10U, 0x01U, 0x0BU,
};

// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "util_random" (func $util_random (param i32 i32) (result
//   i64))) (memory 1) (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $accept (i32.const 0) (i32.const 0) (call $util_random (i32.const
//     0) (i32.const 32))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const utilRandom32Wasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x19U, 0x04U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU,
    0x01U, 0x7EU, 0x02U, 0x29U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU,
    0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U, 0x63U,
    0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0BU, 0x75U,
    0x74U, 0x69U, 0x6CU, 0x5FU, 0x72U, 0x61U, 0x6EU, 0x64U, 0x6FU, 0x6DU, 0x00U,
    0x02U, 0x03U, 0x02U, 0x01U, 0x03U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U,
    0x11U, 0x02U, 0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U,
    0x04U, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x00U, 0x03U, 0x0AU, 0x17U, 0x01U, 0x15U,
    0x00U, 0x41U, 0x01U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x00U, 0x41U,
    0x00U, 0x41U, 0x00U, 0x41U, 0x20U, 0x10U, 0x02U, 0x10U, 0x01U, 0x0BU,
};

// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "util_random" (func $util_random (param i32 i32) (result
//   i64))) (memory 1) (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $accept (i32.const 0) (i32.const 0) (call $util_random (i32.const
//     0) (i32.const 0))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const utilRandom0Wasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x19U, 0x04U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU,
    0x01U, 0x7EU, 0x02U, 0x29U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU,
    0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U, 0x63U,
    0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0BU, 0x75U,
    0x74U, 0x69U, 0x6CU, 0x5FU, 0x72U, 0x61U, 0x6EU, 0x64U, 0x6FU, 0x6DU, 0x00U,
    0x02U, 0x03U, 0x02U, 0x01U, 0x03U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U,
    0x11U, 0x02U, 0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U,
    0x04U, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x00U, 0x03U, 0x0AU, 0x17U, 0x01U, 0x15U,
    0x00U, 0x41U, 0x01U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x00U, 0x41U,
    0x00U, 0x41U, 0x00U, 0x41U, 0x00U, 0x10U, 0x02U, 0x10U, 0x01U, 0x0BU,
};

// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "util_random" (func $util_random (param i32 i32) (result
//   i64))) (memory 1) (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $accept (i32.const 0) (i32.const 0) (call $util_random (i32.const
//     0) (i32.const 513))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const utilRandom513Wasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x19U, 0x04U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU,
    0x01U, 0x7EU, 0x02U, 0x29U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU,
    0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U, 0x63U,
    0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0BU, 0x75U,
    0x74U, 0x69U, 0x6CU, 0x5FU, 0x72U, 0x61U, 0x6EU, 0x64U, 0x6FU, 0x6DU, 0x00U,
    0x02U, 0x03U, 0x02U, 0x01U, 0x03U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U,
    0x11U, 0x02U, 0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U,
    0x04U, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x00U, 0x03U, 0x0AU, 0x18U, 0x01U, 0x16U,
    0x00U, 0x41U, 0x01U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x00U, 0x41U,
    0x00U, 0x41U, 0x00U, 0x41U, 0x81U, 0x04U, 0x10U, 0x02U, 0x10U, 0x01U, 0x0BU,
};

// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "slot_set" (func $slot_set (param i32 i32 i32) (result i64)))
//   (memory 1)
//   (data (i32.const 0)
//   "\52\6e\12\5c\53\f2\38\f6\ca\65\c4\1a\32\87\f7\60\cc\f0\4f\2d\04\07\a8\fe\01\6b\59\f4\60\fe\14\f0\c6\77")
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $accept (i32.const 0) (i32.const 0) (call $slot_set (i32.const 0)
//     (i32.const 34) (i32.const 0))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const slotRandomWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x1AU, 0x04U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x01U,
    0x7FU, 0x01U, 0x7EU, 0x02U, 0x26U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x02U,
    0x5FU, 0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U,
    0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x08U,
    0x73U, 0x6CU, 0x6FU, 0x74U, 0x5FU, 0x73U, 0x65U, 0x74U, 0x00U, 0x02U, 0x03U,
    0x02U, 0x01U, 0x03U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U, 0x11U, 0x02U,
    0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U,
    0x6FU, 0x6FU, 0x6BU, 0x00U, 0x03U, 0x0AU, 0x19U, 0x01U, 0x17U, 0x00U, 0x41U,
    0x01U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x41U,
    0x00U, 0x41U, 0x22U, 0x41U, 0x00U, 0x10U, 0x02U, 0x10U, 0x01U, 0x0BU, 0x0BU,
    0x28U, 0x01U, 0x00U, 0x41U, 0x00U, 0x0BU, 0x22U, 0x52U, 0x6EU, 0x12U, 0x5CU,
    0x53U, 0xF2U, 0x38U, 0xF6U, 0xCAU, 0x65U, 0xC4U, 0x1AU, 0x32U, 0x87U, 0xF7U,
    0x60U, 0xCCU, 0xF0U, 0x4FU, 0x2DU, 0x04U, 0x07U, 0xA8U, 0xFEU, 0x01U, 0x6BU,
    0x59U, 0xF4U, 0x60U, 0xFEU, 0x14U, 0xF0U, 0xC6U, 0x77U,
};

// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "slot_set" (func $slot_set (param i32 i32 i32) (result i64)))
//   (memory 1)
//   (data (i32.const 0)
//   "\00\00\12\5c\53\f2\38\f6\ca\65\c4\1a\32\87\f7\60\cc\f0\4f\2d\04\07\a8\fe\01\6b\59\f4\60\fe\14\f0\c6\77")
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $accept (i32.const 0) (i32.const 0) (call $slot_set (i32.const 0)
//     (i32.const 34) (i32.const 0))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const slotAnyWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x1AU, 0x04U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x01U,
    0x7FU, 0x01U, 0x7EU, 0x02U, 0x26U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x02U,
    0x5FU, 0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U,
    0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x08U,
    0x73U, 0x6CU, 0x6FU, 0x74U, 0x5FU, 0x73U, 0x65U, 0x74U, 0x00U, 0x02U, 0x03U,
    0x02U, 0x01U, 0x03U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U, 0x11U, 0x02U,
    0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U,
    0x6FU, 0x6FU, 0x6BU, 0x00U, 0x03U, 0x0AU, 0x19U, 0x01U, 0x17U, 0x00U, 0x41U,
    0x01U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x41U,
    0x00U, 0x41U, 0x22U, 0x41U, 0x00U, 0x10U, 0x02U, 0x10U, 0x01U, 0x0BU, 0x0BU,
    0x28U, 0x01U, 0x00U, 0x41U, 0x00U, 0x0BU, 0x22U, 0x00U, 0x00U, 0x12U, 0x5CU,
    0x53U, 0xF2U, 0x38U, 0xF6U, 0xCAU, 0x65U, 0xC4U, 0x1AU, 0x32U, 0x87U, 0xF7U,
    0x60U, 0xCCU, 0xF0U, 0x4FU, 0x2DU, 0x04U, 0x07U, 0xA8U, 0xFEU, 0x01U, 0x6BU,
    0x59U, 0xF4U, 0x60U, 0xFEU, 0x14U, 0xF0U, 0xC6U, 0x77U,
};

// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "ledger_keylet" (func $ledger_keylet (param i32 i32 i32 i32
//   i32 i32) (result i64))) (memory 1) (data (i32.const 0)
//   "\00\00\12\5c\53\f2\38\f6\ca\65\c4\1a\32\87\f7\60\cc\f0\4f\2d\04\07\a8\fe\01\6b\59\f4\60\fe\14\f0\c6\76")
//   (data (i32.const 34)
//   "\00\00\12\5c\53\f2\38\f6\ca\65\c4\1a\32\87\f7\60\cc\f0\4f\2d\04\07\a8\fe\01\6b\59\f4\60\fe\14\f0\c6\78")
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $accept (i32.const 0) (i32.const 0) (call $ledger_keylet (i32.const
//     100) (i32.const 34) (i32.const 0) (i32.const 34) (i32.const 34)
//     (i32.const 34))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const ledgerKeyletWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x1DU, 0x04U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x06U, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x01U,
    0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x02U, 0x2BU, 0x03U, 0x03U, 0x65U,
    0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU, 0x76U,
    0x06U, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U, 0x65U,
    0x6EU, 0x76U, 0x0DU, 0x6CU, 0x65U, 0x64U, 0x67U, 0x65U, 0x72U, 0x5FU, 0x6BU,
    0x65U, 0x79U, 0x6CU, 0x65U, 0x74U, 0x00U, 0x02U, 0x03U, 0x02U, 0x01U, 0x03U,
    0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U, 0x11U, 0x02U, 0x06U, 0x6DU, 0x65U,
    0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U, 0x6FU, 0x6FU, 0x6BU,
    0x00U, 0x03U, 0x0AU, 0x20U, 0x01U, 0x1EU, 0x00U, 0x41U, 0x01U, 0x41U, 0x01U,
    0x10U, 0x00U, 0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x41U, 0xE4U, 0x00U, 0x41U,
    0x22U, 0x41U, 0x00U, 0x41U, 0x22U, 0x41U, 0x22U, 0x41U, 0x22U, 0x10U, 0x02U,
    0x10U, 0x01U, 0x0BU, 0x0BU, 0x4FU, 0x02U, 0x00U, 0x41U, 0x00U, 0x0BU, 0x22U,
    0x00U, 0x00U, 0x12U, 0x5CU, 0x53U, 0xF2U, 0x38U, 0xF6U, 0xCAU, 0x65U, 0xC4U,
    0x1AU, 0x32U, 0x87U, 0xF7U, 0x60U, 0xCCU, 0xF0U, 0x4FU, 0x2DU, 0x04U, 0x07U,
    0xA8U, 0xFEU, 0x01U, 0x6BU, 0x59U, 0xF4U, 0x60U, 0xFEU, 0x14U, 0xF0U, 0xC6U,
    0x76U, 0x00U, 0x41U, 0x22U, 0x0BU, 0x22U, 0x00U, 0x00U, 0x12U, 0x5CU, 0x53U,
    0xF2U, 0x38U, 0xF6U, 0xCAU, 0x65U, 0xC4U, 0x1AU, 0x32U, 0x87U, 0xF7U, 0x60U,
    0xCCU, 0xF0U, 0x4FU, 0x2DU, 0x04U, 0x07U, 0xA8U, 0xFEU, 0x01U, 0x6BU, 0x59U,
    0xF4U, 0x60U, 0xFEU, 0x14U, 0xF0U, 0xC6U, 0x78U,
};

}  // namespace

class RNG_test : public beast::unit_test::suite
{
    // Domain separators, as in Change.cpp and HookAPI.cpp.
    static constexpr std::uint32_t rngSeedTag = 0x524E4753;        // 'RNGS'
    static constexpr std::uint32_t rngAccumulateTag = 0x524E4741;  // 'RNGA'
    static constexpr std::uint32_t rngStepTag = 0x524E4749;        // 'RNGI'

    int hookAccounts_ = 0;

    static STTx
    entropyTx(
        std::uint32_t seq,
        PublicKey const& pk,
        uint256 const& nextCommitment,
        std::optional<uint256> const& reveal = std::nullopt)
    {
        return STTx(ttENTROPY, [&](auto& obj) {
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.setFieldVL(sfPublicKey, pk.slice());
            obj.setFieldH256(sfNextRandomDigest, nextCommitment);
            if (reveal)
                obj.setFieldH256(sfRandomData, *reveal);
        });
    }

    // featureRNGV2: one validator's entry in an aggregate
    struct Contribution
    {
        Blob pk;
        uint256 nextCommitment;
        std::optional<uint256> reveal;
        std::optional<std::uint32_t> entrySeq;  // never set by a valid entry
    };

    static STTx
    aggregateTx(
        std::uint32_t seq,
        std::vector<Contribution> const& cs,
        std::function<void(STObject&)> const& extra = {})
    {
        return STTx(ttENTROPY, [&](auto& obj) {
            obj.setFieldU32(sfLedgerSequence, seq);
            STArray digests(sfRandomDigests);
            for (auto const& c : cs)
            {
                STObject e(sfRandomDigestEntry);
                e.setFieldVL(sfPublicKey, c.pk);
                e.setFieldH256(sfNextRandomDigest, c.nextCommitment);
                if (c.reveal)
                    e.setFieldH256(sfRandomData, *c.reveal);
                if (c.entrySeq)
                    e.setFieldU32(sfLedgerSequence, *c.entrySeq);
                digests.push_back(std::move(e));
            }
            obj.setFieldArray(sfRandomDigests, digests);
            if (extra)
                extra(obj);
        });
    }

    static Blob
    blobOf(PublicKey const& pk)
    {
        return Blob(pk.data(), pk.data() + pk.size());
    }

    static std::string
    suffix(FeatureBitset const& features)
    {
        return features[featureRNGV2] ? " (V2)" : " (V1)";
    }

    static TER
    applyTx(jtx::Env& env, OpenView& view, STTx const& tx)
    {
        return apply(env.app(), view, tx, tapNONE, env.journal).ter;
    }

    static uint256
    secret(std::uint32_t i)
    {
        return sha512Half(std::uint32_t{0x5EC12E7}, i);
    }

    static uint256
    step(uint256 const& state)
    {
        return sha512Half(rngStepTag, state);
    }

    // Negative Hook return codes are recorded as 0x8000000000000000 + |code|.
    static std::uint64_t
    rc(hook_api::hook_return_code code)
    {
        return 0x8000000000000000ULL +
            static_cast<std::uint64_t>(-static_cast<std::int64_t>(code));
    }

    static std::vector<std::uint64_t>
    returnCodes(std::shared_ptr<STObject const> const& meta)
    {
        std::vector<std::uint64_t> codes;
        if (meta && meta->isFieldPresent(sfHookExecutions))
            for (auto const& exec : meta->getFieldArray(sfHookExecutions))
                codes.push_back(exec.getFieldU64(sfHookReturnCode));
        return codes;
    }

    static std::optional<STObject>
    commitmentOf(std::shared_ptr<SLE const> const& sle, PublicKey const& pk)
    {
        for (auto const& entry : sle->getFieldArray(sfRandomDigests))
            if (makeSlice(entry.getFieldVL(sfPublicKey)) == pk.slice())
                return entry;
        return std::nullopt;
    }

    // Install a Hook on a fresh account, pay it, and return the Hook's exit
    // codes from the payment's metadata.
    std::vector<std::uint64_t>
    runHook(
        jtx::Env& env,
        jtx::Account const& payer,
        std::vector<std::uint8_t> const& wasm)
    {
        using namespace jtx;
        Account const account{"hook" + std::to_string(++hookAccounts_)};
        env.fund(XRP(10000), account);
        env.close();
        env(hook(account, {{hso(wasm)}}, 0), fee(XRP(100)));
        env.close();
        env(pay(payer, account, XRP(1)), fee(XRP(1)));
        env.close();
        return returnCodes(env.meta());
    }

    void
    testDisabled(FeatureBitset features)
    {
        testcase("Disabled");
        using namespace jtx;

        Env env{*this, features - featureRNG};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // the Hook API functions are not available to SetHook
        env(hook(alice, {{hso(dice6Wasm)}}, 0),
            fee(XRP(100)),
            ter(temMALFORMED));
        env(hook(alice, {{hso(utilRandom32Wasm)}}, 0),
            fee(XRP(100)),
            ter(temMALFORMED));

        // and the pseudo-transaction is rejected
        OpenView view(&*env.closed());
        BEAST_EXPECT(
            applyTx(
                env,
                view,
                entropyTx(
                    view.seq(),
                    randomKeyPair(KeyType::secp256k1).first,
                    sha512Half(secret(1)))) == temDISABLED);
    }

    void
    testTransactor(FeatureBitset features)
    {
        testcase("Entropy transactor");
        using namespace jtx;

        Env env{*this, features};
        auto const pk1 = randomKeyPair(KeyType::secp256k1).first;
        auto const pk2 = randomKeyPair(KeyType::ed25519).first;

        auto l = std::make_shared<Ledger>(
            create_genesis,
            env.app().config(),
            std::vector<uint256>{},
            env.app().getNodeFamily());
        BEAST_EXPECT(l->rules().enabled(featureRNG));
        auto nextLedger = [&]() {
            l = std::make_shared<Ledger>(
                *l, env.app().timeKeeper().closeTime());
        };

        // first contributions only commit
        nextLedger();
        {
            OpenView view(&*l);
            auto const seq = view.seq();
            BEAST_EXPECT(
                applyTx(
                    env, view, entropyTx(seq, pk1, sha512Half(secret(1)))) ==
                tesSUCCESS);
            BEAST_EXPECT(
                applyTx(
                    env, view, entropyTx(seq, pk2, sha512Half(secret(2)))) ==
                tesSUCCESS);

            // one contribution per validator per ledger
            BEAST_EXPECT(
                applyTx(
                    env, view, entropyTx(seq, pk1, sha512Half(secret(3)))) ==
                tefFAILURE);

            // only for the ledger being built
            BEAST_EXPECT(
                applyTx(
                    env,
                    view,
                    entropyTx(seq + 1, pk2, sha512Half(secret(3)))) ==
                tefFAILURE);
            view.apply(*l);
        }

        auto sle = l->read(keylet::random());
        if (!BEAST_EXPECT(sle))
            return;
        BEAST_EXPECT(sle->getFieldU32(sfLedgerSequence) == l->seq());
        BEAST_EXPECT(sle->getFieldU16(sfEntropyCount) == 0);
        BEAST_EXPECT(sle->getFieldU16(sfEntropyDenominator) == 0);
        BEAST_EXPECT(sle->getFieldArray(sfRandomDigests).size() == 2);
        BEAST_EXPECT(
            sle->getFieldH256(sfRandomData) ==
            sha512Half(rngSeedTag, l->seq(), l->info().parentHash));

        // pk1 reveals correctly, pk2 reveals the wrong value; both commit
        nextLedger();
        {
            OpenView view(&*l);
            auto const seq = view.seq();
            auto const res = apply(
                env.app(),
                view,
                entropyTx(seq, pk1, sha512Half(secret(3)), secret(1)),
                tapNONE,
                env.journal);
            BEAST_EXPECT(res.ter == tesSUCCESS);

            // neither the RNG state nor the commitments reach metadata
            if (BEAST_EXPECT(res.metadata))
            {
                auto const meta = res.metadata->getAsObject();
                BEAST_EXPECT(!meta.isFieldPresent(sfRandomData));
                for (auto const& node : meta.getFieldArray(sfAffectedNodes))
                {
                    for (SField const* f :
                         {&sfPreviousFields, &sfFinalFields, &sfNewFields})
                    {
                        if (!node.isFieldPresent(*f))
                            continue;
                        auto const& inner =
                            node.peekAtField(*f).downcast<STObject>();
                        BEAST_EXPECT(!inner.isFieldPresent(sfRandomData));
                        BEAST_EXPECT(!inner.isFieldPresent(sfRandomDigests));
                    }
                }
            }

            BEAST_EXPECT(
                applyTx(
                    env,
                    view,
                    entropyTx(seq, pk2, sha512Half(secret(4)), secret(3))) ==
                tesSUCCESS);
            view.apply(*l);
        }

        sle = l->read(keylet::random());
        BEAST_EXPECT(sle->getFieldU16(sfEntropyCount) == 1);
        BEAST_EXPECT(
            sle->getFieldH256(sfRandomData) ==
            sha512Half(
                rngAccumulateTag,
                sha512Half(rngSeedTag, l->seq(), l->info().parentHash),
                pk1.slice(),
                secret(1)));
        {
            // pk2's reveal was ignored but its new commitment was stored
            auto const c2 = commitmentOf(sle, pk2);
            BEAST_EXPECT(
                c2 &&
                c2->getFieldH256(sfNextRandomDigest) == sha512Half(secret(4)) &&
                c2->getFieldU32(sfLedgerSequence) == l->seq());
        }

        // pk1 withholds, pk2 reveals correctly
        nextLedger();
        {
            OpenView view(&*l);
            BEAST_EXPECT(
                applyTx(
                    env,
                    view,
                    entropyTx(
                        view.seq(), pk2, sha512Half(secret(5)), secret(4))) ==
                tesSUCCESS);
            view.apply(*l);
        }
        sle = l->read(keylet::random());
        BEAST_EXPECT(sle->getFieldU16(sfEntropyCount) == 1);
        BEAST_EXPECT(sle->getFieldU32(sfLedgerSequence) == l->seq());
        auto const pk1Commitment = commitmentOf(sle, pk1);
        if (!BEAST_EXPECT(pk1Commitment))
            return;

        // a commitment that is not refreshed for more than 8 ledgers is dropped
        auto const pk1Seq = pk1Commitment->getFieldU32(sfLedgerSequence);
        for (std::uint32_t s = 5; l->seq() <= pk1Seq + 8; ++s)
        {
            nextLedger();
            OpenView view(&*l);
            BEAST_EXPECT(
                applyTx(
                    env,
                    view,
                    entropyTx(
                        view.seq(),
                        pk2,
                        sha512Half(secret(s + 1)),
                        secret(s))) == tesSUCCESS);
            view.apply(*l);
        }
        sle = l->read(keylet::random());
        BEAST_EXPECT(!commitmentOf(sle, pk1));
        BEAST_EXPECT(commitmentOf(sle, pk2));
        BEAST_EXPECT(sle->getFieldU16(sfEntropyCount) == 1);

        // A change to the Random object (as a Hook draw makes) is left out of
        // provisional metadata, which weak Hooks read through meta_slot, but
        // is in the final metadata.
        {
            OpenView view(&*l);
            ApplyViewImpl avi(&view, tapNONE);
            auto rng = avi.peek(keylet::random());
            if (!BEAST_EXPECT(rng))
                return;
            rng->setFieldH256(
                sfRandomData, step(rng->getFieldH256(sfRandomData)));
            avi.update(rng);

            auto const tx = entropyTx(view.seq(), pk2, sha512Half(secret(99)));
            auto isRandom = [](STObject const& node) {
                return node.getFieldU16(sfLedgerEntryType) == ltRANDOM;
            };

            auto const provisional =
                avi.generateProvisionalMeta(view, tx, env.journal);
            for (auto const& node : provisional.getNodes())
                BEAST_EXPECT(!isRandom(node));

            auto const finalMeta =
                avi.apply(view, tx, tesSUCCESS, false, env.journal);
            if (BEAST_EXPECT(finalMeta))
            {
                bool found = false;
                for (auto const& node : finalMeta->getNodes())
                    found = found || isRandom(node);
                BEAST_EXPECT(found);
            }
        }
    }

    void
    testUNLReport(FeatureBitset features)
    {
        testcase("Only UNLReport validators contribute");
        using namespace jtx;

        Env env{*this, features};
        auto const pk1 = randomKeyPair(KeyType::secp256k1).first;
        auto const pk2 = randomKeyPair(KeyType::secp256k1).first;

        auto l = std::make_shared<Ledger>(
            create_genesis,
            env.app().config(),
            std::vector<uint256>{},
            env.app().getNodeFamily());
        auto nextLedger = [&]() {
            l = std::make_shared<Ledger>(
                *l, env.app().timeKeeper().closeTime());
        };

        // a UNLReport listing only pk1
        nextLedger();
        {
            OpenView view(&*l);
            STTx const report(ttUNL_REPORT, [&](auto& obj) {
                obj.set(([&]() {
                    auto inner = std::make_unique<STObject>(sfActiveValidator);
                    inner->setFieldVL(sfPublicKey, pk1.slice());
                    return inner;
                })());
                obj.setFieldU32(sfLedgerSequence, view.seq());
            });
            BEAST_EXPECT(applyTx(env, view, report) == tesSUCCESS);
            view.apply(*l);
        }
        BEAST_EXPECT(l->read(keylet::UNLReport()));

        nextLedger();
        {
            OpenView view(&*l);
            BEAST_EXPECT(
                applyTx(
                    env,
                    view,
                    entropyTx(view.seq(), pk2, sha512Half(secret(1)))) ==
                tefFAILURE);
            BEAST_EXPECT(
                applyTx(
                    env,
                    view,
                    entropyTx(view.seq(), pk1, sha512Half(secret(2)))) ==
                tesSUCCESS);
            view.apply(*l);
        }

        auto const sle = l->read(keylet::random());
        if (!BEAST_EXPECT(sle))
            return;
        BEAST_EXPECT(sle->getFieldU16(sfEntropyDenominator) == 1);
        BEAST_EXPECT(sle->getFieldArray(sfRandomDigests).size() == 1);
        BEAST_EXPECT(commitmentOf(sle, pk1));
    }

    void
    testNoEntropy(FeatureBitset features)
    {
        testcase("No entropy" + suffix(features));
        using namespace jtx;

        // not a validator, so nothing ever contributes
        Env env{*this, features};
        Account const bob{"bob"};
        env.fund(XRP(10000), bob);
        env.close();

        BEAST_EXPECT(
            runHook(env, bob, dice6Wasm) ==
            std::vector<std::uint64_t>{
                rc(hook_api::hook_return_code::TOO_LITTLE_ENTROPY)});
        BEAST_EXPECT(!env.le(keylet::random()));
        BEAST_EXPECT(!env.meta()->isFieldPresent(sfRandomData));
    }

    void
    testHooks(FeatureBitset features)
    {
        testcase("Hook API" + suffix(features));
        using namespace jtx;
        using enum hook_api::hook_return_code;

        // in standalone mode a validator contributes to every ledger it closes
        Env env{*this, envconfig(validator, ""), features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();
        env.close();

        {
            auto const sle = env.closed()->read(keylet::random());
            if (!BEAST_EXPECT(sle))
                return;
            BEAST_EXPECT(
                sle->getFieldU32(sfLedgerSequence) == env.closed()->seq());
            BEAST_EXPECT(sle->getFieldU16(sfEntropyCount) == 1);
            BEAST_EXPECT(sle->getFieldU16(sfEntropyDenominator) == 0);
        }

        // Two Hooks on alice, each drawing once. bob pays alice twice in one
        // ledger.
        env(hook(alice, {{hso(diceMaxWasm), hso(diceMaxWasm)}}, 0),
            fee(XRP(100)));
        env.close();

        env(pay(bob, alice, XRP(1)), fee(XRP(1)));
        auto const tx1 = env.tx()->getTransactionID();
        env(pay(bob, alice, XRP(1)), fee(XRP(1)));
        auto const tx2 = env.tx()->getTransactionID();
        env.close();

        auto const ledger = env.closed();
        auto const meta1 = ledger->txRead(tx1).second;
        auto const meta2 = ledger->txRead(tx2).second;
        if (!BEAST_EXPECT(meta1 && meta2))
            return;

        // the entropy contribution is applied before anything else
        {
            bool sawEntropy = false;
            for (auto const& [tx, meta] : ledger->txs)
            {
                if (tx->getTxnType() != ttENTROPY)
                    continue;
                sawEntropy = true;
                BEAST_EXPECT(meta->getFieldU32(sfTransactionIndex) == 0);
            }
            BEAST_EXPECT(sawEntropy);
        }

        auto const codes1 = returnCodes(meta1);
        auto const codes2 = returnCodes(meta2);
        if (!BEAST_EXPECT(codes1.size() == 2 && codes2.size() == 2))
            return;
        for (auto const c : {codes1[0], codes1[1], codes2[0], codes2[1]})
            BEAST_EXPECT(c < 0xFFFFFFFFULL);
        BEAST_EXPECT(codes1[0] != codes1[1]);
        BEAST_EXPECT(codes1 != codes2);

        // Each transaction records the state its first draw saw, and the
        // state advances once per draw: across both of tx1's Hook executions,
        // into tx2, and through to the end of the ledger.
        if (BEAST_EXPECT(
                meta1->isFieldPresent(sfRandomData) &&
                meta2->isFieldPresent(sfRandomData)))
        {
            auto const s1 = meta1->getFieldH256(sfRandomData);
            auto const s2 = meta2->getFieldH256(sfRandomData);
            BEAST_EXPECT(s2 == step(step(s1)));

            auto const sle = ledger->read(keylet::random());
            BEAST_EXPECT(
                sle && sle->getFieldH256(sfRandomData) == step(step(s2)));
        }

        // A transaction that ends in a tec keeps its draws: the next draw
        // carries on from where the rolled-back Hook left the state.
        {
            Account const roller{"roller"};
            Account const drawer{"drawer"};
            env.fund(XRP(10000), roller, drawer);
            env.close();
            env(hook(roller, {{hso(diceRollbackWasm)}}, 0), fee(XRP(100)));
            env(hook(drawer, {{hso(diceMaxWasm)}}, 0), fee(XRP(100)));
            env.close();

            env(pay(bob, roller, XRP(1)), fee(XRP(1)), ter(tecHOOK_REJECTED));
            auto const txTec = env.tx()->getTransactionID();
            env(pay(bob, drawer, XRP(1)), fee(XRP(1)));
            auto const txNext = env.tx()->getTransactionID();
            env.close();

            auto const lgr = env.closed();
            auto const metaTec = lgr->txRead(txTec).second;
            auto const metaNext = lgr->txRead(txNext).second;
            if (BEAST_EXPECT(
                    metaTec && metaNext &&
                    metaTec->isFieldPresent(sfRandomData) &&
                    metaNext->isFieldPresent(sfRandomData)))
            {
                BEAST_EXPECT(
                    metaTec->getFieldU8(sfTransactionResult) ==
                    TERtoInt(tecHOOK_REJECTED));
                auto const sTec = metaTec->getFieldH256(sfRandomData);
                BEAST_EXPECT(
                    metaNext->getFieldH256(sfRandomData) == step(sTec));

                auto const rng = lgr->read(keylet::random());
                BEAST_EXPECT(
                    rng && rng->getFieldH256(sfRandomData) == step(step(sTec)));
            }
        }

        // the individual APIs
        {
            auto const codes = runHook(env, bob, dice6Wasm);
            BEAST_EXPECT(codes.size() == 1 && codes[0] < 6);
        }
        BEAST_EXPECT(
            runHook(env, bob, dice0Wasm) ==
            std::vector<std::uint64_t>{rc(INVALID_ARGUMENT)});
        BEAST_EXPECT(
            runHook(env, bob, diceTwiceWasm) == std::vector<std::uint64_t>{0});
        BEAST_EXPECT(
            runHook(env, bob, utilRandom32Wasm) ==
            std::vector<std::uint64_t>{32});
        BEAST_EXPECT(
            runHook(env, bob, utilRandom0Wasm) ==
            std::vector<std::uint64_t>{rc(TOO_SMALL)});
        BEAST_EXPECT(
            runHook(env, bob, utilRandom513Wasm) ==
            std::vector<std::uint64_t>{rc(TOO_BIG)});
    }

    void
    testSlotBlocked(FeatureBitset features)
    {
        testcase("Hooks cannot read the RNG" + suffix(features));
        using namespace jtx;
        using enum hook_api::hook_return_code;

        // the key baked into the slot_set test Hooks
        BEAST_EXPECT(
            to_string(keylet::random().key) ==
            "125C53F238F6CA65C41A3287F760CCF04F2D0407A8FE016B59F460FE14F0C677");

        Env env{*this, envconfig(validator, ""), features};
        Account const bob{"bob"};
        env.fund(XRP(10000), bob);
        env.close();
        env.close();
        BEAST_EXPECT(env.le(keylet::random()));

        // by its own type, and by ltANY, which would otherwise match anything
        BEAST_EXPECT(
            runHook(env, bob, slotRandomWasm) ==
            std::vector<std::uint64_t>{rc(DOESNT_EXIST)});
        BEAST_EXPECT(
            runHook(env, bob, slotAnyWasm) ==
            std::vector<std::uint64_t>{rc(DOESNT_EXIST)});

        // nor can ledger_keylet find it, over a range holding only its key
        BEAST_EXPECT(
            runHook(env, bob, ledgerKeyletWasm) ==
            std::vector<std::uint64_t>{rc(DOESNT_EXIST)});
    }

    void
    testLedgerEntry(FeatureBitset features)
    {
        testcase("ledger_entry random" + suffix(features));
        using namespace jtx;

        Env env{*this, envconfig(validator, ""), features};
        env.close();
        env.close();
        auto const sle = env.le(keylet::random());
        if (!BEAST_EXPECT(sle))
            return;

        auto const index = to_string(keylet::random().key);
        auto const request = [&](Json::Value const& random) {
            Json::Value params;
            params[jss::random] = random;
            return env.rpc(
                "json", "ledger_entry", to_string(params))[jss::result];
        };

        // the singleton, by flag
        {
            auto const jrr = request(true);
            BEAST_EXPECT(jrr[jss::index] == index);
            BEAST_EXPECT(
                jrr[jss::node][sfLedgerEntryType.jsonName] == jss::Random);
            BEAST_EXPECT(
                jrr[jss::node][sfLedgerSequence.jsonName].asUInt() ==
                sle->getFieldU32(sfLedgerSequence));
            BEAST_EXPECT(
                jrr[jss::node][sfRandomData.jsonName] ==
                to_string(sle->getFieldH256(sfRandomData)));
            BEAST_EXPECT(jrr[jss::node][sfEntropyCount.jsonName].asUInt() == 1);
            BEAST_EXPECT(jrr[jss::node].isMember(sfRandomDigests.jsonName));

            // threaded to the last transaction that touched it: the ledger's
            // last entropy contribution, since nothing drew in it
            BEAST_EXPECT(
                jrr[jss::node][sfPreviousTxnLgrSeq.jsonName].asUInt() ==
                sle->getFieldU32(sfLedgerSequence));
            auto const prevTxnID = sle->getFieldH256(sfPreviousTxnID);
            BEAST_EXPECT(
                jrr[jss::node][sfPreviousTxnID.jsonName] ==
                to_string(prevTxnID));
            auto const [prevTxn, prevMeta] = env.closed()->txRead(prevTxnID);
            BEAST_EXPECT(prevTxn && prevTxn->getTxnType() == ttENTROPY);
        }

        // and by its index
        {
            auto const jrr = request(index);
            BEAST_EXPECT(jrr[jss::index] == index);
            BEAST_EXPECT(
                jrr[jss::node][sfLedgerEntryType.jsonName] == jss::Random);
        }

        // anything else is malformed
        BEAST_EXPECT(request(false)[jss::error] == "malformedRequest");
        BEAST_EXPECT(request(7)[jss::error] == "malformedRequest");
        BEAST_EXPECT(request("nothex")[jss::error] == "malformedRequest");

        // ledger_data sees it as type "random"
        {
            Json::Value params;
            params[jss::type] = jss::random;
            auto const jrr =
                env.rpc("json", "ledger_data", to_string(params))[jss::result];
            BEAST_EXPECT(
                jrr[jss::state].size() == 1 &&
                jrr[jss::state][0u][sfLedgerEntryType.jsonName] == jss::Random);
        }
    }

    // Individual form is for ledgers without featureRNGV2, aggregate form for
    // ledgers with it, and nothing else is well-formed.
    void
    testPreflightV2()
    {
        testcase("Entropy preflight forms");
        using namespace jtx;

        auto const pk1 = randomKeyPair(KeyType::secp256k1).first;
        auto const pk2 = randomKeyPair(KeyType::secp256k1).first;
        auto const [lo, hi] = blobOf(pk1) < blobOf(pk2) ? std::pair{pk1, pk2}
                                                        : std::pair{pk2, pk1};
        auto const d = sha512Half(secret(1));

        {
            Env env{*this, supported_amendments()};
            OpenView view(&*env.closed());
            auto const seq = view.seq();
            auto const ter = [&](STTx const& tx) {
                return applyTx(env, view, tx);
            };

            // individual form is never applicable under V2
            BEAST_EXPECT(ter(entropyTx(seq, lo, d)) == temMALFORMED);

            // empty
            BEAST_EXPECT(ter(aggregateTx(seq, {})) == temMALFORMED);

            // unsorted
            BEAST_EXPECT(
                ter(aggregateTx(
                    seq, {{blobOf(hi), d, {}, {}}, {blobOf(lo), d, {}, {}}})) ==
                temMALFORMED);

            // duplicate key
            BEAST_EXPECT(
                ter(aggregateTx(
                    seq, {{blobOf(lo), d, {}, {}}, {blobOf(lo), d, {}, {}}})) ==
                temMALFORMED);

            // an entry with sfLedgerSequence
            BEAST_EXPECT(
                ter(aggregateTx(seq, {{blobOf(lo), d, {}, seq}})) ==
                temMALFORMED);

            // bad key
            BEAST_EXPECT(
                ter(aggregateTx(seq, {{Blob(33, 0x01), d, {}, {}}})) ==
                temMALFORMED);

            // top-level key, digest or reveal alongside the array
            BEAST_EXPECT(
                ter(aggregateTx(
                    seq, {{blobOf(lo), d, {}, {}}}, [&](STObject& obj) {
                        obj.setFieldVL(sfPublicKey, lo.slice());
                    })) == temMALFORMED);
            BEAST_EXPECT(
                ter(aggregateTx(
                    seq, {{blobOf(lo), d, {}, {}}}, [&](STObject& obj) {
                        obj.setFieldH256(sfNextRandomDigest, d);
                    })) == temMALFORMED);
            BEAST_EXPECT(
                ter(aggregateTx(
                    seq, {{blobOf(lo), d, {}, {}}}, [&](STObject& obj) {
                        obj.setFieldH256(sfRandomData, d);
                    })) == temMALFORMED);
        }

        {
            // and the aggregate form is malformed without V2
            Env env{*this, supported_amendments() - featureRNGV2};
            OpenView view(&*env.closed());
            BEAST_EXPECT(
                applyTx(
                    env,
                    view,
                    aggregateTx(view.seq(), {{blobOf(lo), d, {}, {}}})) ==
                temMALFORMED);

            // individual form without the digest
            BEAST_EXPECT(applyTx(env, view, STTx(ttENTROPY, [&](auto& obj) {
                                     obj.setFieldU32(
                                         sfLedgerSequence, view.seq());
                                     obj.setFieldVL(sfPublicKey, lo.slice());
                                 })) == temMALFORMED);
        }
    }

    // featureRNGV2 must not weaken the randomness: the same contributions
    // give the same Random object whether applied one transaction per
    // validator or as one aggregate.
    void
    testEquivalence()
    {
        testcase("Aggregate equals individual contributions");
        using namespace jtx;

        // One Env and chain per rule set (Rules refer to the live config, so
        // they cannot differ within one Env), each built from genesis.
        // Neither ledger is made immutable, so the parent hashes are
        // identical by construction (asserted below) and the seeds match; the
        // expected value is also recomputed from the formula for both.
        Env env1{*this, supported_amendments() - featureRNGV2};
        Env env2{*this, supported_amendments()};
        auto genesis = [&](Env& env) {
            return std::make_shared<Ledger>(
                create_genesis,
                env.app().config(),
                std::vector<uint256>{},
                env.app().getNodeFamily());
        };
        auto v1 = genesis(env1);
        auto v2 = genesis(env2);
        BEAST_EXPECT(!v1->rules().enabled(featureRNGV2));
        BEAST_EXPECT(v2->rules().enabled(featureRNGV2));

        std::vector<PublicKey> keys;
        for (int i = 0; i < 3; ++i)
            keys.push_back(randomKeyPair(KeyType::secp256k1).first);

        struct Step
        {
            int validator;
            std::optional<std::uint32_t> reveal;  // secret() index
            std::uint32_t next;                   // secret() index
            bool valid;                           // the reveal is mixed in
        };
        // Ledger 1 commits; 2 has a correct reveal, a withheld reveal and a
        // wrong one; 3 reveals for everything just committed; 4 has one
        // validator absent.
        std::vector<std::vector<Step>> const ledgers{
            {{0, {}, 10, false}, {1, {}, 11, false}, {2, {}, 12, false}},
            {{0, 10, 20, true}, {1, {}, 21, false}, {2, 999, 22, false}},
            {{0, 20, 30, true}, {1, 21, 31, true}, {2, 22, 32, true}},
            {{1, 31, 41, true}, {2, 32, 42, true}},
        };
        std::vector<std::uint16_t> const counts{0, 1, 3, 2};

        auto next = [&](Env& env, std::shared_ptr<Ledger>& l) {
            l = std::make_shared<Ledger>(
                *l, env.app().timeKeeper().closeTime());
        };

        for (std::size_t n = 0; n < ledgers.size(); ++n)
        {
            next(env1, v1);
            next(env2, v2);
            BEAST_EXPECT(v1->info().parentHash == v2->info().parentHash);

            // ascending public key order
            auto steps = ledgers[n];
            std::sort(steps.begin(), steps.end(), [&](auto& a, auto& b) {
                return blobOf(keys[a.validator]) < blobOf(keys[b.validator]);
            });

            {
                OpenView view(&*v1);
                for (auto const& st : steps)
                    BEAST_EXPECT(
                        applyTx(
                            env1,
                            view,
                            entropyTx(
                                view.seq(),
                                keys[st.validator],
                                sha512Half(secret(st.next)),
                                st.reveal ? std::optional{secret(*st.reveal)}
                                          : std::nullopt)) == tesSUCCESS);
                view.apply(*v1);
            }
            {
                OpenView view(&*v2);
                std::vector<Contribution> cs;
                for (auto const& st : steps)
                    cs.push_back(
                        {blobOf(keys[st.validator]),
                         sha512Half(secret(st.next)),
                         st.reveal ? std::optional{secret(*st.reveal)}
                                   : std::nullopt,
                         {}});
                BEAST_EXPECT(
                    applyTx(env2, view, aggregateTx(view.seq(), cs)) ==
                    tesSUCCESS);
                view.apply(*v2);
            }

            // expected state from the formula
            auto expected =
                sha512Half(rngSeedTag, v1->seq(), v1->info().parentHash);
            for (auto const& st : steps)
                if (st.valid)
                    expected = sha512Half(
                        rngAccumulateTag,
                        expected,
                        keys[st.validator].slice(),
                        secret(*st.reveal));

            auto const a = v1->read(keylet::random());
            auto const b = v2->read(keylet::random());
            if (!BEAST_EXPECT(a && b))
                return;
            BEAST_EXPECT(a->getFieldH256(sfRandomData) == expected);
            BEAST_EXPECT(b->getFieldH256(sfRandomData) == expected);
            BEAST_EXPECT(a->getFieldU16(sfEntropyCount) == counts[n]);
            BEAST_EXPECT(b->getFieldU16(sfEntropyCount) == counts[n]);
            BEAST_EXPECT(
                a->getFieldU16(sfEntropyDenominator) ==
                b->getFieldU16(sfEntropyDenominator));
            BEAST_EXPECT(
                a->getFieldU32(sfLedgerSequence) ==
                b->getFieldU32(sfLedgerSequence));

            auto const& da = a->getFieldArray(sfRandomDigests);
            auto const& db = b->getFieldArray(sfRandomDigests);
            if (BEAST_EXPECT(da.size() == db.size()))
                for (std::size_t i = 0; i < da.size(); ++i)
                {
                    BEAST_EXPECT(
                        da[i].getFieldVL(sfPublicKey) ==
                        db[i].getFieldVL(sfPublicKey));
                    BEAST_EXPECT(
                        da[i].getFieldH256(sfNextRandomDigest) ==
                        db[i].getFieldH256(sfNextRandomDigest));
                    BEAST_EXPECT(
                        da[i].getFieldU32(sfLedgerSequence) ==
                        db[i].getFieldU32(sfLedgerSequence));
                }
        }
    }

    // The ledger-build path collapses the consensus set's individual-form
    // items into one aggregate (V2), or leaves them as they are (V1).
    void
    testBuildLedger(FeatureBitset features)
    {
        testcase("Build ledger" + suffix(features));
        using namespace jtx;
        bool const v2 = features[featureRNGV2];

        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();

        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto const seq = parent->seq() + 1;

        std::vector<PublicKey> keys;
        for (int i = 0; i < 5; ++i)
            keys.push_back(randomKeyPair(KeyType::secp256k1).first);
        auto const& [kA, kB, kC, kD, kE] =
            std::tie(keys[0], keys[1], keys[2], keys[3], keys[4]);

        CanonicalTXSet txns(parent->info().hash);
        auto add = [&](STTx const& tx) {
            txns.insert(std::make_shared<STTx const>(tx));
            return tx.getTransactionID();
        };

        auto const revealA = secret(500);
        auto const a1 = entropyTx(seq, kA, sha512Half(secret(1)), revealA);
        auto const a2 = entropyTx(seq, kA, sha512Half(secret(2)));
        auto const idA1 = add(a1);
        auto const idA2 = add(a2);  // duplicate key
        auto const idB = add(entropyTx(seq, kB, sha512Half(secret(3))));
        auto const idC = add(entropyTx(seq, kC, sha512Half(secret(4))));
        auto const idWrongSeq =
            add(entropyTx(seq + 1, kD, sha512Half(secret(5))));
        auto const idBadKey = add(STTx(ttENTROPY, [&](auto& obj) {
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.setFieldVL(sfPublicKey, Blob(33, 0x01));
            obj.setFieldH256(sfNextRandomDigest, sha512Half(secret(6)));
        }));
        auto const idInjected = add(
            aggregateTx(seq, {{blobOf(kE), sha512Half(secret(7)), {}, {}}}));

        auto const payJt = env.jt(jtx::pay(alice, bob, XRP(1)));
        auto const idPay = payJt.stx->getTransactionID();
        txns.insert(std::make_shared<STTx const>(*payJt.stx));

        // the first of the duplicates in canonical (txid) order wins
        auto const winnerA = idA1 < idA2 ? a1 : a2;
        auto const loserA = idA1 < idA2 ? idA2 : idA1;

        std::set<TxID> failed;
        auto const built = buildLedger(
            parent,
            env.app().timeKeeper().closeTime(),
            true,
            parent->info().closeTimeResolution,
            env.app(),
            txns,
            failed,
            env.journal);

        BEAST_EXPECT(built->txExists(idPay));
        BEAST_EXPECT(!failed.count(idPay));
        for (auto const& [key, tx] : txns)
            BEAST_EXPECT(tx->getTxnType() != ttENTROPY);

        std::vector<std::shared_ptr<STTx const>> entropy;
        for (auto const& [tx, meta] : built->txs)
            if (tx->getTxnType() == ttENTROPY)
                entropy.push_back(tx);

        auto const rng = built->read(keylet::random());
        if (!BEAST_EXPECT(rng))
            return;
        BEAST_EXPECT(rng->getFieldU32(sfLedgerSequence) == built->seq());
        BEAST_EXPECT(rng->getFieldU16(sfEntropyCount) == 0);
        BEAST_EXPECT(rng->getFieldArray(sfRandomDigests).size() == 3);

        if (!v2)
        {
            // unchanged: every valid individual item is recorded
            BEAST_EXPECT(entropy.size() == 3);
            for (auto const& tx : entropy)
                BEAST_EXPECT(!tx->isFieldPresent(sfRandomDigests));
            BEAST_EXPECT(failed.count(loserA));
            BEAST_EXPECT(failed.count(idWrongSeq));
            BEAST_EXPECT(failed.count(idBadKey));
            BEAST_EXPECT(failed.count(idInjected));
            BEAST_EXPECT(!failed.count(idB) && !failed.count(idC));
            return;
        }

        if (!BEAST_EXPECT(entropy.size() == 1))
            return;
        auto const& agg = *entropy[0];
        BEAST_EXPECT(agg.isFieldPresent(sfRandomDigests));
        BEAST_EXPECT(!agg.isFieldPresent(sfPublicKey));
        BEAST_EXPECT(agg.getFieldU32(sfLedgerSequence) == built->seq());

        std::vector<std::pair<Blob, uint256>> expected{
            {blobOf(kA), winnerA.getFieldH256(sfNextRandomDigest)},
            {blobOf(kB), sha512Half(secret(3))},
            {blobOf(kC), sha512Half(secret(4))}};
        std::sort(expected.begin(), expected.end());
        auto const& entries = agg.getFieldArray(sfRandomDigests);
        if (BEAST_EXPECT(entries.size() == expected.size()))
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                BEAST_EXPECT(
                    entries[i].getFieldVL(sfPublicKey) == expected[i].first);
                BEAST_EXPECT(
                    entries[i].getFieldH256(sfNextRandomDigest) ==
                    expected[i].second);
                BEAST_EXPECT(!entries[i].isFieldPresent(sfLedgerSequence));
                // the reveal is carried through
                BEAST_EXPECT(
                    entries[i].isFieldPresent(sfRandomData) ==
                    (entries[i].getFieldVL(sfPublicKey) == blobOf(kA) &&
                     winnerA.isFieldPresent(sfRandomData)));
            }

        BEAST_EXPECT(failed.count(loserA));
        BEAST_EXPECT(failed.count(idWrongSeq));
        BEAST_EXPECT(failed.count(idBadKey));
        BEAST_EXPECT(failed.count(idInjected));
        BEAST_EXPECT(!failed.count(idA1 < idA2 ? idA1 : idA2));
        BEAST_EXPECT(!failed.count(idB) && !failed.count(idC));
        BEAST_EXPECT(failed.size() == 4);
    }

    // A validator Env closing ledgers through consensus records one
    // ttENTROPY per ledger, in aggregate form under V2.
    void
    testValidatorClose(FeatureBitset features)
    {
        testcase("Validator close" + suffix(features));
        using namespace jtx;
        bool const v2 = features[featureRNGV2];

        Env env{*this, envconfig(validator, ""), features};
        env.close();
        env.close();

        std::vector<std::shared_ptr<STTx const>> entropy;
        for (auto const& [tx, meta] : env.closed()->txs)
            if (tx->getTxnType() == ttENTROPY)
                entropy.push_back(tx);
        if (!BEAST_EXPECT(entropy.size() == 1))
            return;
        BEAST_EXPECT(entropy[0]->isFieldPresent(sfRandomDigests) == v2);
        BEAST_EXPECT(entropy[0]->isFieldPresent(sfPublicKey) == !v2);
        if (v2)
            BEAST_EXPECT(
                entropy[0]->getFieldArray(sfRandomDigests).size() == 1);

        auto const sle = env.closed()->read(keylet::random());
        if (BEAST_EXPECT(sle))
            BEAST_EXPECT(sle->getFieldU16(sfEntropyCount) == 1);
    }

    // Under V2: a second aggregate in one ledger, and a key that is not on
    // the UNLReport, both fail.
    void
    testAggregateRejections()
    {
        testcase("Aggregate rejections");
        using namespace jtx;

        Env env{*this, supported_amendments()};
        auto const pk1 = randomKeyPair(KeyType::secp256k1).first;
        auto const pk2 = randomKeyPair(KeyType::secp256k1).first;
        auto const d = sha512Half(secret(1));

        auto l = std::make_shared<Ledger>(
            create_genesis,
            env.app().config(),
            std::vector<uint256>{},
            env.app().getNodeFamily());
        BEAST_EXPECT(l->rules().enabled(featureRNGV2));
        auto nextLedger = [&]() {
            l = std::make_shared<Ledger>(
                *l, env.app().timeKeeper().closeTime());
        };

        nextLedger();
        {
            OpenView view(&*l);
            auto const seq = view.seq();
            BEAST_EXPECT(
                applyTx(
                    env, view, aggregateTx(seq, {{blobOf(pk1), d, {}, {}}})) ==
                tesSUCCESS);

            // a second aggregate in the same ledger, even for another key
            BEAST_EXPECT(
                applyTx(
                    env, view, aggregateTx(seq, {{blobOf(pk2), d, {}, {}}})) ==
                tefFAILURE);

            // and one for the wrong ledger
            BEAST_EXPECT(
                applyTx(
                    env,
                    view,
                    aggregateTx(seq + 1, {{blobOf(pk2), d, {}, {}}})) ==
                tefFAILURE);
            view.apply(*l);
        }

        // a UNLReport listing only pk1
        nextLedger();
        {
            OpenView view(&*l);
            STTx const report(ttUNL_REPORT, [&](auto& obj) {
                auto inner = std::make_unique<STObject>(sfActiveValidator);
                inner->setFieldVL(sfPublicKey, pk1.slice());
                obj.set(std::move(inner));
                obj.setFieldU32(sfLedgerSequence, view.seq());
            });
            BEAST_EXPECT(applyTx(env, view, report) == tesSUCCESS);
            view.apply(*l);
        }

        nextLedger();
        {
            OpenView view(&*l);
            auto const seq = view.seq();
            auto const [lo, hi] = blobOf(pk1) < blobOf(pk2)
                ? std::pair{pk1, pk2}
                : std::pair{pk2, pk1};

            // one key off the UNLReport fails the whole aggregate
            BEAST_EXPECT(
                applyTx(
                    env,
                    view,
                    aggregateTx(
                        seq,
                        {{blobOf(lo), d, {}, {}}, {blobOf(hi), d, {}, {}}})) ==
                tefFAILURE);
            BEAST_EXPECT(
                !view.read(keylet::random()) ||
                view.read(keylet::random())->getFieldU32(sfLedgerSequence) !=
                    seq);

            BEAST_EXPECT(
                applyTx(
                    env, view, aggregateTx(seq, {{blobOf(pk1), d, {}, {}}})) ==
                tesSUCCESS);
            view.apply(*l);
        }
        auto const sle = l->read(keylet::random());
        if (BEAST_EXPECT(sle))
        {
            BEAST_EXPECT(sle->getFieldU16(sfEntropyDenominator) == 1);
            BEAST_EXPECT(sle->getFieldArray(sfRandomDigests).size() == 1);
        }
    }

    // Build the next ledger on parent from a set of consensus items.
    std::shared_ptr<Ledger>
    buildOn(
        jtx::Env& env,
        std::shared_ptr<Ledger const> const& parent,
        std::vector<STTx> const& items,
        std::set<TxID>& failed)
    {
        CanonicalTXSet txns(parent->info().hash);
        for (auto const& tx : items)
            txns.insert(std::make_shared<STTx const>(tx));
        auto const res = parent->info().closeTimeResolution;
        auto built = buildLedger(
            parent,
            parent->info().closeTime + res,
            true,
            res,
            env.app(),
            txns,
            failed,
            env.journal);
        for (auto const& [key, tx] : txns)
            BEAST_EXPECT(tx->getTxnType() != ttENTROPY);
        return built;
    }

    static std::vector<std::shared_ptr<STTx const>>
    entropiesOf(std::shared_ptr<Ledger const> const& l)
    {
        std::vector<std::shared_ptr<STTx const>> out;
        for (auto const& [tx, meta] : l->txs)
            if (tx->getTxnType() == ttENTROPY)
                out.push_back(tx);
        return out;
    }

    static STTx
    unlReportTx(PublicKey const& pk, std::uint32_t seq)
    {
        return STTx(ttUNL_REPORT, [&](auto& obj) {
            auto inner = std::make_unique<STObject>(sfActiveValidator);
            inner->setFieldVL(sfPublicKey, pk.slice());
            obj.set(std::move(inner));
            obj.setFieldU32(sfLedgerSequence, seq);
        });
    }

    // Only the exact bytes a validator builds are accepted into the
    // aggregate.
    void
    testNonCanonical()
    {
        testcase("Non-canonical entropy items");
        using namespace jtx;

        Env env{*this, supported_amendments()};
        env.close();
        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto const seq = parent->seq() + 1;
        auto const kX = randomKeyPair(KeyType::secp256k1).first;
        auto const kY = randomKeyPair(KeyType::secp256k1).first;
        auto const kZ = randomKeyPair(KeyType::secp256k1).first;

        // same four fields plus one more, so a different txid
        auto extra = [&](PublicKey const& pk, uint256 const& d) {
            return STTx(ttENTROPY, [&](auto& obj) {
                obj.setFieldU32(sfLedgerSequence, seq);
                obj.setFieldVL(sfPublicKey, pk.slice());
                obj.setFieldH256(sfNextRandomDigest, d);
                obj.setFieldU32(sfSourceTag, 7);
            });
        };

        // vary the digests so the canonical order of the items varies
        for (std::uint32_t i = 0; i < 8; ++i)
        {
            auto const dX = sha512Half(secret(100 + i));
            auto const dY = sha512Half(secret(200 + i));
            auto const dZ = sha512Half(secret(300 + i));
            auto const xOnly = extra(kX, dX);
            auto const yBad = extra(kY, dY);
            auto const yGood = entropyTx(seq, kY, dY);
            auto const zGood = entropyTx(seq, kZ, dZ);
            BEAST_EXPECT(yBad.getTransactionID() != yGood.getTransactionID());

            std::set<TxID> failed;
            auto const built =
                buildOn(env, parent, {xOnly, yBad, yGood, zGood}, failed);
            auto const entropy = entropiesOf(built);
            if (!BEAST_EXPECT(entropy.size() == 1))
                return;
            auto const& entries = entropy[0]->getFieldArray(sfRandomDigests);
            BEAST_EXPECT(entries.size() == 2);
            for (auto const& e : entries)
            {
                auto const pk = e.getFieldVL(sfPublicKey);
                BEAST_EXPECT(pk != blobOf(kX));
                if (pk == blobOf(kY))
                    BEAST_EXPECT(e.getFieldH256(sfNextRandomDigest) == dY);
            }
            BEAST_EXPECT(failed.count(xOnly.getTransactionID()));
            BEAST_EXPECT(failed.count(yBad.getTransactionID()));
            BEAST_EXPECT(!failed.count(yGood.getTransactionID()));
            BEAST_EXPECT(!failed.count(zGood.getTransactionID()));
            BEAST_EXPECT(failed.size() == 2);
        }
    }

    // Reveals survive the collapse, and a built aggregate replays to the same
    // ledger.
    void
    testRevealAndReplay()
    {
        testcase("Reveal through collapse and replay");
        using namespace jtx;

        Env env{*this, supported_amendments()};
        env.close();
        auto const parent = env.app().getLedgerMaster().getClosedLedger();

        std::vector<PublicKey> keys;
        for (int i = 0; i < 4; ++i)
            keys.push_back(randomKeyPair(KeyType::secp256k1).first);

        // ledger N: everyone commits
        std::set<TxID> failed1;
        std::vector<STTx> items1;
        for (std::uint32_t i = 0; i < keys.size(); ++i)
            items1.push_back(entropyTx(
                parent->seq() + 1, keys[i], sha512Half(secret(10 + i))));
        auto const l1 = buildOn(env, parent, items1, failed1);
        BEAST_EXPECT(failed1.empty());
        BEAST_EXPECT(entropiesOf(l1).size() == 1);

        // ledger N+1: 0 reveals, 1 withholds, 2 reveals wrong, 3 reveals
        auto const seq2 = l1->seq() + 1;
        std::vector<std::optional<uint256>> reveals{
            secret(10), std::nullopt, secret(999), secret(13)};
        std::vector<STTx> items2;
        for (std::uint32_t i = 0; i < keys.size(); ++i)
            items2.push_back(entropyTx(
                seq2, keys[i], sha512Half(secret(20 + i)), reveals[i]));
        std::set<TxID> failed2;
        auto const l2 = buildOn(env, l1, items2, failed2);
        BEAST_EXPECT(failed2.empty());

        auto const entropy = entropiesOf(l2);
        if (!BEAST_EXPECT(entropy.size() == 1))
            return;

        // entries carry the exact reveals, sorted by key
        auto const& entries = entropy[0]->getFieldArray(sfRandomDigests);
        if (!BEAST_EXPECT(entries.size() == keys.size()))
            return;
        std::vector<std::uint32_t> order{0, 1, 2, 3};
        std::sort(order.begin(), order.end(), [&](auto a, auto b) {
            return blobOf(keys[a]) < blobOf(keys[b]);
        });
        auto expected =
            sha512Half(rngSeedTag, l2->seq(), l2->info().parentHash);
        for (std::size_t n = 0; n < order.size(); ++n)
        {
            auto const i = order[n];
            auto const& e = entries[n];
            BEAST_EXPECT(e.getFieldVL(sfPublicKey) == blobOf(keys[i]));
            BEAST_EXPECT(
                e.getFieldH256(sfNextRandomDigest) ==
                sha512Half(secret(20 + i)));
            BEAST_EXPECT(
                e.isFieldPresent(sfRandomData) == reveals[i].has_value());
            if (reveals[i])
            {
                BEAST_EXPECT(e.getFieldH256(sfRandomData) == *reveals[i]);
                if (sha512Half(*reveals[i]) == sha512Half(secret(10 + i)))
                    expected = sha512Half(
                        rngAccumulateTag,
                        expected,
                        keys[i].slice(),
                        *reveals[i]);
            }
        }

        auto const rng = l2->read(keylet::random());
        if (!BEAST_EXPECT(rng))
            return;
        BEAST_EXPECT(rng->getFieldU16(sfEntropyCount) == 2);
        BEAST_EXPECT(rng->getFieldH256(sfRandomData) == expected);

        // replaying either ledger gives the same ledger
        for (auto const& [par, orig] :
             {std::pair<
                  std::shared_ptr<Ledger const>,
                  std::shared_ptr<Ledger const>>{parent, l1},
              std::pair<
                  std::shared_ptr<Ledger const>,
                  std::shared_ptr<Ledger const>>{l1, l2}})
        {
            auto const replayed = buildLedger(
                LedgerReplay(par, orig), tapNONE, env.app(), env.journal);
            BEAST_EXPECT(replayed->info().hash == orig->info().hash);
        }
    }

    // Keys off the UNLReport are dropped, and the denominator is its size.
    void
    testUNLReportCollapse()
    {
        testcase("UNLReport filter through collapse");
        using namespace jtx;

        Env env{*this, supported_amendments()};
        env.close();
        auto const parent = env.app().getLedgerMaster().getClosedLedger();

        std::vector<PublicKey> listed;
        for (int i = 0; i < 3; ++i)
            listed.push_back(randomKeyPair(KeyType::secp256k1).first);
        auto const off = randomKeyPair(KeyType::secp256k1).first;

        std::vector<STTx> reports;
        for (auto const& pk : listed)
            reports.push_back(unlReportTx(pk, parent->seq() + 1));
        std::set<TxID> failed1;
        auto const l1 = buildOn(env, parent, reports, failed1);
        BEAST_EXPECT(failed1.empty());
        auto const rep = l1->read(keylet::UNLReport());
        if (!BEAST_EXPECT(rep))
            return;
        BEAST_EXPECT(rep->getFieldArray(sfActiveValidators).size() == 3);

        auto const seq = l1->seq() + 1;
        std::vector<STTx> items;
        for (std::uint32_t i = 0; i < 2; ++i)  // only two of the three
            items.push_back(entropyTx(seq, listed[i], sha512Half(secret(i))));
        auto const offTx = entropyTx(seq, off, sha512Half(secret(9)));
        items.push_back(offTx);

        std::set<TxID> failed;
        auto const l2 = buildOn(env, l1, items, failed);
        BEAST_EXPECT(failed.size() == 1);
        BEAST_EXPECT(failed.count(offTx.getTransactionID()));

        auto const entropy = entropiesOf(l2);
        if (!BEAST_EXPECT(entropy.size() == 1))
            return;
        auto const& entries = entropy[0]->getFieldArray(sfRandomDigests);
        BEAST_EXPECT(entries.size() == 2);
        for (auto const& e : entries)
            BEAST_EXPECT(e.getFieldVL(sfPublicKey) != blobOf(off));

        auto const rng = l2->read(keylet::random());
        if (BEAST_EXPECT(rng))
        {
            BEAST_EXPECT(rng->getFieldU16(sfEntropyDenominator) == 3);
            BEAST_EXPECT(rng->getFieldArray(sfRandomDigests).size() == 2);
        }
    }

public:
    void
    run() override
    {
        using namespace jtx;
        auto const all = supported_amendments();
        auto const v1 = all - featureRNGV2;

        // individual-form transactions are applied directly: V1 rules only
        testDisabled(v1);
        testTransactor(v1);
        testUNLReport(v1);

        testPreflightV2();
        testEquivalence();
        testAggregateRejections();
        testNonCanonical();
        testRevealAndReplay();
        testUNLReportCollapse();

        // through a real ledger close, under both rule sets
        for (auto const& features : {v1, all})
        {
            testNoEntropy(features);
            testHooks(features);
            testSlotBlocked(features);
            testLedgerEntry(features);
            testBuildLedger(features);
            testValidatorClose(features);
        }
    }
};

BEAST_DEFINE_TESTSUITE(RNG, app, ripple);

}  // namespace test
}  // namespace ripple
