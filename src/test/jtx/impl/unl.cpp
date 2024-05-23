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

#include <ripple/app/tx/apply.h>
#include <ripple/protocol/jss.h>
#include <test/jtx/unl.h>

namespace ripple {
namespace test {
namespace unl {

bool
negUnlSizeTest(
    std::shared_ptr<Ledger const> const& l,
    size_t size,
    bool hasToDisable,
    bool hasToReEnable)
{
    bool sameSize = l->negativeUNL().size() == size;
    bool sameToDisable =
        (l->validatorToDisable() != std::nullopt) == hasToDisable;
    bool sameToReEnable =
        (l->validatorToReEnable() != std::nullopt) == hasToReEnable;

    return sameSize && sameToDisable && sameToReEnable;
}

bool
applyAndTestResult(jtx::Env& env, OpenView& view, STTx const& tx, bool pass)
{
    auto res = apply(env.app(), view, tx, ApplyFlags::tapNONE, env.journal);
    if (pass)
        return res.first == tesSUCCESS;
    else
        return res.first == tefFAILURE || res.first == temDISABLED;
}

std::size_t
countTx(std::shared_ptr<SHAMap> const& txSet)
{
    /*uint64_t counter = 0;
    if (txSet)
    for (auto const& item : *txSet)
    {

        SerialIter sit(item.slice());
        auto tx = std::make_shared<STTx
    const>(SerialIter{sit.getSlice(sit.getVLDataLength())});

        if (tx->getFieldU16(sfTransactionType) == ttUNL_MODIFY)
            counter++;
    }
    */

    std::size_t count = 0;
    for (auto i = txSet->begin(); i != txSet->end(); ++i)
    {
        // RH TODO: why does the above parse??
        auto raw = i->slice();
        if (raw[0] == 0x12U && raw[1] == 0 && raw[2] == 0x66U)
            count++;
    }
    return count;
};

STTx
createTx(bool disabling, LedgerIndex seq, PublicKey const& txKey)
{
    auto fill = [&](auto& obj) {
        obj.setFieldU8(sfUNLModifyDisabling, disabling ? 1 : 0);
        obj.setFieldU32(sfLedgerSequence, seq);
        obj.setFieldVL(sfUNLModifyValidator, txKey);
    };
    return STTx(ttUNL_MODIFY, fill);
}

STTx
createUNLReportTx(
    LedgerIndex seq,
    PublicKey const& importKey,
    PublicKey const& valKey)
{
    auto fill = [&](auto& obj) {
        obj.setFieldU32(sfLedgerSequence, seq);
        obj.set(([&]() {
            auto inner = std::make_unique<STObject>(sfActiveValidator);
            inner->setFieldVL(sfPublicKey, valKey);
            return inner;
        })());
        obj.set(([&]() {
            auto inner = std::make_unique<STObject>(sfImportVLKey);
            inner->setFieldVL(sfPublicKey, importKey);
            return inner;
        })());
    };
    return STTx(ttUNL_REPORT, fill);
}

}  // namespace unl
}  // namespace test
}  // namespace ripple
