//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL Labs

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

#ifndef RIPPLE_TEST_JTX_UNL_H_INCLUDED
#define RIPPLE_TEST_JTX_UNL_H_INCLUDED

#include <ripple/protocol/STAmount.h>
#include <test/jtx/Account.h>
#include <test/jtx/Env.h>

namespace ripple {
namespace test {

namespace unl {

/**
 * Test the size of the negative UNL in a ledger,
 * also test if the ledger has ToDisalbe and/or ToReEnable
 *
 * @param l the ledger
 * @param size the expected negative UNL size
 * @param hasToDisable if expect ToDisable in ledger
 * @param hasToReEnable if expect ToDisable in ledger
 * @return true if meet all three expectation
 */
bool
negUnlSizeTest(
    std::shared_ptr<Ledger const> const& l,
    size_t size,
    bool hasToDisable,
    bool hasToReEnable);

/**
 * Try to apply a ttUNL_MODIFY Tx, and test the apply result
 *
 * @param env the test environment
 * @param view the OpenView of the ledger
 * @param tx the ttUNL_MODIFY Tx
 * @param pass if the Tx should be applied successfully
 * @return true if meet the expectation of apply result
 */
bool
applyAndTestResult(jtx::Env& env, OpenView& view, STTx const& tx, bool pass);

/**
 * Count the number of Tx in a TxSet
 *
 * @param txSet the TxSet
 * @return the number of Tx
 */
std::size_t
countTx(std::shared_ptr<SHAMap> const& txSet);

/**
 * Create ttUNL_MODIFY Tx
 *
 * @param disabling disabling or re-enabling a validator
 * @param seq current ledger seq
 * @param txKey the public key of the validator
 * @return the ttUNL_MODIFY Tx
 */
STTx
createTx(bool disabling, LedgerIndex seq, PublicKey const& txKey);

/**
 * Create ttUNL_REPORT Tx
 *
 * @param seq current ledger seq
 * @param importKey the public key of the import network
 * @param txKey the public key of the validator
 * @return the ttUNL_REPORT Tx
 */
STTx
createUNLReportTx(
    LedgerIndex seq,
    PublicKey const& importKey,
    PublicKey const& valKey);

}  // namespace unl

}  // namespace test
}  // namespace ripple

#endif  // RIPPLE_TEST_JTX_UNL_H_INCLUDED
