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
#include <xrpld/app/misc/UNLReportMember.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/ledger/OpenView.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/Sign.h>

#include <algorithm>
#include <array>
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

    static Manifest
    parsed(std::string const& raw)
    {
        auto manifest = deserializeManifest(raw);
        if (!manifest)
            Throw<std::logic_error>("test manifest must deserialize");
        return std::move(*manifest);
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

    static std::shared_ptr<Ledger>
    parentLedgerWithoutActiveValidators(jtx::Env& env)
    {
        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto ledger = std::make_shared<Ledger>(
            *parent, env.app().timeKeeper().closeTime());
        auto report = std::make_shared<SLE>(keylet::UNLReport());

        OpenView accum(&*ledger);
        accum.rawInsert(report);
        accum.apply(*ledger);
        return ledger;
    }

    static Blob
    serializedBlob(std::string const& serialized)
    {
        return Blob{serialized.begin(), serialized.end()};
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

    template <class F>
    static void
    mutateMember(OpenView& view, PublicKey const& masterKey, F&& f)
    {
        auto sle = std::make_shared<SLE>(
            *view.read(keylet::UNLReportMember(masterKey)));
        f(*sle);
        view.rawReplace(sle);
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
    testBindingViewAvailability()
    {
        testcase("binding view unavailable without active universe");
        auto const v = validator();

        {
            jtx::Env env(
                *this, jtx::supported_amendments() - featureUNLReportV2);
            auto parent = parentLedger(env, {v.masterPublic});
            BEAST_EXPECT(!buildUNLReportMemberBindingView(*parent));
        }

        {
            jtx::Env env(
                *this, jtx::supported_amendments() | featureUNLReportV2);
            auto const parent = env.app().getLedgerMaster().getClosedLedger();
            BEAST_EXPECT(!buildUNLReportMemberBindingView(*parent));
        }

        {
            jtx::Env env(
                *this, jtx::supported_amendments() | featureUNLReportV2);
            auto parent = parentLedgerWithoutActiveValidators(env);
            BEAST_EXPECT(!buildUNLReportMemberBindingView(*parent));
        }

        {
            jtx::Env env(
                *this, jtx::supported_amendments() | featureUNLReportV2);
            auto parent = parentLedger(env, {});
            BEAST_EXPECT(!buildUNLReportMemberBindingView(*parent));
        }
    }

    void
    testBindingViewActiveMasters()
    {
        testcase("binding view sorts and dedupes active masters");
        jtx::Env env(*this, jtx::supported_amendments() | featureUNLReportV2);
        auto const a = validator();
        auto const b = validator();
        auto const c = validator();
        std::vector<PublicKey> expected{
            a.masterPublic, b.masterPublic, c.masterPublic};
        std::sort(expected.begin(), expected.end());

        auto parent = parentLedger(
            env,
            {c.masterPublic,
             a.masterPublic,
             b.masterPublic,
             c.masterPublic,
             a.masterPublic});
        auto bindings = buildUNLReportMemberBindingView(*parent);
        if (!BEAST_EXPECT(bindings.has_value()))
            return;

        BEAST_EXPECT(bindings->members.size() == expected.size());
        for (std::size_t i = 0;
             i < expected.size() && i < bindings->members.size();
             ++i)
        {
            BEAST_EXPECT(bindings->members[i].masterKey == expected[i]);
            BEAST_EXPECT(
                bindings->members[i].resolution ==
                UNLReportMemberBindingResolution::missing);
            BEAST_EXPECT(!bindings->members[i].usableSigningBinding());
            BEAST_EXPECT(
                bindings->findMaster(expected[i]) == &bindings->members[i]);
        }
    }

    void
    testBindingViewResolvedAndMissing()
    {
        testcase("binding view resolves applied records and exposes missing");
        jtx::Env env(*this, jtx::supported_amendments() | featureUNLReportV2);
        auto const resolvedValidator = validator();
        auto const missingValidator = validator();
        auto parent = parentLedger(
            env,
            {resolvedValidator.masterPublic, missingValidator.masterPublic});
        OpenView view(&*parent);

        auto const raw = manifest(resolvedValidator, 7);
        auto const parsedManifest = parsed(raw);
        BEAST_EXPECT(apply(env, view, memberTx(raw)) == tesSUCCESS);

        auto bindings = buildUNLReportMemberBindingView(view);
        if (!BEAST_EXPECT(bindings.has_value()))
            return;

        auto const* resolved =
            bindings->findMaster(resolvedValidator.masterPublic);
        BEAST_EXPECT(resolved != nullptr);
        if (resolved)
        {
            BEAST_EXPECT(
                resolved->resolution ==
                UNLReportMemberBindingResolution::resolved);
            BEAST_EXPECT(resolved->masterKey == resolvedValidator.masterPublic);
            BEAST_EXPECT(resolved->signingKey.has_value());
            BEAST_EXPECT(
                resolved->signingKey &&
                *resolved->signingKey == resolvedValidator.signingPublic);
            BEAST_EXPECT(resolved->manifestSequence == parsedManifest.sequence);
            BEAST_EXPECT(resolved->bindingID == parsedManifest.bindingID());
            BEAST_EXPECT(resolved->manifestBlob == serializedBlob(raw));
            BEAST_EXPECT(!resolved->revoked());
            BEAST_EXPECT(!resolved->frozen());
            BEAST_EXPECT(resolved->usableSigningBinding());
        }

        auto const* missing =
            bindings->findMaster(missingValidator.masterPublic);
        BEAST_EXPECT(missing != nullptr);
        if (missing)
        {
            BEAST_EXPECT(
                missing->resolution ==
                UNLReportMemberBindingResolution::missing);
            BEAST_EXPECT(missing->masterKey == missingValidator.masterPublic);
            BEAST_EXPECT(!missing->signingKey);
            BEAST_EXPECT(!missing->revoked());
            BEAST_EXPECT(!missing->frozen());
            BEAST_EXPECT(!missing->usableSigningBinding());
        }

        BEAST_EXPECT(!bindings->findMaster(validator().masterPublic));
    }

    void
    testBindingViewRevocation()
    {
        testcase("binding view resolves terminal revocation as unusable");
        jtx::Env env(*this, jtx::supported_amendments() | featureUNLReportV2);
        auto const v = validator();
        auto parent = parentLedger(env, {v.masterPublic});
        OpenView view(&*parent);

        auto const raw = revocation(v);
        auto const parsedRevocation = parsed(raw);
        BEAST_EXPECT(apply(env, view, memberTx(raw)) == tesSUCCESS);

        auto bindings = buildUNLReportMemberBindingView(view);
        if (!BEAST_EXPECT(bindings.has_value()))
            return;

        auto const* revoked = bindings->findMaster(v.masterPublic);
        BEAST_EXPECT(revoked != nullptr);
        if (revoked)
        {
            BEAST_EXPECT(
                revoked->resolution ==
                UNLReportMemberBindingResolution::resolved);
            BEAST_EXPECT(!revoked->signingKey);
            BEAST_EXPECT(
                revoked->manifestSequence == parsedRevocation.sequence);
            BEAST_EXPECT(revoked->bindingID == parsedRevocation.bindingID());
            BEAST_EXPECT(revoked->manifestBlob == serializedBlob(raw));
            BEAST_EXPECT(revoked->revoked());
            BEAST_EXPECT(!revoked->frozen());
            BEAST_EXPECT(!revoked->usableSigningBinding());
        }
    }

    void
    testBindingViewFrozenRecords()
    {
        testcase("binding view treats frozen records as resolved unusable");
        jtx::Env env(*this, jtx::supported_amendments() | featureUNLReportV2);

        {
            auto const v = validator();
            auto const [otherPublic, otherSecret] =
                randomKeyPair(KeyType::secp256k1);
            auto parent = parentLedger(env, {v.masterPublic});
            OpenView view(&*parent);
            BEAST_EXPECT(
                apply(env, view, memberTx(manifest(v, 1))) == tesSUCCESS);
            BEAST_EXPECT(
                apply(
                    env,
                    view,
                    memberTx(manifest(v, otherPublic, otherSecret, 1))) ==
                tesSUCCESS);

            auto bindings = buildUNLReportMemberBindingView(view);
            if (BEAST_EXPECT(bindings.has_value()))
            {
                auto const* frozen = bindings->findMaster(v.masterPublic);
                BEAST_EXPECT(frozen != nullptr);
                if (frozen)
                {
                    BEAST_EXPECT(
                        frozen->resolution ==
                        UNLReportMemberBindingResolution::resolved);
                    BEAST_EXPECT(frozen->frozen());
                    BEAST_EXPECT(!frozen->usableSigningBinding());
                    BEAST_EXPECT(
                        frozen->ledgerFlags &
                        lsfUNLReportMemberEquivocationFreeze);
                }
            }
        }

        {
            auto const a = validator();
            auto const b = validator();
            auto parent = parentLedger(env, {a.masterPublic, b.masterPublic});
            OpenView view(&*parent);
            BEAST_EXPECT(
                apply(env, view, memberTx(manifest(a, 1))) == tesSUCCESS);
            BEAST_EXPECT(
                apply(
                    env,
                    view,
                    memberTx(
                        manifest(b, a.signingPublic, a.signingSecret, 1))) ==
                tesSUCCESS);

            auto bindings = buildUNLReportMemberBindingView(view);
            if (BEAST_EXPECT(bindings.has_value()))
            {
                for (auto const& master : {a.masterPublic, b.masterPublic})
                {
                    auto const* frozen = bindings->findMaster(master);
                    BEAST_EXPECT(frozen != nullptr);
                    if (frozen)
                    {
                        BEAST_EXPECT(
                            frozen->resolution ==
                            UNLReportMemberBindingResolution::resolved);
                        BEAST_EXPECT(frozen->frozen());
                        BEAST_EXPECT(!frozen->usableSigningBinding());
                        BEAST_EXPECT(
                            frozen->ledgerFlags &
                            lsfUNLReportMemberSigningKeyCollisionFreeze);
                    }
                }
            }
        }
    }

    void
    testBindingViewMalformedRecords()
    {
        testcase("binding view keeps malformed records visible unusable");
        jtx::Env env(*this, jtx::supported_amendments() | featureUNLReportV2);
        std::vector<Validator> validators;
        validators.reserve(5);
        for (std::size_t i = 0; i < 5; ++i)
            validators.emplace_back(validator());

        std::vector<PublicKey> masters;
        masters.reserve(validators.size());
        for (auto const& v : validators)
            masters.emplace_back(v.masterPublic);

        auto parent = parentLedger(env, masters);
        OpenView view(&*parent);
        for (auto const& v : validators)
            BEAST_EXPECT(
                apply(env, view, memberTx(manifest(v, 1))) == tesSUCCESS);

        mutateMember(view, validators[0].masterPublic, [](SLE& sle) {
            sle.makeFieldAbsent(sfDigest);
        });
        mutateMember(view, validators[1].masterPublic, [&](SLE& sle) {
            sle.setFieldVL(sfPublicKey, validators[0].masterPublic);
        });
        mutateMember(view, validators[2].masterPublic, [](SLE& sle) {
            sle.setFieldU32(sfSequence, 2);
        });
        mutateMember(view, validators[3].masterPublic, [&](SLE& sle) {
            sle.setFieldVL(sfSigningPubKey, validators[0].signingPublic);
        });
        mutateMember(view, validators[4].masterPublic, [](SLE& sle) {
            std::string const invalidBlob(1, '\0');
            sle.setFieldVL(sfBlob, makeSlice(invalidBlob));
        });

        auto bindings = buildUNLReportMemberBindingView(view);
        if (!BEAST_EXPECT(bindings.has_value()))
            return;

        for (auto const& v : validators)
        {
            auto const* malformed = bindings->findMaster(v.masterPublic);
            BEAST_EXPECT(malformed != nullptr);
            if (malformed)
            {
                BEAST_EXPECT(
                    malformed->resolution ==
                    UNLReportMemberBindingResolution::malformed);
                BEAST_EXPECT(!malformed->usableSigningBinding());
                BEAST_EXPECT(!malformed->revoked());
                BEAST_EXPECT(!malformed->frozen());
            }
        }
    }

    void
    testBindingViewDuplicateSigningCompensatingControl()
    {
        testcase(
            "binding view detects duplicate signing keys with flags clear");
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

        mutateMember(view, a.masterPublic, [](SLE& sle) {
            sle.setFieldU32(sfFlags, 0);
        });
        mutateMember(view, b.masterPublic, [](SLE& sle) {
            sle.setFieldU32(sfFlags, 0);
        });

        auto bindings = buildUNLReportMemberBindingView(view);
        if (!BEAST_EXPECT(bindings.has_value()))
            return;

        for (auto const& master : {a.masterPublic, b.masterPublic})
        {
            auto const* binding = bindings->findMaster(master);
            BEAST_EXPECT(binding != nullptr);
            if (binding)
            {
                BEAST_EXPECT(
                    binding->resolution ==
                    UNLReportMemberBindingResolution::resolved);
                BEAST_EXPECT(!binding->frozen());
                BEAST_EXPECT(binding->duplicateSigningKey);
                BEAST_EXPECT(!binding->usableSigningBinding());
                BEAST_EXPECT(binding->signingKey.has_value());
                BEAST_EXPECT(
                    binding->signingKey &&
                    *binding->signingKey == a.signingPublic);
            }
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
        auto const [thirdPublic, thirdSecret] =
            randomKeyPair(KeyType::secp256k1);
        auto const a = memberTx(manifest(v, 1));
        auto const b = memberTx(manifest(v, otherPublic, otherSecret, 1));
        auto const c = memberTx(manifest(v, thirdPublic, thirdSecret, 1));

        auto applyPair = [&](STTx const& first, STTx const& second) {
            auto parent = parentLedger(env, {v.masterPublic});
            OpenView view(&*parent);
            BEAST_EXPECT(apply(env, view, first) == tesSUCCESS);
            BEAST_EXPECT(apply(env, view, second) == tesSUCCESS);
            auto const sle = view.read(keylet::UNLReportMember(v.masterPublic));
            BEAST_EXPECT(
                sle &&
                (sle->getFlags() & lsfUNLReportMemberEquivocationFreeze));
            BEAST_EXPECT(apply(env, view, second) == tefALREADY);
            return std::make_pair(
                sle->getFieldH256(sfDigest), sle->getFieldVL(sfBlob));
        };

        BEAST_EXPECT(applyPair(a, b) == applyPair(b, a));

        std::array<STTx const*, 3> statements{&a, &b, &c};
        std::array<std::size_t, 3> order{0, 1, 2};
        std::optional<std::pair<uint256, Blob>> expected;
        do
        {
            auto permutationParent = parentLedger(env, {v.masterPublic});
            OpenView permutationView(&*permutationParent);
            for (auto const index : order)
            {
                auto const ter =
                    apply(env, permutationView, *statements[index]);
                BEAST_EXPECT(ter == tesSUCCESS || ter == tefALREADY);
            }
            auto const record =
                permutationView.read(keylet::UNLReportMember(v.masterPublic));
            BEAST_EXPECT(
                record &&
                (record->getFlags() & lsfUNLReportMemberEquivocationFreeze));
            auto const result = std::make_pair(
                record->getFieldH256(sfDigest), record->getFieldVL(sfBlob));
            if (!expected)
                expected = result;
            else
                BEAST_EXPECT(result == *expected);
        } while (std::next_permutation(order.begin(), order.end()));

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
    testEquivocationConvergesAcrossParentRounds()
    {
        testcase(
            "equal-sequence equivocation converges across parent-ledger "
            "rounds");
        jtx::Env env(*this, jtx::supported_amendments() | featureUNLReportV2);
        auto const [masterPublic, masterSecret] = generateKeyPair(
            KeyType::ed25519,
            generateSeed("unl-report-member-converge-master"));
        auto const [signingPublic, signingSecret] = generateKeyPair(
            KeyType::secp256k1,
            generateSeed("unl-report-member-converge-signing-0"));
        Validator const v{
            masterPublic, masterSecret, signingPublic, signingSecret};
        auto const [otherPublic, otherSecret] = generateKeyPair(
            KeyType::secp256k1,
            generateSeed("unl-report-member-converge-signing-1"));
        auto const [thirdPublic, thirdSecret] = generateKeyPair(
            KeyType::secp256k1,
            generateSeed("unl-report-member-converge-signing-2"));

        struct Statement
        {
            std::string serialized;
            uint256 binding;
        };

        std::array<Statement, 3> statements{{
            {manifest(v, 1), {}},
            {manifest(v, otherPublic, otherSecret, 1), {}},
            {manifest(v, thirdPublic, thirdSecret, 1), {}},
        }};
        for (auto& statement : statements)
            statement.binding = parsed(statement.serialized).bindingID();
        std::sort(
            statements.begin(),
            statements.end(),
            [](Statement const& lhs, Statement const& rhs) {
                return lhs.binding < rhs.binding;
            });
        BEAST_EXPECT(statements[0].binding < statements[1].binding);
        BEAST_EXPECT(statements[1].binding < statements[2].binding);

        auto evidence = [](Statement const& first, Statement const& second) {
            std::vector<Manifest> result;
            result.emplace_back(parsed(first.serialized));
            result.emplace_back(parsed(second.serialized));
            return result;
        };
        auto ledger = parentLedger(env, {v.masterPublic});
        auto applyRound = [&](std::vector<Manifest> observed) {
            auto const updates =
                buildUNLReportMemberUpdates(*ledger, std::move(observed));
            auto next = std::make_shared<Ledger>(
                *ledger, env.app().timeKeeper().closeTime());
            OpenView view(&*next);
            for (auto const& update : updates)
                BEAST_EXPECT(apply(env, view, update) == tesSUCCESS);
            view.apply(*next);
            ledger = std::move(next);
            return updates.size();
        };
        auto expectStored = [&](Statement const& statement) {
            ReadView const& view = *ledger;
            auto const sle = view.read(keylet::UNLReportMember(v.masterPublic));
            BEAST_EXPECT(sle);
            BEAST_EXPECT(sle && sle->getFieldU32(sfSequence) == 1);
            BEAST_EXPECT(
                sle &&
                (sle->getFlags() & lsfUNLReportMemberEquivocationFreeze));
            BEAST_EXPECT(
                sle && sle->getFieldH256(sfDigest) == statement.binding);
        };

        BEAST_EXPECT(applyRound(evidence(statements[1], statements[2])) == 2);
        expectStored(statements[1]);

        BEAST_EXPECT(applyRound(evidence(statements[0], statements[2])) == 1);
        expectStored(statements[0]);

        BEAST_EXPECT(buildUNLReportMemberUpdates(
                         *ledger, evidence(statements[1], statements[2]))
                         .empty());
        BEAST_EXPECT(buildUNLReportMemberUpdates(
                         *ledger, evidence(statements[0], statements[2]))
                         .empty());
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

    void
    testDeltaSelection()
    {
        testcase("bounded deterministic delta selection");
        jtx::Env env(*this, jtx::supported_amendments() | featureUNLReportV2);
        auto const a = validator();
        auto const b = validator();

        {
            auto coalescedLedger = parentLedger(env, {a.masterPublic});
            std::vector<Manifest> rotations;
            rotations.emplace_back(parsed(manifest(a, 1)));
            rotations.emplace_back(parsed(manifest(a, 2)));
            auto updates = buildUNLReportMemberUpdates(
                *coalescedLedger, std::move(rotations), 1);
            BEAST_EXPECT(updates.size() == 1);
            auto selected =
                deserializeManifest(updates.front().getFieldVL(sfBlob));
            BEAST_EXPECT(selected && selected->sequence == 2);
        }

        {
            auto revocationLedger = parentLedger(env, {a.masterPublic});
            std::vector<Manifest> revocations;
            revocations.emplace_back(parsed(revocation(a)));
            auto updates = buildUNLReportMemberUpdates(
                *revocationLedger, std::move(revocations));
            BEAST_EXPECT(updates.size() == 1);

            OpenView view(&*revocationLedger);
            BEAST_EXPECT(apply(env, view, updates.front()) == tesSUCCESS);
            auto const sle = view.read(keylet::UNLReportMember(a.masterPublic));
            BEAST_EXPECT(sle && !sle->isFieldPresent(sfSigningPubKey));
            BEAST_EXPECT(
                sle && Manifest::revoked(sle->getFieldU32(sfSequence)));
        }

        auto ledger = parentLedger(env, {a.masterPublic, b.masterPublic});
        auto evidence = [&] {
            std::vector<Manifest> result;
            result.emplace_back(parsed(manifest(b, 1)));
            result.emplace_back(parsed(manifest(a, 1)));
            return result;
        };

        auto selected = buildUNLReportMemberUpdates(*ledger, evidence(), 1);
        BEAST_EXPECT(selected.size() == 1);
        auto first = deserializeManifest(selected.front().getFieldVL(sfBlob));
        BEAST_EXPECT(first);
        BEAST_EXPECT(
            first &&
            first->masterKey == std::min(a.masterPublic, b.masterPublic));

        {
            OpenView view(&*ledger);
            BEAST_EXPECT(apply(env, view, selected.front()) == tesSUCCESS);
            view.apply(*ledger);
        }

        selected = buildUNLReportMemberUpdates(*ledger, evidence());
        BEAST_EXPECT(selected.size() == 1);

        auto const& admitted = first->masterKey == a.masterPublic ? a : b;
        auto const [otherPublic, otherSecret] =
            randomKeyPair(KeyType::secp256k1);
        auto conflict = [&] {
            std::vector<Manifest> result;
            result.emplace_back(
                parsed(manifest(admitted, otherPublic, otherSecret, 1)));
            return result;
        };
        selected = buildUNLReportMemberUpdates(*ledger, conflict());
        BEAST_EXPECT(selected.size() == 1);
        {
            OpenView view(&*ledger);
            BEAST_EXPECT(apply(env, view, selected.front()) == tesSUCCESS);
            view.apply(*ledger);
        }
        BEAST_EXPECT(buildUNLReportMemberUpdates(*ledger, conflict()).empty());
    }

public:
    void
    run() override
    {
        testAmendmentAndAdmission();
        testBindingViewAvailability();
        testBindingViewActiveMasters();
        testBindingViewResolvedAndMissing();
        testBindingViewRevocation();
        testBindingViewFrozenRecords();
        testBindingViewMalformedRecords();
        testBindingViewDuplicateSigningCompensatingControl();
        testMalformedEvidence();
        testRotationAndRevocation();
        testInactiveMemberCanRevoke();
        testEquivocationIsOrderIndependent();
        testEquivocationConvergesAcrossParentRounds();
        testSigningKeyCollision();
        testDeltaSelection();
    }
};

BEAST_DEFINE_TESTSUITE(UNLReportMember, consensus, ripple);

}  // namespace test
}  // namespace ripple
