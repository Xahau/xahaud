//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES WHATSOEVER
    RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
    CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
    CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <test/jtx.h>
#include <xrpld/app/tx/detail/ExportLedgerOps.h>
#include <xrpld/ledger/Sandbox.h>
#include <xrpl/protocol/Protocol.h>

#include <vector>

namespace ripple {
namespace test {

struct ExportLatch_test : beast::unit_test::suite
{
    static std::shared_ptr<SLE>
    makeLatch(AccountID const& account, std::uint32_t ordinal)
    {
        uint256 const origin{ordinal + 1};
        auto latch =
            std::make_shared<SLE>(keylet::shadowTicket(account, origin));
        latch->setAccountID(sfAccount, account);
        latch->setFieldU32(sfTicketSequence, 50'000 + ordinal);
        latch->setFieldH256(sfTransactionHash, origin);
        latch->setFieldH256(sfDigest, uint256{10'000 + ordinal});
        latch->setFieldU32(sfLedgerSequence, 4'000'000 + ordinal);
        latch->setFieldH256(sfExportUniverseHash, uint256{20'000 + ordinal});
        latch->setFieldVL(sfExportCommittee, Blob{0x01});
        return latch;
    }

    void
    testDirectoryLifecycle()
    {
        testcase("enhanced latch directory lifecycle");

        using namespace jtx;
        Account const alice{"alice"};
        Env env{*this};
        env.fund(XRP(10'000), alice);
        env.close();

        beast::Journal j{beast::Journal::getNullSink()};
        Sandbox sb{env.closed().get(), tapNONE};
        std::vector<Keylet> latches;
        latches.reserve(dirNodeMaxEntries + 1);

        for (std::uint32_t i = 0; i <= dirNodeMaxEntries; ++i)
        {
            auto latch = makeLatch(alice.id(), i);
            latches.emplace_back(keylet::unchecked(latch->key()));
            auto const ter =
                ExportLedgerOps::insertPendingExportLatch(sb, latch, j);
            BEAST_EXPECTS(
                isTesSuccess(ter),
                "ordinal=" + std::to_string(i) + " ter=" + transToken(ter));
        }

        auto const account = sb.read(keylet::account(alice.id()));
        auto const pending = sb.read(keylet::pendingExports());
        BEAST_EXPECT(account);
        BEAST_EXPECT(pending);
        BEAST_EXPECT(account->getFieldU16(sfExportCount) == 33);
        BEAST_EXPECT(account->getFieldU32(sfOwnerCount) == 33);
        BEAST_EXPECT(pending->getFieldU16(sfExportCount) == 33);
        BEAST_EXPECT(pending->getFieldV256(sfIndexes).size() == 32);

        auto const first = sb.read(latches.front());
        auto const last = sb.read(latches.back());
        BEAST_EXPECT(first && first->getFieldU64(sfOwnerNode) == 0);
        BEAST_EXPECT(first && first->isFieldPresent(sfExportNode));
        BEAST_EXPECT(first && first->getFieldU64(sfExportNode) == 0);
        BEAST_EXPECT(last && last->getFieldU64(sfOwnerNode) == 1);
        BEAST_EXPECT(last && last->getFieldU64(sfExportNode) == 1);

        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::removePendingExportLink(sb, latches.front(), j)));
        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::removePendingExportLink(sb, latches.back(), j)));
        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::removePendingExportLink(sb, latches.back(), j)));

        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::eraseExportLatch(sb, latches.front(), j)));
        BEAST_EXPECT(
            isTesSuccess(ExportLedgerOps::eraseExportLatch(sb, latches[1], j)));

        auto const afterAccount = sb.read(keylet::account(alice.id()));
        auto const afterPending = sb.read(keylet::pendingExports());
        BEAST_EXPECT(afterAccount->getFieldU16(sfExportCount) == 31);
        BEAST_EXPECT(afterAccount->getFieldU32(sfOwnerCount) == 31);
        BEAST_EXPECT(afterPending->getFieldU16(sfExportCount) == 31);

        Sandbox restarted{&sb};
        std::vector<uint256> recovered;
        forEachItem(
            restarted,
            keylet::pendingExports(),
            [&](std::shared_ptr<SLE const> const& sle) {
                recovered.push_back(sle->key());
            });
        BEAST_EXPECT(recovered.size() == 30);

        for (std::size_t i = 2; i < latches.size(); ++i)
            BEAST_EXPECT(isTesSuccess(
                ExportLedgerOps::eraseExportLatch(sb, latches[i], j)));

        auto const finalAccount = sb.read(keylet::account(alice.id()));
        auto const finalPending = sb.read(keylet::pendingExports());
        BEAST_EXPECT(finalAccount->getFieldU16(sfExportCount) == 0);
        BEAST_EXPECT(finalAccount->getFieldU32(sfOwnerCount) == 0);
        BEAST_EXPECT(finalPending);
        BEAST_EXPECT(finalPending->getFieldU16(sfExportCount) == 0);
        BEAST_EXPECT(finalPending->getFieldV256(sfIndexes).empty());
        BEAST_EXPECT(finalPending->getFieldU64(sfIndexNext) == 0);
        BEAST_EXPECT(finalPending->getFieldU64(sfIndexPrevious) == 0);
    }

    void
    run() override
    {
        testDirectoryLifecycle();
    }
};

BEAST_DEFINE_TESTSUITE(ExportLatch, app, ripple);

}  // namespace test
}  // namespace ripple
