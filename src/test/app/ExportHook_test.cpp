//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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
#include <xrpld/app/tx/apply.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/ledger/OpenView.h>
#include <xrpld/ledger/Sandbox.h>
#include <xrpl/hook/Enum.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/jss.h>
#include <optional>

namespace ripple {
namespace test {

namespace {

// Test Hooks for xport(), xport_prepare() and xport_fee() (featureExport).
// Each accept()s with what it is testing as its exit code. Built from the WAT
// shown with wat2wasm (wabt 1.0.34) and checked with the guard checker
// (include/xrpl/hook/guard_checker).
//
// xport() takes a complete transaction and fills nothing in, so the hooks
// that call it without xport_prepare() hold the whole export in their data
// segment and write their own account into it with hook_account(): a 1 drop
// Payment to 0102..14 with Sequence 0, OperationLimit 21337 (this network in
// these tests), Fee 1000 and an empty SigningPubKey. Those that prepare hold
// what a hook would start from: the type, the Amount and the Destination.

// A complete export naming TicketSequence 1. A hook naming its own ticket
// needs no pool.
//
// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "etxn_reserve" (func $etxn_reserve (param i32) (result i64)))
//   (import "env" "hook_account" (func $hook_account (param i32 i32) (result
//   i64))) (import "env" "xport" (func $xport (param i32 i32 i32 i32) (result
//   i64))) (memory 1) (data (i32.const 0)
//   "\12\00\00\24\00\00\00\00\20\1d\00\00\53\59\20\29\00\00\00\01\61\40\00\00\00\00\00\00\01\68\40\00\00\00\00\00\03\e8\73\00\81\14\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\83\14\01\02\03\04\05\06\07\08\09\0a\0b\0c\0d\0e\0f\10\11\12\13\14")
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (drop (call $etxn_reserve (i32.const 1)))
//     (drop (call $hook_account (i32.const 42) (i32.const 20)))
//     (call $accept (i32.const 0) (i32.const 0)
//       (call $xport (i32.const 256) (i32.const 32) (i32.const 0) (i32.const
//       84))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const xportTicketWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x21U, 0x05U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x02U, 0x7FU, 0x7FU,
    0x01U, 0x7EU, 0x60U, 0x04U, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x02U,
    0x49U, 0x05U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U, 0x00U,
    0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U,
    0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0CU, 0x65U, 0x74U, 0x78U, 0x6EU,
    0x5FU, 0x72U, 0x65U, 0x73U, 0x65U, 0x72U, 0x76U, 0x65U, 0x00U, 0x02U, 0x03U,
    0x65U, 0x6EU, 0x76U, 0x0CU, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x5FU, 0x61U, 0x63U,
    0x63U, 0x6FU, 0x75U, 0x6EU, 0x74U, 0x00U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U,
    0x05U, 0x78U, 0x70U, 0x6FU, 0x72U, 0x74U, 0x00U, 0x04U, 0x03U, 0x02U, 0x01U,
    0x02U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U, 0x11U, 0x02U, 0x06U, 0x6DU,
    0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U, 0x6FU, 0x6FU,
    0x6BU, 0x00U, 0x05U, 0x0AU, 0x29U, 0x01U, 0x27U, 0x00U, 0x41U, 0x01U, 0x41U,
    0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x01U, 0x10U, 0x02U, 0x1AU, 0x41U, 0x2AU,
    0x41U, 0x14U, 0x10U, 0x03U, 0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x41U, 0x80U,
    0x02U, 0x41U, 0x20U, 0x41U, 0x00U, 0x41U, 0xD4U, 0x00U, 0x10U, 0x04U, 0x10U,
    0x01U, 0x0BU, 0x0BU, 0x5AU, 0x01U, 0x00U, 0x41U, 0x00U, 0x0BU, 0x54U, 0x12U,
    0x00U, 0x00U, 0x24U, 0x00U, 0x00U, 0x00U, 0x00U, 0x20U, 0x1DU, 0x00U, 0x00U,
    0x53U, 0x59U, 0x20U, 0x29U, 0x00U, 0x00U, 0x00U, 0x01U, 0x61U, 0x40U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x68U, 0x40U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x03U, 0xE8U, 0x73U, 0x00U, 0x81U, 0x14U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x83U, 0x14U, 0x01U, 0x02U, 0x03U,
    0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU,
    0x0FU, 0x10U, 0x11U, 0x12U, 0x13U, 0x14U,
};

// The same without a ticket: left to the pool, which has none here.
//
// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "etxn_reserve" (func $etxn_reserve (param i32) (result i64)))
//   (import "env" "hook_account" (func $hook_account (param i32 i32) (result
//   i64))) (import "env" "xport" (func $xport (param i32 i32 i32 i32) (result
//   i64))) (memory 1) (data (i32.const 0)
//   "\12\00\00\24\00\00\00\00\20\1d\00\00\53\59\61\40\00\00\00\00\00\00\01\68\40\00\00\00\00\00\03\e8\73\00\81\14\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\83\14\01\02\03\04\05\06\07\08\09\0a\0b\0c\0d\0e\0f\10\11\12\13\14")
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (drop (call $etxn_reserve (i32.const 1)))
//     (drop (call $hook_account (i32.const 36) (i32.const 20)))
//     (call $accept (i32.const 0) (i32.const 0)
//       (call $xport (i32.const 256) (i32.const 32) (i32.const 0) (i32.const
//       78))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const xportWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x21U, 0x05U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x02U, 0x7FU, 0x7FU,
    0x01U, 0x7EU, 0x60U, 0x04U, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x02U,
    0x49U, 0x05U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U, 0x00U,
    0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U,
    0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0CU, 0x65U, 0x74U, 0x78U, 0x6EU,
    0x5FU, 0x72U, 0x65U, 0x73U, 0x65U, 0x72U, 0x76U, 0x65U, 0x00U, 0x02U, 0x03U,
    0x65U, 0x6EU, 0x76U, 0x0CU, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x5FU, 0x61U, 0x63U,
    0x63U, 0x6FU, 0x75U, 0x6EU, 0x74U, 0x00U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U,
    0x05U, 0x78U, 0x70U, 0x6FU, 0x72U, 0x74U, 0x00U, 0x04U, 0x03U, 0x02U, 0x01U,
    0x02U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U, 0x11U, 0x02U, 0x06U, 0x6DU,
    0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U, 0x6FU, 0x6FU,
    0x6BU, 0x00U, 0x05U, 0x0AU, 0x29U, 0x01U, 0x27U, 0x00U, 0x41U, 0x01U, 0x41U,
    0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x01U, 0x10U, 0x02U, 0x1AU, 0x41U, 0x24U,
    0x41U, 0x14U, 0x10U, 0x03U, 0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x41U, 0x80U,
    0x02U, 0x41U, 0x20U, 0x41U, 0x00U, 0x41U, 0xCEU, 0x00U, 0x10U, 0x04U, 0x10U,
    0x01U, 0x0BU, 0x0BU, 0x54U, 0x01U, 0x00U, 0x41U, 0x00U, 0x0BU, 0x4EU, 0x12U,
    0x00U, 0x00U, 0x24U, 0x00U, 0x00U, 0x00U, 0x00U, 0x20U, 0x1DU, 0x00U, 0x00U,
    0x53U, 0x59U, 0x61U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x68U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U, 0xE8U, 0x73U, 0x00U,
    0x81U, 0x14U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x83U, 0x14U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U,
    0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U, 0x11U, 0x12U, 0x13U, 0x14U,
};

// xportTicketWasm without etxn_reserve().
//
// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "hook_account" (func $hook_account (param i32 i32) (result
//   i64))) (import "env" "xport" (func $xport (param i32 i32 i32 i32) (result
//   i64))) (memory 1) (data (i32.const 0)
//   "\12\00\00\24\00\00\00\00\20\1d\00\00\53\59\20\29\00\00\00\01\61\40\00\00\00\00\00\00\01\68\40\00\00\00\00\00\03\e8\73\00\81\14\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\83\14\01\02\03\04\05\06\07\08\09\0a\0b\0c\0d\0e\0f\10\11\12\13\14")
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (drop (call $hook_account (i32.const 42) (i32.const 20)))
//     (call $accept (i32.const 0) (i32.const 0)
//       (call $xport (i32.const 256) (i32.const 32) (i32.const 0) (i32.const
//       84))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const xportNoReserveWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x21U, 0x05U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x04U, 0x7FU,
    0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x02U,
    0x36U, 0x04U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U, 0x00U,
    0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U,
    0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0CU, 0x68U, 0x6FU, 0x6FU, 0x6BU,
    0x5FU, 0x61U, 0x63U, 0x63U, 0x6FU, 0x75U, 0x6EU, 0x74U, 0x00U, 0x02U, 0x03U,
    0x65U, 0x6EU, 0x76U, 0x05U, 0x78U, 0x70U, 0x6FU, 0x72U, 0x74U, 0x00U, 0x03U,
    0x03U, 0x02U, 0x01U, 0x04U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U, 0x11U,
    0x02U, 0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U, 0x04U,
    0x68U, 0x6FU, 0x6FU, 0x6BU, 0x00U, 0x04U, 0x0AU, 0x24U, 0x01U, 0x22U, 0x00U,
    0x41U, 0x01U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x2AU, 0x41U, 0x14U,
    0x10U, 0x02U, 0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x41U, 0x80U, 0x02U, 0x41U,
    0x20U, 0x41U, 0x00U, 0x41U, 0xD4U, 0x00U, 0x10U, 0x03U, 0x10U, 0x01U, 0x0BU,
    0x0BU, 0x5AU, 0x01U, 0x00U, 0x41U, 0x00U, 0x0BU, 0x54U, 0x12U, 0x00U, 0x00U,
    0x24U, 0x00U, 0x00U, 0x00U, 0x00U, 0x20U, 0x1DU, 0x00U, 0x00U, 0x53U, 0x59U,
    0x20U, 0x29U, 0x00U, 0x00U, 0x00U, 0x01U, 0x61U, 0x40U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x68U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x03U, 0xE8U, 0x73U, 0x00U, 0x81U, 0x14U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x83U, 0x14U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U,
    0x06U, 0x07U, 0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U,
    0x11U, 0x12U, 0x13U, 0x14U,
};

// xport()'s own checks, none of which emits. Adds up: 1 for TOO_SMALL (a
// 31 byte write buffer), 10 for OUT_OF_BOUNDS, 100 for INVALID_ARGUMENT from
// what is no transaction (3 bytes), and 1000 for INVALID_ARGUMENT from what
// xport() once completed by itself (at offset 128: no Account, Sequence, Fee
// or SigningPubKey).
//
// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "etxn_reserve" (func $etxn_reserve (param i32) (result i64)))
//   (import "env" "hook_account" (func $hook_account (param i32 i32) (result
//   i64))) (import "env" "xport" (func $xport (param i32 i32 i32 i32) (result
//   i64))) (memory 1) (data (i32.const 0)
//   "\12\00\00\24\00\00\00\00\20\1d\00\00\53\59\20\29\00\00\00\01\61\40\00\00\00\00\00\00\01\68\40\00\00\00\00\00\03\e8\73\00\81\14\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\83\14\01\02\03\04\05\06\07\08\09\0a\0b\0c\0d\0e\0f\10\11\12\13\14")
//   (data (i32.const 128)
//   "\12\00\00\20\29\00\00\00\01\61\40\00\00\00\00\00\00\01\83\14\01\02\03\04\05\06\07\08\09\0a\0b\0c\0d\0e\0f\10\11\12\13\14")
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (drop (call $etxn_reserve (i32.const 1)))
//     (drop (call $hook_account (i32.const 42) (i32.const 20)))
//     (call $accept (i32.const 0) (i32.const 0)
//       (i64.add
//         (i64.add
//           (i64.mul (i64.const 1) (i64.extend_i32_u (i64.eq (call $xport
//           (i32.const 256) (i32.const 31) (i32.const 0) (i32.const 84))
//           (i64.const -4)))) (i64.mul (i64.const 10) (i64.extend_i32_u (i64.eq
//           (call $xport (i32.const 256) (i32.const 32) (i32.const 70000)
//           (i32.const 84)) (i64.const -1)))))
//         (i64.add
//           (i64.mul (i64.const 100) (i64.extend_i32_u (i64.eq (call $xport
//           (i32.const 256) (i32.const 32) (i32.const 0) (i32.const 3))
//           (i64.const -7)))) (i64.mul (i64.const 1000) (i64.extend_i32_u
//           (i64.eq (call $xport (i32.const 256) (i32.const 32) (i32.const 128)
//           (i32.const 40)) (i64.const -7))))))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const xportChecksWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x21U, 0x05U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x02U, 0x7FU, 0x7FU,
    0x01U, 0x7EU, 0x60U, 0x04U, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x02U,
    0x49U, 0x05U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U, 0x00U,
    0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U,
    0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0CU, 0x65U, 0x74U, 0x78U, 0x6EU,
    0x5FU, 0x72U, 0x65U, 0x73U, 0x65U, 0x72U, 0x76U, 0x65U, 0x00U, 0x02U, 0x03U,
    0x65U, 0x6EU, 0x76U, 0x0CU, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x5FU, 0x61U, 0x63U,
    0x63U, 0x6FU, 0x75U, 0x6EU, 0x74U, 0x00U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U,
    0x05U, 0x78U, 0x70U, 0x6FU, 0x72U, 0x74U, 0x00U, 0x04U, 0x03U, 0x02U, 0x01U,
    0x02U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U, 0x11U, 0x02U, 0x06U, 0x6DU,
    0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U, 0x6FU, 0x6FU,
    0x6BU, 0x00U, 0x05U, 0x0AU, 0x6FU, 0x01U, 0x6DU, 0x00U, 0x41U, 0x01U, 0x41U,
    0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x01U, 0x10U, 0x02U, 0x1AU, 0x41U, 0x2AU,
    0x41U, 0x14U, 0x10U, 0x03U, 0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x42U, 0x01U,
    0x41U, 0x80U, 0x02U, 0x41U, 0x1FU, 0x41U, 0x00U, 0x41U, 0xD4U, 0x00U, 0x10U,
    0x04U, 0x42U, 0x7CU, 0x51U, 0xADU, 0x7EU, 0x42U, 0x0AU, 0x41U, 0x80U, 0x02U,
    0x41U, 0x20U, 0x41U, 0xF0U, 0xA2U, 0x04U, 0x41U, 0xD4U, 0x00U, 0x10U, 0x04U,
    0x42U, 0x7FU, 0x51U, 0xADU, 0x7EU, 0x7CU, 0x42U, 0xE4U, 0x00U, 0x41U, 0x80U,
    0x02U, 0x41U, 0x20U, 0x41U, 0x00U, 0x41U, 0x03U, 0x10U, 0x04U, 0x42U, 0x79U,
    0x51U, 0xADU, 0x7EU, 0x42U, 0xE8U, 0x07U, 0x41U, 0x80U, 0x02U, 0x41U, 0x20U,
    0x41U, 0x80U, 0x01U, 0x41U, 0x28U, 0x10U, 0x04U, 0x42U, 0x79U, 0x51U, 0xADU,
    0x7EU, 0x7CU, 0x7CU, 0x10U, 0x01U, 0x0BU, 0x0BU, 0x88U, 0x01U, 0x02U, 0x00U,
    0x41U, 0x00U, 0x0BU, 0x54U, 0x12U, 0x00U, 0x00U, 0x24U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x20U, 0x1DU, 0x00U, 0x00U, 0x53U, 0x59U, 0x20U, 0x29U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x61U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x68U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U, 0xE8U, 0x73U, 0x00U,
    0x81U, 0x14U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x83U, 0x14U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U,
    0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U, 0x11U, 0x12U, 0x13U, 0x14U,
    0x00U, 0x41U, 0x80U, 0x01U, 0x0BU, 0x28U, 0x12U, 0x00U, 0x00U, 0x20U, 0x29U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x61U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x83U, 0x14U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
    0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U, 0x11U, 0x12U,
    0x13U, 0x14U,
};

// xport_prepare() for an account with no signer list on the destination.
// Adds up: 1 for DOESNT_EXIST, 10 for INVALID_ARGUMENT from what is no
// object (a truncated Amount), 100 and 1000 for OUT_OF_BOUNDS reading and
// writing.
//
// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "xport_prepare" (func $xport_prepare (param i32 i32 i32 i32)
//   (result i64))) (memory 1) (data (i32.const 0)
//   "\12\00\00\61\40\00\00\00\00\00\00\01\83\14\01\02\03\04\05\06\07\08\09\0a\0b\0c\0d\0e\0f\10\11\12\13\14")
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $accept (i32.const 0) (i32.const 0)
//       (i64.add
//         (i64.add
//           (i64.mul (i64.const 1) (i64.extend_i32_u (i64.eq (call
//           $xport_prepare (i32.const 512) (i32.const 512) (i32.const 0)
//           (i32.const 34)) (i64.const -5)))) (i64.mul (i64.const 10)
//           (i64.extend_i32_u (i64.eq (call $xport_prepare (i32.const 512)
//           (i32.const 512) (i32.const 3) (i32.const 3)) (i64.const -7)))))
//         (i64.add
//           (i64.mul (i64.const 100) (i64.extend_i32_u (i64.eq (call
//           $xport_prepare (i32.const 512) (i32.const 512) (i32.const 70000)
//           (i32.const 34)) (i64.const -1)))) (i64.mul (i64.const 1000)
//           (i64.extend_i32_u (i64.eq (call $xport_prepare (i32.const 65530)
//           (i32.const 512) (i32.const 0) (i32.const 34)) (i64.const -1))))))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const xportPrepareWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x1BU, 0x04U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x04U, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U,
    0x01U, 0x7FU, 0x01U, 0x7EU, 0x02U, 0x2BU, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U,
    0x02U, 0x5FU, 0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U,
    0x63U, 0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U,
    0x0DU, 0x78U, 0x70U, 0x6FU, 0x72U, 0x74U, 0x5FU, 0x70U, 0x72U, 0x65U, 0x70U,
    0x61U, 0x72U, 0x65U, 0x00U, 0x02U, 0x03U, 0x02U, 0x01U, 0x03U, 0x05U, 0x03U,
    0x01U, 0x00U, 0x01U, 0x07U, 0x11U, 0x02U, 0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU,
    0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x00U, 0x03U,
    0x0AU, 0x65U, 0x01U, 0x63U, 0x00U, 0x41U, 0x01U, 0x41U, 0x01U, 0x10U, 0x00U,
    0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x42U, 0x01U, 0x41U, 0x80U, 0x04U, 0x41U,
    0x80U, 0x04U, 0x41U, 0x00U, 0x41U, 0x22U, 0x10U, 0x02U, 0x42U, 0x7BU, 0x51U,
    0xADU, 0x7EU, 0x42U, 0x0AU, 0x41U, 0x80U, 0x04U, 0x41U, 0x80U, 0x04U, 0x41U,
    0x03U, 0x41U, 0x03U, 0x10U, 0x02U, 0x42U, 0x79U, 0x51U, 0xADU, 0x7EU, 0x7CU,
    0x42U, 0xE4U, 0x00U, 0x41U, 0x80U, 0x04U, 0x41U, 0x80U, 0x04U, 0x41U, 0xF0U,
    0xA2U, 0x04U, 0x41U, 0x22U, 0x10U, 0x02U, 0x42U, 0x7FU, 0x51U, 0xADU, 0x7EU,
    0x42U, 0xE8U, 0x07U, 0x41U, 0xFAU, 0xFFU, 0x03U, 0x41U, 0x80U, 0x04U, 0x41U,
    0x00U, 0x41U, 0x22U, 0x10U, 0x02U, 0x42U, 0x7FU, 0x51U, 0xADU, 0x7EU, 0x7CU,
    0x7CU, 0x10U, 0x01U, 0x0BU, 0x0BU, 0x28U, 0x01U, 0x00U, 0x41U, 0x00U, 0x0BU,
    0x22U, 0x12U, 0x00U, 0x00U, 0x61U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x83U, 0x14U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
    0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U, 0x11U, 0x12U,
    0x13U, 0x14U,
};

// xport_prepare() for an account that has one. Adds up: 1 for TOO_SMALL (the
// result is never written in part), 10 for NOT_EXPORTABLE (an Invoke, at
// offset 64), 100 for a result longer than what went in.
//
// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "xport_prepare" (func $xport_prepare (param i32 i32 i32 i32)
//   (result i64))) (memory 1) (data (i32.const 0)
//   "\12\00\00\61\40\00\00\00\00\00\00\01\83\14\01\02\03\04\05\06\07\08\09\0a\0b\0c\0d\0e\0f\10\11\12\13\14")
//   (data (i32.const 64) "\12\00\63")
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $accept (i32.const 0) (i32.const 0)
//       (i64.add
//         (i64.mul (i64.const 1) (i64.extend_i32_u (i64.eq (call $xport_prepare
//         (i32.const 512) (i32.const 8) (i32.const 0) (i32.const 34))
//         (i64.const -4)))) (i64.add
//           (i64.mul (i64.const 10) (i64.extend_i32_u (i64.eq (call
//           $xport_prepare (i32.const 512) (i32.const 512) (i32.const 64)
//           (i32.const 3)) (i64.const -48)))) (i64.mul (i64.const 100)
//           (i64.extend_i32_u (i64.gt_s
//             (call $xport_prepare (i32.const 512) (i32.const 512) (i32.const
//             0) (i32.const 34)) (i64.const 34))))))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const xportPrepareListedWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x1BU, 0x04U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x04U, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U,
    0x01U, 0x7FU, 0x01U, 0x7EU, 0x02U, 0x2BU, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U,
    0x02U, 0x5FU, 0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U,
    0x63U, 0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U,
    0x0DU, 0x78U, 0x70U, 0x6FU, 0x72U, 0x74U, 0x5FU, 0x70U, 0x72U, 0x65U, 0x70U,
    0x61U, 0x72U, 0x65U, 0x00U, 0x02U, 0x03U, 0x02U, 0x01U, 0x03U, 0x05U, 0x03U,
    0x01U, 0x00U, 0x01U, 0x07U, 0x11U, 0x02U, 0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU,
    0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x00U, 0x03U,
    0x0AU, 0x4DU, 0x01U, 0x4BU, 0x00U, 0x41U, 0x01U, 0x41U, 0x01U, 0x10U, 0x00U,
    0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x42U, 0x01U, 0x41U, 0x80U, 0x04U, 0x41U,
    0x08U, 0x41U, 0x00U, 0x41U, 0x22U, 0x10U, 0x02U, 0x42U, 0x7CU, 0x51U, 0xADU,
    0x7EU, 0x42U, 0x0AU, 0x41U, 0x80U, 0x04U, 0x41U, 0x80U, 0x04U, 0x41U, 0xC0U,
    0x00U, 0x41U, 0x03U, 0x10U, 0x02U, 0x42U, 0x50U, 0x51U, 0xADU, 0x7EU, 0x42U,
    0xE4U, 0x00U, 0x41U, 0x80U, 0x04U, 0x41U, 0x80U, 0x04U, 0x41U, 0x00U, 0x41U,
    0x22U, 0x10U, 0x02U, 0x42U, 0x22U, 0x55U, 0xADU, 0x7EU, 0x7CU, 0x7CU, 0x10U,
    0x01U, 0x0BU, 0x0BU, 0x31U, 0x02U, 0x00U, 0x41U, 0x00U, 0x0BU, 0x22U, 0x12U,
    0x00U, 0x00U, 0x61U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x83U, 0x14U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U,
    0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U, 0x11U, 0x12U, 0x13U, 0x14U,
    0x00U, 0x41U, 0xC0U, 0x00U, 0x0BU, 0x03U, 0x12U, 0x00U, 0x63U,
};

// What a hook is meant to do: xport_prepare() what it starts from, then
// xport() the result (or accept with xport_prepare()'s error).
//
// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "etxn_reserve" (func $etxn_reserve (param i32) (result i64)))
//   (import "env" "xport" (func $xport (param i32 i32 i32 i32) (result i64)))
//   (import "env" "xport_prepare" (func $xport_prepare (param i32 i32 i32 i32)
//   (result i64))) (memory 1) (data (i32.const 0)
//   "\12\00\00\61\40\00\00\00\00\00\00\01\83\14\01\02\03\04\05\06\07\08\09\0a\0b\0c\0d\0e\0f\10\11\12\13\14")
//   (func $hook (param i32) (result i64)
//     (local $n i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (drop (call $etxn_reserve (i32.const 1)))
//     (local.set $n (call $xport_prepare (i32.const 512) (i32.const 512)
//     (i32.const 0) (i32.const 34))) (if (i64.lt_s (local.get $n) (i64.const
//     0))
//       (then (drop (call $accept (i32.const 0) (i32.const 0) (local.get
//       $n)))))
//     (call $accept (i32.const 0) (i32.const 0)
//       (call $xport (i32.const 256) (i32.const 32) (i32.const 512)
//         (i32.wrap_i64 (local.get $n)))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const xportPreparedWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x1BU, 0x04U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x04U, 0x7FU, 0x7FU,
    0x7FU, 0x7FU, 0x01U, 0x7EU, 0x02U, 0x4AU, 0x05U, 0x03U, 0x65U, 0x6EU, 0x76U,
    0x02U, 0x5FU, 0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U,
    0x63U, 0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U,
    0x0CU, 0x65U, 0x74U, 0x78U, 0x6EU, 0x5FU, 0x72U, 0x65U, 0x73U, 0x65U, 0x72U,
    0x76U, 0x65U, 0x00U, 0x02U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x05U, 0x78U, 0x70U,
    0x6FU, 0x72U, 0x74U, 0x00U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0DU, 0x78U,
    0x70U, 0x6FU, 0x72U, 0x74U, 0x5FU, 0x70U, 0x72U, 0x65U, 0x70U, 0x61U, 0x72U,
    0x65U, 0x00U, 0x03U, 0x03U, 0x02U, 0x01U, 0x02U, 0x05U, 0x03U, 0x01U, 0x00U,
    0x01U, 0x07U, 0x11U, 0x02U, 0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U,
    0x02U, 0x00U, 0x04U, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x00U, 0x05U, 0x0AU, 0x44U,
    0x01U, 0x42U, 0x01U, 0x01U, 0x7EU, 0x41U, 0x01U, 0x41U, 0x01U, 0x10U, 0x00U,
    0x1AU, 0x41U, 0x01U, 0x10U, 0x02U, 0x1AU, 0x41U, 0x80U, 0x04U, 0x41U, 0x80U,
    0x04U, 0x41U, 0x00U, 0x41U, 0x22U, 0x10U, 0x04U, 0x21U, 0x01U, 0x20U, 0x01U,
    0x42U, 0x00U, 0x53U, 0x04U, 0x40U, 0x41U, 0x00U, 0x41U, 0x00U, 0x20U, 0x01U,
    0x10U, 0x01U, 0x1AU, 0x0BU, 0x41U, 0x00U, 0x41U, 0x00U, 0x41U, 0x80U, 0x02U,
    0x41U, 0x20U, 0x41U, 0x80U, 0x04U, 0x20U, 0x01U, 0xA7U, 0x10U, 0x03U, 0x10U,
    0x01U, 0x0BU, 0x0BU, 0x28U, 0x01U, 0x00U, 0x41U, 0x00U, 0x0BU, 0x22U, 0x12U,
    0x00U, 0x00U, 0x61U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x83U, 0x14U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U,
    0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U, 0x11U, 0x12U, 0x13U, 0x14U,
};

// xport_fee(): the unit, plus 100 if the median is DOESNT_EXIST and 1000 if
// a code that is none is INVALID_ARGUMENT.
//
// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "xport_fee" (func $xport_fee (param i32 i32) (result i64)))
//   (memory 1)
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $accept (i32.const 0) (i32.const 0)
//       (i64.add
//         (call $xport_fee (i32.const 0) (i32.const 0))
//         (i64.add
//           (i64.mul (i64.const 100) (i64.extend_i32_u (i64.eq (call $xport_fee
//           (i32.const 0) (i32.const 1)) (i64.const -5)))) (i64.mul (i64.const
//           1000) (i64.extend_i32_u (i64.eq (call $xport_fee (i32.const 0)
//           (i32.const 5)) (i64.const -7))))))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const xportFeeWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x19U, 0x04U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU,
    0x01U, 0x7EU, 0x02U, 0x27U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU,
    0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U, 0x63U,
    0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x09U, 0x78U,
    0x70U, 0x6FU, 0x72U, 0x74U, 0x5FU, 0x66U, 0x65U, 0x65U, 0x00U, 0x02U, 0x03U,
    0x02U, 0x01U, 0x03U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U, 0x11U, 0x02U,
    0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U,
    0x6FU, 0x6FU, 0x6BU, 0x00U, 0x03U, 0x0AU, 0x35U, 0x01U, 0x33U, 0x00U, 0x41U,
    0x01U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x41U,
    0x00U, 0x41U, 0x00U, 0x10U, 0x02U, 0x42U, 0xE4U, 0x00U, 0x41U, 0x00U, 0x41U,
    0x01U, 0x10U, 0x02U, 0x42U, 0x7BU, 0x51U, 0xADU, 0x7EU, 0x42U, 0xE8U, 0x07U,
    0x41U, 0x00U, 0x41U, 0x05U, 0x10U, 0x02U, 0x42U, 0x79U, 0x51U, 0xADU, 0x7EU,
    0x7CU, 0x7CU, 0x10U, 0x01U, 0x0BU,
};

// xport_fee(): every code, each in decimal places of its own: the unit, 100
// x the median, 10^4 x the ledger, 10^8 x its transactions, 10^10 x its burn.
//
// (module
//   (import "env" "_g" (func $_g (param i32 i32) (result i32)))
//   (import "env" "accept" (func $accept (param i32 i32 i64) (result i64)))
//   (import "env" "xport_fee" (func $xport_fee (param i32 i32) (result i64)))
//   (memory 1)
//   (func $hook (param i32) (result i64)
//     (drop (call $_g (i32.const 1) (i32.const 1)))
//     (call $accept (i32.const 0) (i32.const 0)
//       (i64.add
//         (i64.add
//           (call $xport_fee (i32.const 0) (i32.const 0))
//           (i64.mul (i64.const 100) (call $xport_fee (i32.const 0) (i32.const
//           1))))
//         (i64.add
//           (i64.mul (i64.const 10000) (call $xport_fee (i32.const 0)
//           (i32.const 2))) (i64.add
//             (i64.mul (i64.const 100000000) (call $xport_fee (i32.const 0)
//             (i32.const 3))) (i64.mul (i64.const 10000000000) (call $xport_fee
//             (i32.const 0) (i32.const 4))))))))
//   (export "memory" (memory 0))
//   (export "hook" (func $hook)))
static std::vector<std::uint8_t> const xportFeeAllWasm = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x19U, 0x04U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU,
    0x01U, 0x7EU, 0x02U, 0x27U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU,
    0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U, 0x63U,
    0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x09U, 0x78U,
    0x70U, 0x6FU, 0x72U, 0x74U, 0x5FU, 0x66U, 0x65U, 0x65U, 0x00U, 0x02U, 0x03U,
    0x02U, 0x01U, 0x03U, 0x05U, 0x03U, 0x01U, 0x00U, 0x01U, 0x07U, 0x11U, 0x02U,
    0x06U, 0x6DU, 0x65U, 0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U,
    0x6FU, 0x6FU, 0x6BU, 0x00U, 0x03U, 0x0AU, 0x49U, 0x01U, 0x47U, 0x00U, 0x41U,
    0x01U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x41U,
    0x00U, 0x41U, 0x00U, 0x10U, 0x02U, 0x42U, 0xE4U, 0x00U, 0x41U, 0x00U, 0x41U,
    0x01U, 0x10U, 0x02U, 0x7EU, 0x7CU, 0x42U, 0x90U, 0xCEU, 0x00U, 0x41U, 0x00U,
    0x41U, 0x02U, 0x10U, 0x02U, 0x7EU, 0x42U, 0x80U, 0xC2U, 0xD7U, 0x2FU, 0x41U,
    0x00U, 0x41U, 0x03U, 0x10U, 0x02U, 0x7EU, 0x42U, 0x80U, 0xC8U, 0xAFU, 0xA0U,
    0x25U, 0x41U, 0x00U, 0x41U, 0x04U, 0x10U, 0x02U, 0x7EU, 0x7CU, 0x7CU, 0x7CU,
    0x10U, 0x01U, 0x0BU,
};

}  // namespace

/** xport(), xport_prepare() and xport_fee(): a hook exporting a transaction
    for another network. The ttEXPORT it emits is followed as far as it goes
    here; Export_test covers the transactors themselves. */
class ExportHook_test : public beast::unit_test::suite
{
    int hookAccounts_ = 0;

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

    static std::unique_ptr<Config>
    xahau()
    {
        // not network 0: an export to XRPL (0) is refused from there
        auto cfg = jtx::envconfig();
        cfg->NETWORK_ID = 21337;
        return cfg;
    }

    // Install a Hook on a fresh account
    jtx::Account
    install(jtx::Env& env, std::vector<std::uint8_t> const& wasm)
    {
        using namespace jtx;
        Account const account{"hook" + std::to_string(++hookAccounts_)};
        env.fund(XRP(10000), account);
        env.close();
        env(hook(account, {{hso(wasm)}}, 0), fee(XRP(100)));
        env.close();
        return account;
    }

    // Install a Hook on a fresh account, pay it, and return the account and
    // the payment's metadata.
    std::pair<jtx::Account, std::shared_ptr<STObject const>>
    runHook(
        jtx::Env& env,
        jtx::Account const& payer,
        std::vector<std::uint8_t> const& wasm)
    {
        using namespace jtx;
        auto const account = install(env, wasm);
        env(pay(payer, account, XRP(1)), fee(XRP(1)));
        env.close();
        return {account, env.meta()};
    }

    // What only an Import, or a ttINGEST, puts on the ledger is written
    // straight into a view over the last closed ledger, and the payment that
    // runs the hook (and then what the hook emits) applied to that view as
    // the next ledger would apply it. The view must not be open: an open
    // view keeps no metadata and adds no emitted txns to the ledger
    // (finalizeHookResult), and Env::meta() would close the ledger,
    // rebuilding it from its transactions without what was written in.

    // `id` as an exporter to the XRPL: `signers` signers on its list there,
    // and tickets 10 and 11 pooled
    static void
    makeExporter(OpenView& view, AccountID const& id, std::size_t signers)
    {
        Sandbox sb(&view, tapNONE);
        auto const acc = sb.peek(keylet::account(id));
        auto const ex = exporterOf(
            sb, acc, 0, beast::Journal{beast::Journal::getNullSink()});
        STArray entries(sfSignerEntries);
        for (std::size_t i = 0; i < signers; ++i)
        {
            STObject e(sfSignerEntry);
            e.setAccountID(
                sfAccount,
                calcAccountID(randomKeyPair(KeyType::ed25519).first));
            e.setFieldU16(sfSignerWeight, 1);
            entries.push_back(std::move(e));
        }
        auto l = STObject::makeInnerObject(sfExportSignerList);
        l[sfSignerQuorum] = 1;
        l.setFieldArray(sfSignerEntries, entries);
        l[sfLedgerSequence] = 1;
        l[sfTransactionIndex] = 0;
        ex->peekFieldObject(sfExportSignerList) = std::move(l);
        ExportTickets tickets;
        tickets.add(10, 2);
        tickets.store(*ex);
        sb.insert(ex);
        sb.update(acc);
        sb.apply(view);
    }

    // what ttINGEST has proven of the XRPL (see Ingest.h)
    static void
    makeProven(
        OpenView& view,
        std::uint32_t unit,
        std::uint32_t median,
        std::uint32_t ledger,
        std::uint32_t txns,
        std::uint64_t burned)
    {
        auto const sle = std::make_shared<SLE>(keylet::exportFee(0));
        sle->setFieldU32(sfRemoteNetworkID, 0);
        sle->setFieldU32(sfExportFeeUnit, unit);
        sle->setFieldU32(sfSampleFee, median);
        sle->setFieldU32(sfLedgerSequence, ledger);
        sle->setFieldU32(sfTransactionCount, txns);
        sle->setFieldU64(sfBurnedDrops, burned);
        view.rawInsert(sle);
    }

    // Pay `account` in `view`: what its hook returned and, if it emitted
    // one transaction, that transaction
    std::pair<std::vector<std::uint64_t>, std::optional<STTx>>
    payHook(
        jtx::Env& env,
        OpenView& view,
        jtx::Account const& payer,
        jtx::Account const& account)
    {
        using namespace jtx;
        auto const jt = env.jt(pay(payer, account, XRP(1)), fee(XRP(1)));
        auto const res =
            ripple::apply(env.app(), view, *jt.stx, tapNONE, env.journal);
        if (!BEAST_EXPECT(res.applied && res.ter == tesSUCCESS && res.metadata))
            return {};
        auto const meta =
            std::make_shared<STObject const>(res.metadata->getAsObject());
        std::optional<STTx> emitted;
        if (meta->isFieldPresent(sfHookEmissions))
        {
            auto const& emissions = meta->getFieldArray(sfHookEmissions);
            auto const sle = emissions.size() == 1
                ? view.read(keylet::emittedTxn(
                      emissions[0].getFieldH256(sfEmittedTxnID)))
                : nullptr;
            if (BEAST_EXPECT(sle))
            {
                Serializer s;
                sle->peekAtField(sfEmittedTxn).downcast<STObject>().add(s);
                SerialIter sit{s.slice()};
                emitted.emplace(sit);
            }
        }
        return {returnCodes(meta), emitted};
    }

    void
    testXport()
    {
        testcase("xport");
        using namespace jtx;
        using enum hook_api::hook_return_code;

        Env env{*this, xahau(), supported_amendments() | featureExport};
        Account const bob{"bob"};
        env.fund(XRP(10000), bob);
        env.close();

        // the export is emitted as the hook built it: nothing is filled in
        {
            auto const [account, meta] = runHook(env, bob, xportTicketWasm);
            BEAST_EXPECT(returnCodes(meta) == std::vector<std::uint64_t>{32});
            if (!BEAST_EXPECT(meta && meta->isFieldPresent(sfHookEmissions)))
                return;
            auto const& emissions = meta->getFieldArray(sfHookEmissions);
            if (!BEAST_EXPECT(emissions.size() == 1))
                return;
            auto const id = emissions[0].getFieldH256(sfEmittedTxnID);

            // it lands in the next ledger, and stops at preclaim: the
            // account has no signer list on the other network
            env.close();
            Json::Value params;
            params[jss::transaction] = to_string(id);
            auto const r =
                env.rpc("json", "tx", to_string(params))[jss::result];
            BEAST_EXPECT(r[jss::TransactionType] == "Export");
            BEAST_EXPECT(r[jss::Account] == account.human());
            auto const& inner = r[sfExportedTxn.jsonName];
            BEAST_EXPECT(inner[jss::TransactionType] == "Payment");
            BEAST_EXPECT(inner[jss::Account] == account.human());
            BEAST_EXPECT(inner[jss::SigningPubKey] == "");
            BEAST_EXPECT(inner[jss::Sequence] == 0);
            BEAST_EXPECT(inner[jss::Fee] == "1000");
            BEAST_EXPECT(
                inner[sfOperationLimit.jsonName] ==
                env.app().config().NETWORK_ID);
            BEAST_EXPECT(inner[sfTicketSequence.jsonName] == 1);
            BEAST_EXPECT(!inner.isMember(sfLastLedgerSequence.jsonName));
            BEAST_EXPECT(
                r[jss::meta][sfTransactionResult.jsonName] == "tecNO_TARGET");
        }

        // without a ticket of its own the hook needs one from the account's
        // pool, and this account has none
        BEAST_EXPECT(
            returnCodes(runHook(env, bob, xportWasm).second) ==
            std::vector<std::uint64_t>{rc(NO_TICKET)});

        // The API's own checks: a write buffer too small for the id, a read
        // out of bounds, what is no transaction, and a transaction that is
        // not complete, which xport() no longer completes. Nothing emitted.
        {
            auto const meta = runHook(env, bob, xportChecksWasm).second;
            BEAST_EXPECT(returnCodes(meta) == std::vector<std::uint64_t>{1111});
            BEAST_EXPECT(meta && !meta->isFieldPresent(sfHookEmissions));
        }
        BEAST_EXPECT(
            returnCodes(runHook(env, bob, xportNoReserveWasm).second) ==
            std::vector<std::uint64_t>{rc(PREREQUISITE_NOT_MET)});
    }

    void
    testPrepare()
    {
        testcase("xport_prepare");
        using namespace jtx;
        using enum hook_api::hook_return_code;

        Env env{*this, xahau(), supported_amendments() | featureExport};
        Account const bob{"bob"};
        env.fund(XRP(10000), bob);
        env.close();

        // An account with no signer list on the destination has nothing to
        // prepare an export for; and the API's own checks.
        BEAST_EXPECT(
            returnCodes(runHook(env, bob, xportPrepareWasm).second) ==
            std::vector<std::uint64_t>{1111});
        // so the hook that prepares and then exports stops at the first
        {
            auto const meta = runHook(env, bob, xportPreparedWasm).second;
            BEAST_EXPECT(
                returnCodes(meta) ==
                std::vector<std::uint64_t>{rc(DOESNT_EXIST)});
            BEAST_EXPECT(meta && !meta->isFieldPresent(sfHookEmissions));
        }

        // With a list there: never written in part, only what the schema
        // allows, and otherwise the transaction, longer than it went in.
        {
            auto const account = install(env, xportPrepareListedWasm);
            auto const closed = env.closed();
            OpenView view(&*closed, closed);
            makeExporter(view, account.id(), 3);
            BEAST_EXPECT(
                payHook(env, view, bob, account).first ==
                std::vector<std::uint64_t>{111});
        }

        // Prepared, then exported. What is emitted is what xport_prepare()
        // made of a type, an Amount and a Destination: from the hook's
        // account, unsigned, Sequence 0, bound back here, and priced for
        // itself and each of the three signers at `unit`; with a deadline
        // if a ledger has been proven there. And it is what applies: only
        // the ticket is added then, from the pool.
        auto const account = install(env, xportPreparedWasm);
        auto const check = [&](std::optional<std::uint32_t> proven,
                               std::int64_t unit) {
            auto const closed = env.closed();
            OpenView view(&*closed, closed);
            makeExporter(view, account.id(), 3);
            if (proven)
                makeProven(view, unit, unit, *proven, 1, unit);

            auto const [codes, emitted] = payHook(env, view, bob, account);
            BEAST_EXPECT(codes == std::vector<std::uint64_t>{32});
            if (!BEAST_EXPECT(emitted && emitted->getTxnType() == ttEXPORT))
                return;
            auto const& inner =
                emitted->peekAtField(sfExportedTxn).downcast<STObject>();
            BEAST_EXPECT(inner[sfTransactionType] == ttPAYMENT);
            BEAST_EXPECT(inner[sfAccount] == account.id());
            BEAST_EXPECT(inner.getFieldVL(sfSigningPubKey).empty());
            BEAST_EXPECT(inner[sfSequence] == 0u);
            BEAST_EXPECT(
                inner[sfOperationLimit] == env.app().config().NETWORK_ID);
            BEAST_EXPECT(!inner.isFieldPresent(sfNetworkID));
            BEAST_EXPECT(!inner.isFieldPresent(sfTicketSequence));
            BEAST_EXPECT(
                inner.getFieldAmount(sfFee).xrp() == XRPAmount{unit * 4});
            BEAST_EXPECT(
                inner[~sfLastLedgerSequence] ==
                (proven ? std::optional{*proven + Export::expiry}
                        : std::nullopt));

            auto const res =
                ripple::apply(env.app(), view, *emitted, tapNONE, env.journal);
            BEAST_EXPECT(res.applied && res.ter == tesSUCCESS);
            auto const st =
                view.read(keylet::shadowTicket(account.id(), 0, 10));
            if (!BEAST_EXPECT(st))
                return;
            BEAST_EXPECT((*st)[~sfExportTxnID] == emitted->getTransactionID());
            BEAST_EXPECT(
                (*st)[~sfLastLedgerSequence] == inner[~sfLastLedgerSequence]);
            auto const sle = view.read(
                keylet::exportedTxn(view.seq(), (*st)[sfTransactionHash]));
            if (!BEAST_EXPECT(sle))
                return;
            // as emitted, but for the ticket
            auto signedOver =
                sle->peekAtField(sfExportedTxn).downcast<STObject>();
            BEAST_EXPECT(signedOver[~sfTicketSequence] == 10u);
            signedOver.makeFieldAbsent(sfTicketSequence);
            Serializer was, is;
            inner.add(was);
            signedOver.add(is);
            BEAST_EXPECT(was.slice() == is.slice());
        };
        check(std::nullopt, Export::xrplBaseFee.drops());
        check(1000, 25);
    }

    void
    testFee()
    {
        testcase("xport_fee");
        using namespace jtx;

        Env env{*this, xahau(), supported_amendments() | featureExport};
        Account const bob{"bob"};
        env.fund(XRP(10000), bob);
        env.close();

        // Nothing proven of the XRPL: the unit is the reference fee, which
        // is what xport_prepare() would price by, and the rest is not there.
        // A code that is none is refused either way.
        BEAST_EXPECT(
            returnCodes(runHook(env, bob, xportFeeWasm).second) ==
            std::vector<std::uint64_t>{
                static_cast<std::uint64_t>(Export::xrplBaseFee.drops()) + 100 +
                1000});

        // With a ledger proven: the unit it left, and of that ledger its
        // median, index, transactions and burn.
        {
            auto const account = install(env, xportFeeAllWasm);
            auto const closed = env.closed();
            OpenView view(&*closed, closed);
            makeProven(view, 25, 30, 7, 3, 9);
            BEAST_EXPECT(
                payHook(env, view, bob, account).first ==
                std::vector<std::uint64_t>{
                    25 + 100 * 30 + 10'000 * 7 + 100'000'000ULL * 3 +
                    10'000'000'000ULL * 9});
        }
        {
            auto const account = install(env, xportFeeWasm);
            auto const closed = env.closed();
            OpenView view(&*closed, closed);
            makeProven(view, 25, 30, 7, 3, 9);
            BEAST_EXPECT(
                payHook(env, view, bob, account).first ==
                std::vector<std::uint64_t>{25 + 1000});
        }
    }

    void
    testDisabled()
    {
        testcase("xport disabled");
        using namespace jtx;

        Env env{*this, supported_amendments() - featureExport};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // the APIs are not available to SetHook
        for (auto const* wasm : {&xportWasm, &xportPrepareWasm, &xportFeeWasm})
            env(hook(alice, {{hso(*wasm)}}, 0),
                fee(XRP(100)),
                ter(temMALFORMED));
    }

public:
    void
    run() override
    {
        testXport();
        testPrepare();
        testFee();
        testDisabled();
    }
};

BEAST_DEFINE_TESTSUITE(ExportHook, app, ripple);

}  // namespace test
}  // namespace ripple
