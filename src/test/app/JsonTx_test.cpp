//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.
*/
//==============================================================================

#include <test/jtx.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/JsonTx.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STParsedJSON.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>

namespace ripple {
namespace test {

struct JsonTx_test : public beast::unit_test::suite
{
    // Build a canonical Payment tx_json_str for `from` -> `to` with the given
    // drops amount and sequence. Field order follows XRPL ordinal order so
    // the json-tx codec can compress it maximally; tests here don't care
    // about compression but the node accepts any valid JSON shape.
    static std::string
    buildPaymentJson(
        jtx::Account const& from,
        jtx::Account const& to,
        std::uint64_t amountDrops,
        std::uint32_t sequence,
        std::uint64_t fee)
    {
        std::string const pkHex = strHex(from.pk().slice());
        std::ostringstream os;
        os << "{"
           << R"("TransactionType":"Payment",)"
           << R"("Flags":2147483648,)"
           << R"("Sequence":)" << sequence << ","
           << R"("Amount":")" << amountDrops << R"(",)"
           << R"("Fee":")" << fee << R"(",)"
           << R"("SigningPubKey":")" << pkHex << R"(",)"
           << R"("Account":")" << from.human() << R"(",)"
           << R"("Destination":")" << to.human() << R"(")"
           << "}";
        return os.str();
    }

    // Sign the UTF-8 bytes of tx_json_str with `from`'s secret key.
    static Buffer
    signBody(jtx::Account const& from, std::string const& tx_json_str)
    {
        return sign(
            from.pk(),
            from.sk(),
            Slice{tx_json_str.data(), tx_json_str.size()});
    }

    static Json::Value
    rpcSubmit(
        jtx::Env& env,
        std::string const& tx_json_str,
        Buffer const& signature)
    {
        Json::Value params(Json::objectValue);
        params["tx_json_str"] = tx_json_str;
        params[jss::signature] = strHex(signature);
        return env.rpc("json", "submit_json_tx", to_string(params));
    }

    // ---------- RPC-level tests ----------

    void
    testEnabledGate(FeatureBitset features)
    {
        testcase("enabled (feature gate)");
        using namespace jtx;

        for (bool const withFeature : {true, false})
        {
            auto const amend =
                withFeature ? features : features - featureJsonTx;
            Env env{*this, amend};

            Account const alice{"alice"};
            Account const bob{"bob"};
            env.fund(XRP(1000), alice, bob);
            env.close();

            auto const fee = env.current()->fees().base.drops();
            auto const txJson =
                buildPaymentJson(alice, bob, 1'000'000, env.seq(alice), fee);
            auto const sig = signBody(alice, txJson);
            auto const result = rpcSubmit(env, txJson, sig);
            env.close();

            auto const& inner = result[jss::result];
            if (withFeature)
            {
                BEAST_EXPECT(inner[jss::engine_result] == "tesSUCCESS");
                BEAST_EXPECT(inner[jss::applied].asBool());
            }
            else
            {
                // Amendment is off -> the RPC itself rejects the
                // submission before any classical verification.
                BEAST_EXPECT(inner[jss::error].asString() == "notEnabled");
            }
        }
    }

    void
    testBasicRoundtrip(FeatureBitset features)
    {
        testcase("basic payment roundtrip");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(1000), alice, bob);
        env.close();

        auto const preAlice = env.balance(alice);
        auto const preBob = env.balance(bob);
        auto const feeDrops = env.current()->fees().base;

        auto const txJson = buildPaymentJson(
            alice, bob, 1'000'000, env.seq(alice), feeDrops.drops());
        auto const sig = signBody(alice, txJson);

        auto const result = rpcSubmit(env, txJson, sig);
        env.close();

        auto const& inner = result[jss::result];
        BEAST_EXPECT(inner[jss::engine_result] == "tesSUCCESS");
        BEAST_EXPECT(inner[jss::applied].asBool());
        BEAST_EXPECT(inner["tx_json_str"].asString() == txJson);

        BEAST_EXPECT(env.balance(alice) == preAlice - XRP(1) - feeDrops);
        BEAST_EXPECT(env.balance(bob) == preBob + XRP(1));
    }

    void
    testMissingParams(FeatureBitset features)
    {
        testcase("missing params");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        env.close();

        auto const txJson = buildPaymentJson(
            alice,
            Account{"bob"},
            1'000'000,
            env.seq(alice),
            env.current()->fees().base.drops());
        auto const sig = signBody(alice, txJson);

        // No tx_json_str.
        {
            Json::Value p(Json::objectValue);
            p[jss::signature] = strHex(sig);
            auto const r = env.rpc("json", "submit_json_tx", to_string(p));
            BEAST_EXPECT(r[jss::result][jss::error] == "invalidParams");
        }

        // No signature.
        {
            Json::Value p(Json::objectValue);
            p["tx_json_str"] = txJson;
            auto const r = env.rpc("json", "submit_json_tx", to_string(p));
            BEAST_EXPECT(r[jss::result][jss::error] == "invalidParams");
        }

        // Signature is not valid hex.
        {
            Json::Value p(Json::objectValue);
            p["tx_json_str"] = txJson;
            p[jss::signature] = "notahex";
            auto const r = env.rpc("json", "submit_json_tx", to_string(p));
            BEAST_EXPECT(r[jss::result][jss::error] == "invalidParams");
        }
    }

    void
    testInvalidJson(FeatureBitset features)
    {
        testcase("invalid tx_json_str");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        env.close();

        // Syntactically invalid JSON.
        {
            std::string const bad = "{not valid json";
            auto const sig = signBody(alice, bad);
            auto const r = rpcSubmit(env, bad, sig);
            BEAST_EXPECT(
                r[jss::result][jss::error].asString() == "invalidTransaction");
        }

        // Valid JSON but not an object.
        {
            std::string const arr = R"([1,2,3])";
            auto const sig = signBody(alice, arr);
            auto const r = rpcSubmit(env, arr, sig);
            BEAST_EXPECT(
                r[jss::result][jss::error].asString() == "invalidTransaction");
        }
    }

    void
    testBadSignature(FeatureBitset features)
    {
        testcase("bad signature");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(1000), alice, bob);
        env.close();

        auto const txJson = buildPaymentJson(
            alice,
            bob,
            1'000'000,
            env.seq(alice),
            env.current()->fees().base.drops());
        auto sig = signBody(alice, txJson);

        // Flip a bit in the signature.
        std::vector<std::uint8_t> corrupted(
            sig.data(), sig.data() + sig.size());
        corrupted.at(0) ^= 0x01;
        Buffer bad(corrupted.data(), corrupted.size());

        auto const result = rpcSubmit(env, txJson, bad);
        BEAST_EXPECT(
            result[jss::result][jss::error].asString() == "invalidTransaction");
    }

    void
    testSignatureOverDifferentBytesFails(FeatureBitset features)
    {
        testcase("signature over different bytes");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(1000), alice, bob);
        env.close();

        auto const txJson = buildPaymentJson(
            alice,
            bob,
            1'000'000,
            env.seq(alice),
            env.current()->fees().base.drops());
        // Sign a different string -- same tx, different bytes.
        auto const otherJson = txJson + " ";
        auto const sig = signBody(alice, otherJson);

        auto const result = rpcSubmit(env, txJson, sig);
        BEAST_EXPECT(
            result[jss::result][jss::error].asString() == "invalidTransaction");
    }

    void
    testWrongSigningPubKey(FeatureBitset features)
    {
        testcase("wrong SigningPubKey");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const mallory{"mallory"};
        env.fund(XRP(1000), alice, bob, mallory);
        env.close();

        // Alice's account, but signed with mallory's key: claim alice's
        // SigningPubKey -> sig fails to verify. Then also try with
        // mallory's SigningPubKey: sig verifies but Account mismatch
        // causes downstream failure (tecNO_AUTH or similar).
        auto txJson = buildPaymentJson(
            alice,
            bob,
            1'000'000,
            env.seq(alice),
            env.current()->fees().base.drops());
        auto const badSig = signBody(mallory, txJson);

        auto const r = rpcSubmit(env, txJson, badSig);
        BEAST_EXPECT(
            r[jss::result][jss::error].asString() == "invalidTransaction");
    }

    // ---------- helper-level unit tests (no RPC) ----------

    void
    testHelperDirectly(FeatureBitset features)
    {
        testcase("helper functions direct");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(1000), alice, bob);
        env.close();

        std::string const txJson = buildPaymentJson(
            alice,
            bob,
            1'000'000,
            env.seq(alice),
            env.current()->fees().base.drops());
        auto const sig = signBody(alice, txJson);

        // Assemble an STTx mirroring what the node would produce from
        // submit_json_tx.
        Json::Value parsed;
        Json::Reader reader;
        BEAST_EXPECT(reader.parse(txJson, parsed) && parsed.isObject());

        parsed[jss::TxnSignature] = strHex(sig);
        parsed[sfJsonTxBody.jsonName] = strHex(txJson);

        STParsedJSONObject parsedObj("tx", parsed);
        BEAST_EXPECT(static_cast<bool>(parsedObj.object));
        STTx const stx(std::move(*parsedObj.object));

        // hasBody / body / bodyHash
        BEAST_EXPECT(jsonTx::hasBody(stx));
        auto const slice = jsonTx::body(stx);
        BEAST_EXPECT(slice.size() == txJson.size());
        BEAST_EXPECT(
            std::memcmp(slice.data(), txJson.data(), txJson.size()) == 0);
        auto const h = jsonTx::bodyHash(stx);
        BEAST_EXPECT(h == sha512Half(Slice{txJson.data(), txJson.size()}));

        // Signature check passes on a well-formed tx.
        BEAST_EXPECT(static_cast<bool>(jsonTx::checkSignature(stx)));

        // Structural equivalence passes.
        BEAST_EXPECT(
            static_cast<bool>(jsonTx::checkStructuralEquivalence(stx)));

        // Tamper with the body -- change Amount in the ASCII without
        // touching the structural fields. Signature will still be over
        // the original bytes, so we re-sign over the new body so we
        // isolate the structural-equivalence check.
        std::string tampered = txJson;
        auto const needle = std::string(R"("Amount":"1000000")");
        auto const pos = tampered.find(needle);
        BEAST_EXPECT(pos != std::string::npos);
        tampered.replace(pos, needle.size(), R"("Amount":"9000000")");

        auto const tamperedSig = signBody(alice, tampered);
        Json::Value mismatched = parsed;
        mismatched[jss::TxnSignature] = strHex(tamperedSig);
        mismatched[sfJsonTxBody.jsonName] = strHex(tampered);

        STParsedJSONObject mismatchedObj("tx", mismatched);
        BEAST_EXPECT(static_cast<bool>(mismatchedObj.object));
        STTx const mismatchedTx(std::move(*mismatchedObj.object));

        // Signature now verifies over the tampered body...
        BEAST_EXPECT(static_cast<bool>(jsonTx::checkSignature(mismatchedTx)));
        // ...but structural equivalence fails.
        BEAST_EXPECT(!jsonTx::checkStructuralEquivalence(mismatchedTx));
    }

    void
    testHelperEmptyAndMissing(FeatureBitset features)
    {
        testcase("helpers on non-jsontx STTx");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        env.close();

        // A noop signed classically carries no sfJsonTxBody.
        auto const jt = env.jt(noop(alice));
        BEAST_EXPECT(!jsonTx::hasBody(*jt.stx));
        BEAST_EXPECT(jsonTx::body(*jt.stx).empty());
        BEAST_EXPECT(jsonTx::bodyHash(*jt.stx) == uint256{});
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnabledGate(features);
        testBasicRoundtrip(features);
        testMissingParams(features);
        testInvalidJson(features);
        testBadSignature(features);
        testSignatureOverDifferentBytesFails(features);
        testWrongSigningPubKey(features);
        testHelperDirectly(features);
        testHelperEmptyAndMissing(features);
    }

public:
    void
    run() override
    {
        using namespace jtx;
        auto const sa = supported_amendments();
        testWithFeats(sa);
    }
};

BEAST_DEFINE_TESTSUITE(JsonTx, app, ripple);

}  // namespace test
}  // namespace ripple
