//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 Xahau

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.
*/
//==============================================================================

#include <test/jtx.h>
#include <test/jtx/xpop.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/ledger/OpenView.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/Sign.h>

#include <limits>

namespace ripple {
namespace test {

class UNLReportMember_test : public beast::unit_test::suite
{
    struct Validator
    {
        PublicKey masterPublic;
        SecretKey masterSecret;
        PublicKey signingPublic;
        SecretKey signingSecret;
    };

    static Validator
    validator()
    {
        auto const masterSecret = randomSecretKey();
        auto const masterPublic =
            derivePublicKey(KeyType::ed25519, masterSecret);
        auto const [signingPublic, signingSecret] =
            randomKeyPair(KeyType::secp256k1);
        return {masterPublic, masterSecret, signingPublic, signingSecret};
    }

    static std::string
    manifest(Validator const& v, std::uint32_t sequence)
    {
        return jtx::xpop::makeManifestRaw(
            v.masterPublic,
            v.masterSecret,
            v.signingPublic,
            v.signingSecret,
            sequence);
    }

    static std::string
    manifest(
        Validator const& v,
        PublicKey const& signingPublic,
        SecretKey const& signingSecret,
        std::uint32_t sequence)
    {
        return jtx::xpop::makeManifestRaw(
            v.masterPublic,
            v.masterSecret,
            signingPublic,
            signingSecret,
            sequence);
    }

    static std::string
    revocation(Validator const& v)
    {
        STObject st(sfGeneric);
        st[sfSequence] = std::numeric_limits<std::uint32_t>::max();
        st[sfPublicKey] = v.masterPublic;
        sign(
            st,
            HashPrefix::manifest,
            KeyType::ed25519,
            v.masterSecret,
            sfMasterSignature);
        Serializer s;
        st.add(s);
        return std::string(static_cast<char const*>(s.data()), s.size());
    }

    static STTx
    memberTx(std::string const& blob)
    {
        return STTx(ttUNL_REPORT_MEMBER, [&](auto& obj) {
            obj.setAccountID(sfAccount, AccountID{});
            obj.setFieldU32(sfSequence, 0);
            obj.setFieldAmount(sfFee, STAmount{});
            obj.setFieldVL(sfBlob, makeSlice(blob));
        });
    }

    static std::shared_ptr<Ledger>
    parentLedger(jtx::Env& env, std::vector<PublicKey> const& members)
    {
        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto ledger = std::make_shared<Ledger>(
            *parent, env.app().timeKeeper().closeTime());
        auto report = std::make_shared<SLE>(keylet::UNLReport());
        std::vector<STObject> active;
        for (auto const& key : members)
        {
            active.push_back(STObject::makeInnerObject(sfActiveValidator));
            active.back().setFieldVL(sfPublicKey, key);
        }
        report->setFieldArray(
            sfActiveValidators, STArray(active, sfActiveValidators));

        OpenView accum(&*ledger);
        accum.rawInsert(report);
        accum.apply(*ledger);
        return ledger;
    }

    TER
    apply(jtx::Env& env, OpenView& view, STTx const& tx)
    {
        return ripple::apply(env.app(), view, tx, tapNONE, env.journal).ter;
    }

    static void
    setMembers(OpenView& view, std::vector<PublicKey> const& members)
    {
        auto report = std::make_shared<SLE>(*view.read(keylet::UNLReport()));
        std::vector<STObject> active;
        for (auto const& key : members)
        {
            active.push_back(STObject::makeInnerObject(sfActiveValidator));
            active.back().setFieldVL(sfPublicKey, key);
        }
        report->setFieldArray(
            sfActiveValidators, STArray(active, sfActiveValidators));
        view.rawReplace(report);
    }

    void
    testAmendmentAndAdmission()
    {
        testcase("amendment and parent admission");
        auto const v = validator();

        {
            jtx::Env env(
                *this, jtx::supported_amendments() - featureUNLReportV2);
            auto parent = parentLedger(env, {v.masterPublic});
            OpenView view(&*parent);
            BEAST_EXPECT(
                apply(env, view, memberTx(manifest(v, 1))) == temDISABLED);
            BEAST_EXPECT(!view.read(keylet::UNLReportMember(v.masterPublic)));
        }

        {
            jtx::Env env(
                *this, jtx::supported_amendments() | featureUNLReportV2);
            auto parent = parentLedger(env, {});
            OpenView view(&*parent);
            BEAST_EXPECT(
                apply(env, view, memberTx(manifest(v, 1))) == tefBAD_AUTH);
            BEAST_EXPECT(!view.read(keylet::UNLReportMember(v.masterPublic)));
        }

        {
            jtx::Env env(
                *this, jtx::supported_amendments() | featureUNLReportV2);
            auto parent = parentLedger(env, {});
            OpenView view(&*parent);
            setMembers(view, {v.masterPublic});
            BEAST_EXPECT(
                apply(env, view, memberTx(manifest(v, 1))) == tefBAD_AUTH);
            BEAST_EXPECT(!view.read(keylet::UNLReportMember(v.masterPublic)));
        }
    }

    void
    testMalformedEvidence()
    {
        testcase("malformed evidence is rejected without state changes");
        jtx::Env env(*this, jtx::supported_amendments() | featureUNLReportV2);
        auto const v = validator();
        auto parent = parentLedger(env, {v.masterPublic});
        OpenView view(&*parent);

        BEAST_EXPECT(
            apply(env, view, memberTx(std::string(1, '\0'))) == temMALFORMED);
        BEAST_EXPECT(
            apply(env, view, memberTx(std::string(2049, 'x'))) == temMALFORMED);

        auto badSignature = manifest(v, 1);
        badSignature.back() ^= 1;
        BEAST_EXPECT(
            apply(env, view, memberTx(badSignature)) == temBAD_SIGNATURE);
        BEAST_EXPECT(!view.read(keylet::UNLReportMember(v.masterPublic)));
    }

    void
    testRotationAndRevocation()
    {
        testcase("rotation, idempotence, and revocation");
        jtx::Env env(*this, jtx::supported_amendments() | featureUNLReportV2);
        auto const v = validator();
        auto parent = parentLedger(env, {v.masterPublic});
        OpenView view(&*parent);

        auto const first = memberTx(manifest(v, 1));
        BEAST_EXPECT(apply(env, view, first) == tesSUCCESS);
        BEAST_EXPECT(apply(env, view, first) == tefALREADY);

        auto const [nextPublic, nextSecret] = randomKeyPair(KeyType::secp256k1);
        BEAST_EXPECT(
            apply(
                env, view, memberTx(manifest(v, nextPublic, nextSecret, 2))) ==
            tesSUCCESS);

        auto sle = view.read(keylet::UNLReportMember(v.masterPublic));
        BEAST_EXPECT(sle);
        BEAST_EXPECT(sle && sle->getFieldU32(sfSequence) == 2);
        BEAST_EXPECT(
            sle &&
            PublicKey{makeSlice(sle->getFieldVL(sfSigningPubKey))} ==
                nextPublic);

        BEAST_EXPECT(apply(env, view, memberTx(revocation(v))) == tesSUCCESS);
        sle = view.read(keylet::UNLReportMember(v.masterPublic));
        BEAST_EXPECT(sle && !sle->isFieldPresent(sfSigningPubKey));
        BEAST_EXPECT(
            apply(
                env, view, memberTx(manifest(v, nextPublic, nextSecret, 3))) ==
            tefPAST_SEQ);
    }

    void
    testInactiveMemberCanRevoke()
    {
        testcase("inactive historical member can only revoke");
        jtx::Env env(*this, jtx::supported_amendments() | featureUNLReportV2);
        auto const v = validator();
        auto ledger = parentLedger(env, {v.masterPublic});
        {
            OpenView view(&*ledger);
            BEAST_EXPECT(
                apply(env, view, memberTx(manifest(v, 1))) == tesSUCCESS);
            view.apply(*ledger);
        }

        auto inactive = std::make_shared<Ledger>(
            *ledger, env.app().timeKeeper().closeTime());
        {
            OpenView view(&*inactive);
            setMembers(view, {});
            view.apply(*inactive);
        }

        OpenView view(&*inactive);
        BEAST_EXPECT(apply(env, view, memberTx(manifest(v, 2))) == tefBAD_AUTH);
        BEAST_EXPECT(apply(env, view, memberTx(revocation(v))) == tesSUCCESS);
        auto const sle = view.read(keylet::UNLReportMember(v.masterPublic));
        BEAST_EXPECT(sle && !sle->isFieldPresent(sfSigningPubKey));
        BEAST_EXPECT(sle && Manifest::revoked(sle->getFieldU32(sfSequence)));
    }

    void
    testEquivocationIsOrderIndependent()
    {
        testcase("equal-sequence equivocation freezes deterministically");
        jtx::Env env(*this, jtx::supported_amendments() | featureUNLReportV2);
        auto const v = validator();
        auto const [otherPublic, otherSecret] =
            randomKeyPair(KeyType::secp256k1);
        auto const a = memberTx(manifest(v, 1));
        auto const b = memberTx(manifest(v, otherPublic, otherSecret, 1));

        auto applyPair = [&](STTx const& first, STTx const& second) {
            auto parent = parentLedger(env, {v.masterPublic});
            OpenView view(&*parent);
            BEAST_EXPECT(apply(env, view, first) == tesSUCCESS);
            BEAST_EXPECT(apply(env, view, second) == tesSUCCESS);
            auto const sle = view.read(keylet::UNLReportMember(v.masterPublic));
            BEAST_EXPECT(
                sle &&
                (sle->getFlags() & lsfUNLReportMemberEquivocationFreeze));
            return std::make_pair(
                sle->getFieldH256(sfDigest), sle->getFieldVL(sfBlob));
        };

        BEAST_EXPECT(applyPair(a, b) == applyPair(b, a));

        auto parent = parentLedger(env, {v.masterPublic});
        OpenView view(&*parent);
        BEAST_EXPECT(apply(env, view, a) == tesSUCCESS);
        BEAST_EXPECT(apply(env, view, b) == tesSUCCESS);
        BEAST_EXPECT(
            apply(
                env,
                view,
                memberTx(manifest(v, otherPublic, otherSecret, 2))) ==
            tesSUCCESS);
        auto const sle = view.read(keylet::UNLReportMember(v.masterPublic));
        BEAST_EXPECT(sle && sle->getFlags() == 0);
    }

    void
    testSigningKeyCollision()
    {
        testcase("cross-member signing-key collision freezes both records");
        jtx::Env env(*this, jtx::supported_amendments() | featureUNLReportV2);
        auto const a = validator();
        auto const b = validator();
        auto parent = parentLedger(env, {a.masterPublic, b.masterPublic});
        OpenView view(&*parent);

        BEAST_EXPECT(apply(env, view, memberTx(manifest(a, 1))) == tesSUCCESS);
        BEAST_EXPECT(
            apply(
                env,
                view,
                memberTx(manifest(b, a.signingPublic, a.signingSecret, 1))) ==
            tesSUCCESS);

        auto const aSle = view.read(keylet::UNLReportMember(a.masterPublic));
        auto const bSle = view.read(keylet::UNLReportMember(b.masterPublic));
        BEAST_EXPECT(
            aSle &&
            (aSle->getFlags() & lsfUNLReportMemberSigningKeyCollisionFreeze));
        BEAST_EXPECT(
            bSle &&
            (bSle->getFlags() & lsfUNLReportMemberSigningKeyCollisionFreeze));

        BEAST_EXPECT(apply(env, view, memberTx(manifest(b, 2))) == tesSUCCESS);
        BEAST_EXPECT(
            view.read(keylet::UNLReportMember(b.masterPublic))->getFlags() ==
            0);
        BEAST_EXPECT(apply(env, view, memberTx(manifest(a, 2))) == tesSUCCESS);
        BEAST_EXPECT(
            view.read(keylet::UNLReportMember(a.masterPublic))->getFlags() ==
            0);
    }

public:
    void
    run() override
    {
        testAmendmentAndAdmission();
        testMalformedEvidence();
        testRotationAndRevocation();
        testInactiveMemberCanRevoke();
        testEquivocationIsOrderIndependent();
        testSigningKeyCollision();
    }
};

BEAST_DEFINE_TESTSUITE(UNLReportMember, consensus, ripple);

}  // namespace test
}  // namespace ripple
