//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES...
*/
//==============================================================================

#include <test/jtx.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/jss.h>

namespace ripple {

// Functional tests for the PWA boot-blob transactor (SetBoot / featurePWABoot).
// v0: store / replace / delete a single sfBootBlob on the sender's own AccountRoot.
class SetBoot_test : public beast::unit_test::suite
{
    // Maximum allowed blob (mirror SetBoot::maxBootBlobBytes) — kept local to avoid pulling the
    // transactor header into the test.
    static constexpr std::size_t maxBootBlobBytes = 4096;

    static Json::Value
    setBoot(test::jtx::Account const& acct, std::optional<Blob> const& blob)
    {
        Json::Value jv;
        jv[jss::TransactionType] = "SetBoot";
        jv[jss::Account] = acct.human();
        // explicit, generous fee — the per-byte surcharge in calculateBaseFee isn't autofilled.
        jv[jss::Fee] = "1000000";
        if (blob)
            jv[sfBootBlob.jsonName] = strHex(*blob);
        return jv;
    }

    // Same tx but WITHOUT a baked-in fee, so a caller can attach an exact fee() and actually
    // exercise calculateBaseFee (base + 1 drop/blob-byte) via Transactor::checkFee.
    static Json::Value
    setBootNoFee(test::jtx::Account const& acct, std::optional<Blob> const& blob)
    {
        Json::Value jv;
        jv[jss::TransactionType] = "SetBoot";
        jv[jss::Account] = acct.human();
        if (blob)
            jv[sfBootBlob.jsonName] = strHex(*blob);
        return jv;
    }

    void
    testDisabled()
    {
        testcase("amendment disabled => temDISABLED");
        using namespace test::jtx;
        Env env{*this, supported_amendments() - featurePWABoot};
        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();
        env(setBoot(alice, Blob{'h', 'i'}), ter(temDISABLED));
        env.close();
    }

    void
    testSetReplaceDelete()
    {
        testcase("set / replace / delete");
        using namespace test::jtx;
        Env env{*this, supported_amendments()};  // PWABoot is Supported::yes => included
        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        Blob const b1{'h', 'e', 'l', 'l', 'o'};
        env(setBoot(alice, b1));
        env.close();
        {
            auto const sle = env.le(alice);
            BEAST_EXPECT(sle && sle->isFieldPresent(sfBootBlob));
            BEAST_EXPECT(sle->getFieldVL(sfBootBlob) == b1);
        }

        Blob const b2{'w', 'o', 'r', 'l', 'd', '!'};
        env(setBoot(alice, b2));
        env.close();
        BEAST_EXPECT(env.le(alice)->getFieldVL(sfBootBlob) == b2);

        env(setBoot(alice, std::nullopt));  // omit blob => delete
        env.close();
        BEAST_EXPECT(!env.le(alice)->isFieldPresent(sfBootBlob));
    }

    void
    testBounds()
    {
        testcase("empty / oversize => temMALFORMED");
        using namespace test::jtx;
        Env env{*this, supported_amendments()};
        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        env(setBoot(alice, Blob{}), ter(temMALFORMED));  // empty (present but zero-length)
        env.close();
        env(setBoot(alice, Blob(maxBootBlobBytes + 1, 0x41)), ter(temMALFORMED));  // > cap
        env.close();
        env(setBoot(alice, Blob(maxBootBlobBytes, 0x41)));  // exactly at cap => ok
        env.close();
        BEAST_EXPECT(env.le(alice)->isFieldPresent(sfBootBlob));
    }

    void
    testFeeSurcharge()
    {
        // calculateBaseFee = Transactor base + 1 drop per blob byte. Submit the EXACT required
        // fee (success) and one drop short (telINSUF_FEE_P) so the per-byte surcharge is actually
        // exercised — a zero/wrong/sign-flipped surcharge would change one of these outcomes.
        testcase("per-byte fee surcharge enforced");
        using namespace test::jtx;
        Env env{*this, supported_amendments()};
        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        Blob const blob(200, 0x42);  // 200-byte blob => +200 drop surcharge
        auto const base = env.current()->fees().base;
        auto const required = base + XRPAmount{static_cast<std::int64_t>(blob.size())};

        // one drop short of (base + surcharge) => the surcharge is what tips it under.
        env(setBootNoFee(alice, blob),
            fee(required - XRPAmount{1}),
            ter(telINSUF_FEE_P));
        env.close();
        BEAST_EXPECT(!env.le(alice)->isFieldPresent(sfBootBlob));  // rejected, nothing stored

        // exactly base + surcharge => succeeds.
        env(setBootNoFee(alice, blob), fee(required));
        env.close();
        BEAST_EXPECT(env.le(alice)->getFieldVL(sfBootBlob) == blob);
    }

    void
    testDeleteWhenAbsentIsNoOp()
    {
        // Deleting a boot blob that was never set: doApply's fall-through branch. Must be a clean
        // no-op (tesSUCCESS, field stays absent), not an error or an unexpected mutation.
        testcase("delete when absent => no-op success");
        using namespace test::jtx;
        Env env{*this, supported_amendments()};
        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        BEAST_EXPECT(!env.le(alice)->isFieldPresent(sfBootBlob));  // precondition: none set
        env(setBoot(alice, std::nullopt));  // delete with nothing present
        env.close();
        BEAST_EXPECT(!env.le(alice)->isFieldPresent(sfBootBlob));  // still absent, no error
    }

public:
    void
    run() override
    {
        testDisabled();
        testSetReplaceDelete();
        testBounds();
        testFeeSurcharge();
        testDeleteWhenAbsentIsNoOp();
    }
};

BEAST_DEFINE_TESTSUITE(SetBoot, app, ripple);

}  // namespace ripple
