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
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/ledger/ApplyViewImpl.h>
#include <xrpld/ledger/OpenView.h>
#include <xrpl/hook/Enum.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>

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
        testcase("No entropy");
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
        testcase("Hook API");
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
        testcase("Hooks cannot read the RNG");
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

public:
    void
    run() override
    {
        using namespace jtx;
        auto const all = supported_amendments();
        testDisabled(all);
        testTransactor(all);
        testUNLReport(all);
        testNoEntropy(all);
        testHooks(all);
        testSlotBlocked(all);
    }
};

BEAST_DEFINE_TESTSUITE(RNG, app, ripple);

}  // namespace test
}  // namespace ripple
