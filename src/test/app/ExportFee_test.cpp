//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
    IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <test/jtx.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/tx/apply.h>
#include <xrpl/protocol/ExportCommittee.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/ExportOriginMemo.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/jss.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace ripple {
namespace test {

struct ExportFee_test : public beast::unit_test::suite
{
    static std::unique_ptr<Config>
    exportTestConfig()
    {
        auto cfg = jtx::envconfig(jtx::validator, "");
        cfg->NETWORK_ID = 21337;
        return cfg;
    }

    void
    seedUNLReportLedger(jtx::Env& env, PublicKey const& masterKey)
    {
        env.app().openLedger().modify(
            [&](OpenView& view, beast::Journal) -> bool {
                STTx tx(ttUNL_REPORT, [&](auto& obj) {
                    obj.setFieldU32(sfLedgerSequence, env.current()->seq());
                    auto active = std::make_unique<STObject>(sfActiveValidator);
                    active->setFieldVL(sfPublicKey, masterKey);
                    obj.set(std::move(active));
                });
                auto const txID = tx.getTransactionID();
                auto serialized = std::make_shared<Serializer>();
                tx.add(*serialized);
                env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                view.rawTxInsert(txID, std::move(serialized), nullptr);
                return true;
            });

        BEAST_EXPECT(env.close(
            env.now() + std::chrono::seconds{5}, std::chrono::milliseconds{0}));
        BEAST_EXPECT(env.le(keylet::UNLReport()));
    }

    static STObject
    exportedPayment(
        AccountID const& source,
        AccountID const& destination,
        LedgerIndex const first,
        LedgerIndex const last)
    {
        STObject target(sfExportedTxn);
        target.setFieldU16(sfTransactionType, ttPAYMENT);
        target.setFieldU32(sfFlags, tfFullyCanonicalSig);
        target.setFieldU32(sfSequence, 0);
        target.setFieldU32(sfTicketSequence, 1);
        target.setFieldU32(sfNetworkID, 1);
        target.setFieldU32(sfFirstLedgerSequence, first);
        target.setFieldU32(sfLastLedgerSequence, last);
        target.setFieldAmount(sfAmount, XRPAmount{1'000'000});
        target.setFieldAmount(sfFee, XRPAmount{10});
        target.setFieldVL(sfSigningPubKey, Blob{});
        target.setAccountID(sfAccount, source);
        target.setAccountID(sfDestination, destination);
        return target;
    }

    static Json::Value
    exportIntent(
        jtx::Account const& account,
        STObject const& target,
        LedgerIndex const last,
        Blob const& committee)
    {
        Json::Value tx;
        tx[jss::TransactionType] = jss::Export;
        tx[jss::Account] = account.human();
        tx[jss::LastLedgerSequence] = last;
        tx[sfExportedTxn.jsonName] = target.getJson(JsonOptions::none);
        tx[sfExportCommitteeHash.jsonName] =
            to_string(exportCommitteeHash(makeSlice(committee)));
        tx[sfExportCommittee.jsonName] = strHex(committee);
        return tx;
    }

    static void
    addHookParameterPayload(STObject& target, std::size_t count)
    {
        STArray parameters(sfHookParameters);
        for (std::size_t i = 0; i < count; ++i)
        {
            STObject parameter(sfHookParameter);
            parameter.setFieldVL(
                sfHookParameterName, Blob{static_cast<std::uint8_t>(i + 1)});
            parameter.setFieldVL(sfHookParameterValue, Blob(256, 0xA5));
            parameters.emplace_back(std::move(parameter));
        }
        target.setFieldArray(sfHookParameters, std::move(parameters));
    }

    static XRPAmount
    expectedFee(ReadView const& view, STTx const& tx, std::size_t committeeSize)
    {
        Serializer target;
        tx.peekAtField(sfExportedTxn).downcast<STObject>().add(target);

        auto const witnessBytes = target.size() +
            ExportLimits::feeWitnessFixedAllowanceBytes +
            committeeSize * ExportLimits::feeWitnessSignerAllowanceBytes;
        auto const workUnits = committeeSize *
                ExportLimits::feeSharePublicationRounds *
                ExportLimits::feeWorkUnitsPerSharePublication +
            committeeSize * ExportLimits::feeWorkUnitsPerWitnessSignature +
            ((witnessBytes + ExportLimits::feeWitnessChunkBytes - 1) /
             ExportLimits::feeWitnessChunkBytes) *
                ExportLimits::feeWorkUnitsPerWitnessChunk;
        auto const unrounded = workUnits * view.fees().base.drops() +
            witnessBytes * ExportLimits::feePermanentWitnessByteDrops;
        auto const surcharge =
            ((unrounded + ExportLimits::feeSurchargeRoundDrops - 1) /
             ExportLimits::feeSurchargeRoundDrops) *
            ExportLimits::feeSurchargeRoundDrops;
        return view.fees().base +
            XRPAmount{static_cast<std::int64_t>(surcharge)};
    }

    void
    testIntentFeeAndEnforcement(FeatureBitset const& features)
    {
        testcase("Export intent fee prices publication and witness work");

        using namespace jtx;
        Env env{*this, exportTestConfig(), features};
        Account const alice{"alice"};
        Account const carol{"carol"};
        env.fund(XRP(10'000), alice, carol);
        env.close();

        auto const& validatorKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(validatorKeys.keys);
        if (!validatorKeys.keys)
            return;
        auto const committee =
            serializeExportCommittee({validatorKeys.keys->masterPublicKey});
        seedUNLReportLedger(env, validatorKeys.keys->masterPublicKey);

        auto const sequence = env.current()->seq();
        auto const last = sequence + ExportLimits::maxAdmissionWindowLedgers;
        auto const target =
            exportedPayment(alice.id(), carol.id(), sequence + 1, last);
        auto const intent = exportIntent(alice, target, last, committee);
        auto const prepared = env.jt(intent);
        auto const required = calculateBaseFee(*env.current(), *prepared.stx);
        auto const expected = expectedFee(*env.current(), *prepared.stx, 1);

        BEAST_EXPECT(required == expected);
        // Any positive surcharge rounds to at least one provisional fee unit.
        BEAST_EXPECT(
            required >= env.current()->fees().base +
                XRPAmount{static_cast<std::int64_t>(
                    ExportLimits::feeSurchargeRoundDrops)});

        env(intent, fee(required - drops(1)), ter(telINSUF_FEE_P));
        env(intent, fee(required), ter(tesSUCCESS));
        env.close();

        auto oversizedTarget = exportedPayment(
            alice.id(), carol.id(), env.current()->seq() + 1, last + 1);
        addHookParameterPayload(oversizedTarget, 8);
        auto projectedTarget = oversizedTarget;
        auto const projected = ExportOriginMemo::releaseForm(
            STTx{std::move(projectedTarget)},
            ExportOriginMemo::Origin{21337, 1, uint256{}},
            ExportOriginMemo::Anchor{0, uint256{}});
        BEAST_EXPECT(projected);
        if (!projected)
            return;
        Serializer projectedBytes;
        projected.value().add(projectedBytes);
        BEAST_EXPECT(
            projectedBytes.size() > ExportLimits::maxExportReleaseTargetBytes);
        auto const oversizedIntent =
            exportIntent(alice, oversizedTarget, last + 1, committee);
        auto const oversizedFee =
            calculateBaseFee(*env.current(), *env.jt(oversizedIntent).stx);
        env(oversizedIntent, fee(oversizedFee), ter(temMALFORMED));
    }

    void
    testFeeShape(FeatureBitset const& features)
    {
        testcase("Export fee scales by committee while admin stays ordinary");

        using namespace jtx;
        Env env{*this, exportTestConfig(), features};
        Account const alice{"alice"};
        Account const carol{"carol"};
        env.fund(XRP(10'000), alice, carol);
        env.close();

        std::vector<PublicKey> members;
        members.reserve(ExportLimits::maxCommitteeMembers);
        for (std::size_t i = 0; i < ExportLimits::maxCommitteeMembers; ++i)
            members.push_back(randomKeyPair(KeyType::secp256k1).first);
        auto const largeCommittee = serializeExportCommittee(members);
        auto const oneMember = serializeExportCommittee({members.front()});

        Json::Value setup;
        setup[jss::TransactionType] = jss::Export;
        setup[jss::Account] = alice.human();
        setup[sfExportCommittee.jsonName] = strHex(largeCommittee);
        auto const setupFee =
            calculateBaseFee(*env.current(), *env.jt(setup).stx);
        BEAST_EXPECT(setupFee == env.current()->fees().base);

        Json::Value oneMemberSetup = setup;
        oneMemberSetup[sfExportCommittee.jsonName] = strHex(oneMember);
        env(oneMemberSetup, fee(env.current()->fees().base), ter(tesSUCCESS));
        env.close();

        auto const sequence = env.current()->seq();
        auto const last = sequence + ExportLimits::maxAdmissionWindowLedgers;
        auto const target =
            exportedPayment(alice.id(), carol.id(), sequence + 1, last);
        auto const smallIntent = exportIntent(alice, target, last, oneMember);
        auto const largeIntent =
            exportIntent(alice, target, last, largeCommittee);
        auto const smallFee =
            calculateBaseFee(*env.current(), *env.jt(smallIntent).stx);
        auto const largeFee =
            calculateBaseFee(*env.current(), *env.jt(largeIntent).stx);

        auto residentIntent = smallIntent;
        residentIntent.removeMember(sfExportCommittee.jsonName);
        auto const residentFee =
            calculateBaseFee(*env.current(), *env.jt(residentIntent).stx);

        BEAST_EXPECT(largeFee > smallFee);
        BEAST_EXPECT(residentFee == smallFee);
        BEAST_EXPECT(
            largeFee ==
            expectedFee(
                *env.current(),
                *env.jt(largeIntent).stx,
                ExportLimits::maxCommitteeMembers));
    }

    void
    run() override
    {
        auto const features = jtx::supported_amendments() | featureExport;
        testIntentFeeAndEnforcement(features);
        testFeeShape(features);
    }
};

BEAST_DEFINE_TESTSUITE(ExportFee, app, ripple);

}  // namespace test
}  // namespace ripple
