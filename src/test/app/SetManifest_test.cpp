//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

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
#include <test/jtx/network.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpld/core/Config.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/protocol/st.h>

#include <limits>

namespace ripple {
namespace test {

/** Tests the OnChainManifests amendment: the SetManifest transactor, the
    submit RPC path that builds its transaction, and reading manifests back out
    of the ledger into the manifest cache.
*/
struct SetManifest_test : public beast::unit_test::suite
{
    // A manifest transaction is unsigned, so the network id is mandatory and
    // the network must be one that requires it (id > 1024).
    static std::unique_ptr<Config>
    makeConfig()
    {
        return jtx::network::makeNetworkConfig(
            21337, "10", "1000000", "200000");
    }

    /** Builds a manifest signed by `master` nominating `ephemeral`.

        A sequence of UINT32_MAX makes it a revocation, which by definition
        names no signing key and carries no ephemeral signature.
    */
    static std::string
    makeManifest(
        jtx::Account const& master,
        jtx::Account const& ephemeral,
        std::uint32_t seq)
    {
        STObject st(sfGeneric);
        st[sfSequence] = seq;
        st[sfPublicKey] = master.pk();

        if (seq != std::numeric_limits<std::uint32_t>::max())
        {
            st[sfSigningPubKey] = ephemeral.pk();
            sign(
                st,
                HashPrefix::manifest,
                *publicKeyType(ephemeral.pk()),
                ephemeral.sk());
        }

        sign(
            st,
            HashPrefix::manifest,
            *publicKeyType(master.pk()),
            master.sk(),
            sfMasterSignature);

        Serializer s;
        st.add(s);
        return std::string(static_cast<char const*>(s.data()), s.size());
    }

    /** Submits a manifest through the submit RPC.

        The transaction is unsigned -- authority comes from the manifest's own
        master and ephemeral signatures -- so it cannot be submitted through
        env() the way a signed transaction can.
    */
    static Json::Value
    submit(jtx::Env& env, std::string const& manifest)
    {
        Json::Value params;
        params[jss::manifest] = strHex(manifest);
        return env.rpc("json", "submit", to_string(params))[jss::result];
    }

    static std::string
    engineResult(Json::Value const& result)
    {
        return result[jss::engine_result].asString();
    }

    void
    testSubmission(FeatureBitset features)
    {
        testcase("submission");
        using namespace jtx;

        Env env{*this, makeConfig(), features};

        auto const master = Account("master", KeyType::ed25519);
        auto const ephemeral = Account("ephemeral", KeyType::ed25519);
        env.fund(XRP(1000), master);
        env.close();

        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(master, ephemeral, 1))) ==
            "tesSUCCESS");
        env.close();

        // A manifest is written twice so it can be found from either key, and
        // each copy points at the other.
        auto const byMaster = env.le(keylet::manifest(master.pk()));
        auto const byEphemeral = env.le(keylet::manifest(ephemeral.pk()));
        if (!BEAST_EXPECT(byMaster))
            return;
        if (!BEAST_EXPECT(byEphemeral))
            return;

        BEAST_EXPECT(byMaster->getAccountID(sfAccount) == master.id());
        BEAST_EXPECT(byEphemeral->getAccountID(sfAccount) == master.id());
        BEAST_EXPECT(byMaster->getFieldU32(sfSequence) == 1);
        BEAST_EXPECT(
            byMaster->getFieldH256(sfManifestID) ==
            keylet::manifest(ephemeral.pk()).key);
        BEAST_EXPECT(
            byEphemeral->getFieldH256(sfManifestID) ==
            keylet::manifest(master.pk()).key);

        // Both signatures are mirrored so the object can be verified, and
        // re-served, by any node that reads it.
        BEAST_EXPECT(byMaster->isFieldPresent(sfMasterSignature));
        BEAST_EXPECT(byMaster->isFieldPresent(sfSignature));

        // The account root points at the master key's copy.
        auto const sleAcct = env.le(master);
        if (!BEAST_EXPECT(sleAcct))
            return;
        BEAST_EXPECT(
            sleAcct->getFieldH256(sfManifestID) ==
            keylet::manifest(master.pk()).key);

        // The account sequence must be untouched: the transaction is unsigned
        // and pinned to sequence 0, so consuming a sequence would let a third
        // party burn the validator's sequence numbers -- and writing seq + 1
        // would reset the account to 1.
        BEAST_EXPECT(sleAcct->getFieldU32(sfSequence) == env.seq(master));
    }

    void
    testUpdate(FeatureBitset features)
    {
        testcase("update and stale rejection");
        using namespace jtx;

        Env env{*this, makeConfig(), features};

        auto const master = Account("master", KeyType::ed25519);
        auto const eph1 = Account("eph1", KeyType::ed25519);
        auto const eph2 = Account("eph2", KeyType::ed25519);
        env.fund(XRP(1000), master);
        env.close();

        submit(env, makeManifest(master, eph1, 1));
        env.close();

        // Rotating the ephemeral key erases both old copies and writes two
        // new ones, so the two can never drift apart.
        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(master, eph2, 2))) ==
            "tesSUCCESS");
        env.close();

        BEAST_EXPECT(!env.le(keylet::manifest(eph1.pk())));
        if (!BEAST_EXPECT(env.le(keylet::manifest(eph2.pk()))))
            return;
        BEAST_EXPECT(
            env.le(keylet::manifest(master.pk()))->getFieldU32(sfSequence) ==
            2);

        // Replaying the manifest we just applied, and anything older, is
        // rejected on sequence. This is what prevents replay: the transaction
        // is unsigned, so nothing else would.
        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(master, eph2, 2))) ==
            "tefPAST_MANIFEST_SEQ");
        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(master, eph1, 1))) ==
            "tefPAST_MANIFEST_SEQ");
    }

    void
    testRevocation(FeatureBitset features)
    {
        testcase("revocation");
        using namespace jtx;

        Env env{*this, makeConfig(), features};

        auto const master = Account("master", KeyType::ed25519);
        auto const ephemeral = Account("ephemeral", KeyType::ed25519);
        env.fund(XRP(1000), master);
        env.close();

        submit(env, makeManifest(master, ephemeral, 1));
        env.close();

        BEAST_EXPECT(
            engineResult(submit(
                env,
                makeManifest(
                    master,
                    ephemeral,
                    std::numeric_limits<std::uint32_t>::max()))) ==
            "tesSUCCESS");
        env.close();

        // A revocation has no signing key, so only the master key's copy
        // exists and it points at nothing.
        auto const byMaster = env.le(keylet::manifest(master.pk()));
        if (!BEAST_EXPECT(byMaster))
            return;
        BEAST_EXPECT(!env.le(keylet::manifest(ephemeral.pk())));
        BEAST_EXPECT(!byMaster->isFieldPresent(sfManifestID));
        BEAST_EXPECT(!byMaster->isFieldPresent(sfSigningPubKey));
        BEAST_EXPECT(
            byMaster->getFieldU32(sfSequence) ==
            std::numeric_limits<std::uint32_t>::max());

        // Nothing supersedes a revocation.
        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(master, ephemeral, 2))) ==
            "tefREVOKED_MANIFEST");
    }

    void
    testRetrieval(FeatureBitset features)
    {
        testcase("retrieval into the manifest cache");
        using namespace jtx;

        Env env{*this, makeConfig(), features};

        auto const master = Account("master", KeyType::ed25519);
        auto const eph1 = Account("eph1", KeyType::ed25519);
        auto const eph2 = Account("eph2", KeyType::ed25519);
        env.fund(XRP(1000), master);
        env.close();

        submit(env, makeManifest(master, eph1, 1));
        env.close();

        auto& cache = env.app().validatorManifests();

        // Nothing has fed the cache yet, so an unknown key maps to itself.
        BEAST_EXPECT(cache.getMasterKey(eph1.pk()) == eph1.pk());

        // Reading the ledger resolves both directions of the mapping. The
        // manifest is reconstructed from the ledger object and verified, so a
        // lossy round trip would fail here rather than be accepted.
        BEAST_EXPECT(cache.applyLedger(*env.closed(), {master.pk()}) == 1);
        BEAST_EXPECT(cache.getMasterKey(eph1.pk()) == master.pk());
        BEAST_EXPECT(cache.getSigningKey(master.pk()) == eph1.pk());
        BEAST_EXPECT(cache.getSequence(master.pk()) == 1);

        // Applying the same ledger again is a no-op: the cache is already at
        // that sequence.
        BEAST_EXPECT(cache.applyLedger(*env.closed(), {master.pk()}) == 0);

        // A rotation on-ledger is picked up, and the superseded ephemeral key
        // stops resolving.
        submit(env, makeManifest(master, eph2, 2));
        env.close();

        BEAST_EXPECT(cache.applyLedger(*env.closed(), {master.pk()}) == 1);
        BEAST_EXPECT(cache.getSigningKey(master.pk()) == eph2.pk());
        BEAST_EXPECT(cache.getMasterKey(eph1.pk()) == eph1.pk());

        // A key we never ask about is never read.
        BEAST_EXPECT(cache.applyLedger(*env.closed(), {eph2.pk()}) == 0);

        // A revocation reaches the cache too, and is the one thing that must
        // never be lost: forgetting it would mean trusting the old key again.
        submit(
            env,
            makeManifest(
                master, eph2, std::numeric_limits<std::uint32_t>::max()));
        env.close();

        BEAST_EXPECT(cache.applyLedger(*env.closed(), {master.pk()}) == 1);
        BEAST_EXPECT(cache.revoked(master.pk()));
    }

    void
    testMalformed(FeatureBitset features)
    {
        testcase("malformed submissions");
        using namespace jtx;

        Env env{*this, makeConfig(), features};

        auto const master = Account("master", KeyType::ed25519);
        auto const other = Account("other", KeyType::ed25519);
        auto const ephemeral = Account("ephemeral", KeyType::ed25519);
        env.fund(XRP(1000), master, other);
        env.close();

        // Not a manifest at all.
        BEAST_EXPECT(
            submit(env, "not a manifest")[jss::error].asString() ==
            "invalidManifest");

        // A manifest whose ephemeral signature does not check out.
        {
            auto blob = makeManifest(master, ephemeral, 1);
            blob[blob.size() - 1] ^= 0xFF;
            BEAST_EXPECT(
                submit(env, blob)[jss::error].asString() == "invalidManifest");
        }

        // The master key's account must exist: it pays the fee and holds the
        // pointer to the manifest.
        auto const unfunded = Account("unfunded", KeyType::ed25519);
        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(unfunded, ephemeral, 1))) ==
            "terNO_ACCOUNT");

        // An ephemeral key already claimed by a different account would
        // collide with -- and clobber -- that account's manifest object.
        submit(env, makeManifest(master, ephemeral, 1));
        env.close();

        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(other, ephemeral, 1))) ==
            "tecDUPLICATE");
    }

    void
    testDisabled(FeatureBitset features)
    {
        testcase("amendment gate");
        using namespace jtx;

        Env env{*this, makeConfig(), features - featureOnChainManifests};

        auto const master = Account("master", KeyType::ed25519);
        auto const ephemeral = Account("ephemeral", KeyType::ed25519);
        env.fund(XRP(1000), master);
        env.close();

        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(master, ephemeral, 1))) ==
            "temDISABLED");
        env.close();

        BEAST_EXPECT(!env.le(keylet::manifest(master.pk())));
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testSubmission(sa);
        testUpdate(sa);
        testRevocation(sa);
        testRetrieval(sa);
        testMalformed(sa);
        testDisabled(sa);
    }
};

BEAST_DEFINE_TESTSUITE(SetManifest, app, ripple);

}  // namespace test
}  // namespace ripple
