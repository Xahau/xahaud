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
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/app/tx/detail/SetManifest.h>
#include <xrpld/core/Config.h>
#include <xrpld/ledger/OpenView.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/XRPAmount.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/protocol/st.h>

#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace ripple {
namespace test {

/** Tests the OnChainManifests amendment: the account-signed SetManifest
    transactor, and reading manifests back out of the ledger into the manifest
    cache.
*/
struct SetManifest_test : public beast::unit_test::suite
{
    // A network that requires NetworkID (id > 1024), so every SetManifest in
    // these tests carries one.
    static std::unique_ptr<Config>
    makeConfig(std::string fee = "10")
    {
        return jtx::network::makeNetworkConfig(
            21337, std::move(fee), "1000000", "200000");
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

    /** Submits an already formed SetManifest transaction. */
    static Json::Value
    submit(jtx::Env& env, std::shared_ptr<STTx const> const& tx)
    {
        Serializer s;
        tx->add(s);
        return env.rpc("submit", strHex(s.slice()))[jss::result];
    }

    /** Signs `manifest` into a SetManifest from `account` and submits it. */
    static Json::Value
    submit(
        jtx::Env& env,
        std::string const& manifest,
        jtx::Account const& account)
    {
        return submit(env, signedEnvelope(env, manifest, account));
    }

    static std::string
    engineResult(Json::Value const& result)
    {
        return result[jss::engine_result].asString();
    }

    /** A well formed STObject that is not a well formed manifest.

        The manifest format requires sfPublicKey and sfMasterSignature, so this
        parses as an object and then fails applyTemplate.
    */
    static std::string
    makeUnparseableManifest()
    {
        STObject st(sfGeneric);
        st[sfSequence] = 1;

        Serializer s;
        st.add(s);
        return std::string(static_cast<char const*>(s.data()), s.size());
    }

    /** An account-signed SetManifest transaction.

        SetManifest is an ordinary account transaction. This uses the account's
        current Sequence unless a test overrides it; `tweak` runs before the
        outer signature, so the result is always correctly signed.
    */
    static std::shared_ptr<STTx const>
    signedEnvelope(
        jtx::Env& env,
        std::string const& manifest,
        jtx::Account const& account,
        std::optional<std::uint32_t> sequence = std::nullopt,
        std::optional<XRPAmount> fee = std::nullopt,
        std::function<void(STObject&)> const& tweak = {})
    {
        auto const build = [&](XRPAmount fee) {
            auto tx =
                std::make_shared<STTx>(ttMANIFEST_SET, [&](STObject& obj) {
                    obj.setAccountID(sfAccount, account.id());
                    obj.setFieldU32(
                        sfSequence, sequence ? *sequence : env.seq(account));
                    obj.setFieldU32(sfNetworkID, env.app().config().NETWORK_ID);
                    obj.setFieldAmount(sfFee, fee);
                    obj.setFieldVL(sfSigningPubKey, account.pk().slice());

                    SerialIter mit{makeSlice(manifest)};
                    obj.peekFieldObject(sfManifest).set(mit);

                    if (tweak)
                        tweak(obj);
                });
            tx->sign(account.pk(), account.sk());
            return tx;
        };

        auto const probe = build(XRPAmount{0});
        auto const baseFee =
            SetManifest::calculateBaseFee(*env.current(), *probe);
        return build(fee.value_or(baseFee));
    }

    /** A mutable copy of a ledger object, suitable for the RawView interface.

        Round-tripped through the wire format rather than copy constructed.
        applyTemplate() rejects an object carrying a materialised soeDEFAULT
        field that holds its default value, and that is exactly what reading one
        out of a view hands back: an AccountRoot arrives with sfMintedNFTokens
        present and zero. Serializing omits those fields and deserializing
        re-materialises them.
    */
    static std::shared_ptr<SLE>
    rawCopy(std::shared_ptr<SLE const> const& sle)
    {
        Serializer s;
        sle->add(s);
        SerialIter sit{s.slice()};
        return std::make_shared<SLE>(sit, sle->key());
    }

    /** Removes a ledger object from a view, if present.

        OpenView exposes only the RawView interface, so the entry has to be
        copied off the read side before it can be handed back to rawErase.
    */
    static void
    rawErase(OpenView& view, Keylet const& kl)
    {
        if (auto const sle = view.read(kl))
            view.rawErase(rawCopy(sle));
    }

    /** Applies `tx` to a throwaway copy of the open ledger and reports the TER.

        `prepare` runs against the same view first, so a test can corrupt state
        that no ordinary transaction could produce. Nothing is retained.
    */
    TER
    applyDirect(
        jtx::Env& env,
        std::shared_ptr<STTx const> const& tx,
        std::function<void(OpenView&)> const& prepare = {},
        ApplyFlags flags = tapNONE)
    {
        TER ret = tesSUCCESS;
        env.app().openLedger().modify([&](OpenView& view, beast::Journal j) {
            if (prepare)
                prepare(view);
            ret = ripple::apply(env.app(), view, *tx, flags, j).ter;
            return false;  // discard, corrupt or otherwise
        });
        return ret;
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

        auto const sequenceBefore = env.seq(master);

        // Registration is an ordinary account-signed transaction.
        BEAST_EXPECT(
            engineResult(
                submit(env, makeManifest(master, ephemeral, 1), master)) ==
            "tesSUCCESS");
        env.close();

        // The complete manifest is available in one lookup from either key.
        // Only the stable master-key copy is the owned directory object.
        auto const byMaster = env.le(keylet::manifest(master.pk()));
        auto const byEphemeral = env.le(keylet::manifest(ephemeral.pk()));
        if (!BEAST_EXPECT(byMaster))
            return;
        if (!BEAST_EXPECT(byEphemeral))
            return;

        BEAST_EXPECT(byMaster->getAccountID(sfAccount) == master.id());
        BEAST_EXPECT(byEphemeral->getAccountID(sfAccount) == master.id());
        BEAST_EXPECT(byMaster->getType() == ltMANIFEST);
        BEAST_EXPECT(byEphemeral->getType() == ltMANIFEST);
        BEAST_EXPECT(byMaster->isFieldPresent(sfOwnerNode));
        BEAST_EXPECT(!byEphemeral->isFieldPresent(sfOwnerNode));
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
        BEAST_EXPECT(byEphemeral->isFieldPresent(sfMasterSignature));
        BEAST_EXPECT(byEphemeral->isFieldPresent(sfSignature));

        // The account root points at the master key's copy.
        auto const sleAcct = env.le(master);
        if (!BEAST_EXPECT(sleAcct))
            return;
        BEAST_EXPECT(
            sleAcct->getFieldH256(sfManifestID) ==
            keylet::manifest(master.pk()).key);
        BEAST_EXPECT(sleAcct->getFieldU32(sfOwnerCount) == 1);

        // Account-signed registration consumed exactly one ordinary Sequence.
        BEAST_EXPECT(env.seq(master) == sequenceBefore + 1);
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
        auto const eph3 = Account("eph3", KeyType::ed25519);
        env.fund(XRP(1000), master);
        env.close();

        submit(env, signedEnvelope(env, makeManifest(master, eph1, 1), master));
        env.close();
        auto const accountSequence = env.seq(master);
        auto const ownerNode =
            env.le(keylet::manifest(master.pk()))->getFieldU64(sfOwnerNode);
        BEAST_EXPECT(env.ownerCount(master) == 1);

        // Rotation rewrites both complete copies while preserving one owner.
        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(master, eph2, 2), master)) ==
            "tesSUCCESS");
        env.close();

        BEAST_EXPECT(!env.le(keylet::manifest(eph1.pk())));
        if (!BEAST_EXPECT(env.le(keylet::manifest(eph2.pk()))))
            return;
        BEAST_EXPECT(
            env.le(keylet::manifest(master.pk()))->getFieldU32(sfSequence) ==
            2);
        BEAST_EXPECT(
            env.le(keylet::manifest(master.pk()))->getFieldU64(sfOwnerNode) ==
            ownerNode);
        BEAST_EXPECT(env.ownerCount(master) == 1);
        BEAST_EXPECT(env.seq(master) == accountSequence + 1);

        // Ordinary replay protection: a stale outer Sequence is rejected
        // before the manifest sequence matters, and the correct one both
        // rotates the manifest and advances the account.
        BEAST_EXPECT(
            engineResult(submit(
                env,
                signedEnvelope(
                    env,
                    makeManifest(master, eph3, 3),
                    master,
                    accountSequence))) == "tefPAST_SEQ");
        BEAST_EXPECT(!env.le(keylet::manifest(eph3.pk())));

        BEAST_EXPECT(
            engineResult(submit(
                env,
                signedEnvelope(env, makeManifest(master, eph3, 3), master))) ==
            "tesSUCCESS");
        env.close();
        BEAST_EXPECT(env.seq(master) == accountSequence + 2);
        BEAST_EXPECT(!env.le(keylet::manifest(eph2.pk())));
        BEAST_EXPECT(env.le(keylet::manifest(eph3.pk())));
        BEAST_EXPECT(
            env.le(keylet::manifest(master.pk()))->getFieldU64(sfOwnerNode) ==
            ownerNode);
        BEAST_EXPECT(env.ownerCount(master) == 1);

        // An older manifest in a fresh, correctly sequenced transaction is
        // rejected by manifest sequence.
        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(master, eph2, 2), master)) ==
            "tefPAST_MANIFEST_SEQ");
        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(master, eph1, 1), master)) ==
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

        submit(
            env,
            signedEnvelope(env, makeManifest(master, ephemeral, 1), master));
        env.close();

        BEAST_EXPECT(
            engineResult(submit(
                env,
                makeManifest(
                    master,
                    ephemeral,
                    std::numeric_limits<std::uint32_t>::max()),
                master)) == "tesSUCCESS");
        env.close();

        // A revocation remains owned, but has no active signing-key copy.
        auto const byMaster = env.le(keylet::manifest(master.pk()));
        if (!BEAST_EXPECT(byMaster))
            return;
        BEAST_EXPECT(!env.le(keylet::manifest(ephemeral.pk())));
        BEAST_EXPECT(!byMaster->isFieldPresent(sfManifestID));
        BEAST_EXPECT(!byMaster->isFieldPresent(sfSigningPubKey));
        BEAST_EXPECT(
            byMaster->getFieldU32(sfSequence) ==
            std::numeric_limits<std::uint32_t>::max());
        BEAST_EXPECT(env.ownerCount(master) == 1);

        // Nothing supersedes a revocation.
        BEAST_EXPECT(
            engineResult(
                submit(env, makeManifest(master, ephemeral, 2), master)) ==
            "tefREVOKED_MANIFEST");
    }

    void
    testOwnership(FeatureBitset features)
    {
        testcase("owner reserve and AccountDelete obligation");
        using namespace jtx;

        auto const master = Account("owner-master", KeyType::ed25519);
        auto const ephemeral = Account("owner-ephemeral", KeyType::ed25519);

        // The account-authorized registration creates one owned object, so
        // merely funding the account's base reserve is not enough.
        {
            Env env{*this, makeConfig(), features};
            auto const baseReserve = env.current()->fees().accountReserve(0);
            env.fund(baseReserve, master);
            env.close();

            BEAST_EXPECT(
                engineResult(submit(
                    env,
                    signedEnvelope(
                        env, makeManifest(master, ephemeral, 1), master))) ==
                "tecINSUFFICIENT_RESERVE");
            env.close();

            BEAST_EXPECT(env.ownerCount(master) == 0);
            BEAST_EXPECT(!env.le(keylet::manifest(master.pk())));
            BEAST_EXPECT(!env.le(keylet::manifest(ephemeral.pk())));
        }

        // Once registered, the canonical manifest is deliberately an
        // obligation. AccountDelete cannot erase it -- especially a terminal
        // revocation -- until a future amendment defines safe expiry.
        {
            Env env{*this, makeConfig(), features};
            auto const destination = Account("owner-destination");
            env.fund(XRP(1000), master, destination);
            env.close();

            BEAST_EXPECT(
                engineResult(submit(
                    env,
                    signedEnvelope(
                        env, makeManifest(master, ephemeral, 1), master))) ==
                "tesSUCCESS");
            env.close();

            auto const manifest = env.le(keylet::manifest(master.pk()));
            if (!BEAST_EXPECT(manifest))
                return;
            BEAST_EXPECT(manifest->isFieldPresent(sfOwnerNode));
            BEAST_EXPECT(env.ownerCount(master) == 1);

            BEAST_EXPECT(
                engineResult(submit(
                    env,
                    makeManifest(
                        master,
                        ephemeral,
                        std::numeric_limits<std::uint32_t>::max()),
                    master)) == "tesSUCCESS");
            env.close();
            BEAST_EXPECT(env.ownerCount(master) == 1);
            BEAST_EXPECT(
                env.le(keylet::manifest(master.pk()))
                    ->getFieldU32(sfSequence) ==
                std::numeric_limits<std::uint32_t>::max());

            while (env.seq(master) + 255 > env.current()->seq())
                env.close();

            auto const accountDeleteFee{drops(env.current()->fees().increment)};
            env(acctdelete(master, destination),
                fee(accountDeleteFee),
                ter(tecHAS_OBLIGATIONS));
            env.close();

            BEAST_EXPECT(env.le(master));
            BEAST_EXPECT(env.le(keylet::manifest(master.pk())));
            BEAST_EXPECT(env.ownerCount(master) == 1);
        }
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

        submit(env, signedEnvelope(env, makeManifest(master, eph1, 1), master));
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
        submit(env, makeManifest(master, eph2, 2), master);
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
                master, eph2, std::numeric_limits<std::uint32_t>::max()),
            master);
        env.close();

        BEAST_EXPECT(cache.applyLedger(*env.closed(), {master.pk()}) == 1);
        BEAST_EXPECT(cache.revoked(master.pk()));
    }

    void
    testListedAfterLedgerRevocation(
        FeatureBitset features,
        bool scheduled = false)
    {
        testcase(
            scheduled
                ? "scheduled listing checks revocation before publishing trust"
                : "newly listed warm identity honours the ledger revocation");
        using namespace jtx;
        Env env{*this, makeConfig(), features};
        auto const master = Account("late-listed-master", KeyType::ed25519);
        auto const signing = Account("late-listed-signing", KeyType::secp256k1);
        auto const control = Account("late-list-control", KeyType::ed25519);
        auto const controlSigning =
            Account("late-list-control-signing", KeyType::secp256k1);
        auto& cache = env.app().validatorManifests();
        auto& lists = env.app().validators();
        BEAST_EXPECT(
            cache.applyManifest(*deserializeManifest(makeManifest(
                control, controlSigning, 1))) == ManifestDisposition::accepted);
        BEAST_EXPECT(lists.load(
            {}, {toBase58(TokenType::NodePublic, control.pk())}, {}));
        env.fund(XRP(1000), master);
        env.close();
        BEAST_EXPECT(
            engineResult(submit(
                env,
                signedEnvelope(
                    env, makeManifest(master, signing, 1), master))) ==
            "tesSUCCESS");
        env.close();

        BEAST_EXPECT(!lists.listed(master.pk()));
        BEAST_EXPECT(
            cache.applyLedgerSigningKey(*env.closed(), signing.pk()) ==
            master.pk());
        BEAST_EXPECT(cache.getSequence(master.pk()) == 1);

        BEAST_EXPECT(
            engineResult(submit(
                env,
                makeManifest(
                    master, signing, std::numeric_limits<std::uint32_t>::max()),
                master)) == "tesSUCCESS");
        env.close();
        auto const onLedger = env.le(keylet::manifest(master.pk()));
        if (!BEAST_EXPECT(onLedger))
            return;
        BEAST_EXPECT(
            onLedger->getFieldU32(sfSequence) ==
            std::numeric_limits<std::uint32_t>::max());
        BEAST_EXPECT(!cache.revoked(master.pk()));

        auto const effective =
            env.timeKeeper().now() + std::chrono::seconds{120};
        if (scheduled)
        {
            auto const publisher =
                Account("late-list-publisher", KeyType::ed25519);
            auto const pubSigning =
                Account("late-list-pub-signing", KeyType::secp256k1);
            BEAST_EXPECT(lists.load({}, {}, {strHex(publisher.pk())}));
            auto list = [&](bool future) {
                Json::Value body(Json::objectValue);
                body["sequence"] = future ? 2 : 1;
                body["expiration"] = static_cast<Json::UInt>(
                    (effective + std::chrono::seconds{3600})
                        .time_since_epoch()
                        .count());
                if (future)
                    body["effective"] = static_cast<Json::UInt>(
                        effective.time_since_epoch().count());
                body["validators"] = Json::Value(Json::arrayValue);
                auto add = [&](Account const& m, Account const& s) {
                    Json::Value entry(Json::objectValue);
                    entry["validation_public_key"] = strHex(m.pk());
                    entry["manifest"] = base64_encode(makeManifest(m, s, 1));
                    body["validators"].append(entry);
                };
                add(control, controlSigning);
                if (future)
                    add(master, signing);
                auto const raw = to_string(body);
                return ValidatorBlobInfo{
                    base64_encode(raw),
                    strHex(
                        sign(pubSigning.pk(), pubSigning.sk(), makeSlice(raw))),
                    {}};
            };
            auto const result = lists.applyLists(
                base64_encode(makeManifest(publisher, pubSigning, 1)),
                2,
                {list(false), list(true)},
                "test");
            BEAST_EXPECT(result.bestDisposition() == ListDisposition::accepted);
            BEAST_EXPECT(lists.listed(control.pk()));
            BEAST_EXPECT(!lists.listed(master.pk()));
        }
        else
            BEAST_EXPECT(lists.load(
                {}, {toBase58(TokenType::NodePublic, master.pk())}, {}));
        // Run the actual beginConsensus path, not a test-side cache refresh.
        if (scheduled)
            BEAST_EXPECT(env.close(effective + std::chrono::seconds{1}));
        else
            BEAST_EXPECT(env.close());
        BEAST_EXPECT(lists.listed(master.pk()));
        BEAST_EXPECT(cache.revoked(master.pk()));
        BEAST_EXPECT(!lists.trusted(master.pk()));
        BEAST_EXPECT(!lists.getQuorumKeys().second.contains(signing.pk()));
        BEAST_EXPECT(lists.trusted(control.pk()));
        BEAST_EXPECT(env.close());
        BEAST_EXPECT(cache.revoked(master.pk()));
        BEAST_EXPECT(!lists.trusted(master.pk()));
    }

    void
    testSigningKeyRetrieval(FeatureBitset features)
    {
        testcase("retrieval by ephemeral key");
        using namespace jtx;

        Env env{*this, makeConfig(), features};

        auto const master = Account("master", KeyType::ed25519);
        auto const eph1 = Account("eph1", KeyType::ed25519);
        auto const eph2 = Account("eph2", KeyType::ed25519);
        auto const stranger = Account("stranger", KeyType::ed25519);
        env.fund(XRP(1000), master);
        env.close();

        submit(env, signedEnvelope(env, makeManifest(master, eph1, 1), master));
        env.close();

        auto& cache = env.app().validatorManifests();

        // The situation applyLedger() cannot serve: a validation arrives
        // signed by eph1 and the node holds no manifest naming it, so the
        // master key to probe for is exactly what is missing.
        BEAST_EXPECT(cache.getMasterKey(eph1.pk()) == eph1.pk());

        BEAST_EXPECT(
            cache.applyLedgerSigningKey(*env.closed(), eph1.pk()) ==
            master.pk());
        BEAST_EXPECT(cache.getMasterKey(eph1.pk()) == master.pk());
        BEAST_EXPECT(cache.getSigningKey(master.pk()) == eph1.pk());
        BEAST_EXPECT(cache.getSequence(master.pk()) == 1);

        // At capacity, an unseen unlisted ledger identity is refused too.
        // Listing it enables reconciliation without discarding the old row.
        ManifestCache bounded{env.journal, 1};
        auto other = deserializeManifest(makeManifest(stranger, eph2, 1));
        BEAST_EXPECT(other);
        bounded.applyManifest(std::move(*other));
        BEAST_EXPECT(!bounded.applyLedgerSigningKey(*env.closed(), eph1.pk()));
        BEAST_EXPECT(bounded.getMasterKey(eph1.pk()) == eph1.pk());
        bounded.pin({master.pk()});
        BEAST_EXPECT(bounded.applyLedger(*env.closed(), {master.pk()}) == 1);
        BEAST_EXPECT(bounded.getMasterKey(eph2.pk()) == stranger.pk());
        auto const previousLedger = env.closed();
        env.close();
        BEAST_EXPECT(
            bounded.applyLedgerSigningKey(*env.closed(), eph1.pk()) ==
            master.pk());

        // Cycling past the negative-cache capacity must not reset the read
        // budget. A real on-ledger key waits for the next ledger once full.
        ManifestCache probes{env.journal};
        for (std::size_t i = 0; i <= ManifestCache::probeLimit; ++i)
        {
            auto const key =
                derivePublicKey(KeyType::secp256k1, randomSecretKey());
            BEAST_EXPECT(!probes.applyLedgerSigningKey(*env.closed(), key));
        }
        BEAST_EXPECT(
            !probes.applyLedgerSigningKey(*previousLedger, stranger.pk()));
        BEAST_EXPECT(!probes.applyLedgerSigningKey(*env.closed(), eph1.pk()));
        env.close();
        BEAST_EXPECT(
            probes.applyLedgerSigningKey(*env.closed(), eph1.pk()) ==
            master.pk());

        // A key with no manifest on-ledger resolves to nothing and leaves the
        // cache untouched.
        BEAST_EXPECT(
            !cache.applyLedgerSigningKey(*env.closed(), stranger.pk()));
        BEAST_EXPECT(cache.getMasterKey(stranger.pk()) == stranger.pk());

        // A rotation is recovered from the new ephemeral key alone, and the
        // superseded key stops resolving because its object is gone.
        submit(env, makeManifest(master, eph2, 2), master);
        env.close();

        BEAST_EXPECT(
            cache.applyLedgerSigningKey(*env.closed(), eph2.pk()) ==
            master.pk());
        BEAST_EXPECT(cache.getSigningKey(master.pk()) == eph2.pk());
        BEAST_EXPECT(cache.getMasterKey(eph1.pk()) == eph1.pk());
        BEAST_EXPECT(!env.le(keylet::manifest(eph1.pk())));
        BEAST_EXPECT(!cache.applyLedgerSigningKey(*env.closed(), eph1.pk()));

        // Asking again is answered from the cache, ahead of the per-ledger
        // probe bookkeeping.
        BEAST_EXPECT(
            cache.applyLedgerSigningKey(*env.closed(), eph2.pk()) ==
            master.pk());

        // A master key is not a signing key. The object at its keylet is a
        // perfectly good manifest and is ingested, but it binds eph2, not the
        // master key, so nothing is reported for the key asked about.
        BEAST_EXPECT(!cache.applyLedgerSigningKey(*env.closed(), master.pk()));

        // A revoked master publishes no ephemeral object at all, so this
        // direction goes quiet. The revocation still reaches the cache by
        // master key, which is what applyLedger() is for.
        submit(
            env,
            makeManifest(
                master, eph2, std::numeric_limits<std::uint32_t>::max()),
            master);
        env.close();

        BEAST_EXPECT(!env.le(keylet::manifest(eph2.pk())));
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
        env.fund(XRP(1000), master, other, ephemeral);
        env.close();

        // Not a manifest at all.
        BEAST_EXPECT(
            engineResult(submit(env, makeUnparseableManifest(), master)) ==
            "temMALFORMED");

        // A manifest whose ephemeral signature does not check out. The
        // account signature authenticates only the envelope.
        {
            auto blob = makeManifest(master, ephemeral, 1);
            blob[blob.size() - 1] ^= 0xFF;
            BEAST_EXPECT(
                engineResult(submit(env, blob, master)) == "temMALFORMED");
        }

        // The master key's account must exist: it signs, pays the fee, and
        // holds the pointer to the manifest.
        auto const unfunded = Account("unfunded", KeyType::ed25519);
        BEAST_EXPECT(
            engineResult(submit(
                env,
                signedEnvelope(
                    env, makeManifest(unfunded, ephemeral, 1), unfunded, 1))) ==
            "terNO_ACCOUNT");

        // An ephemeral key already claimed by a different account would
        // collide with -- and clobber -- that account's manifest object.
        submit(env, makeManifest(master, ephemeral, 1), master);
        env.close();

        BEAST_EXPECT(
            engineResult(
                submit(env, makeManifest(other, ephemeral, 1), other)) ==
            "tecDUPLICATE");

        // The reverse role collision uses the same keylet: an active signing
        // key cannot subsequently enrol as another validator's master key.
        BEAST_EXPECT(
            engineResult(
                submit(env, makeManifest(ephemeral, other, 1), ephemeral)) ==
            "tecDUPLICATE");
    }

    void
    testEnvelopeRejections(FeatureBitset features)
    {
        testcase("envelope rejections");
        using namespace jtx;

        Env env{*this, makeConfig(), features};

        auto const master = Account("master", KeyType::ed25519);
        auto const other = Account("other", KeyType::ed25519);
        auto const ephemeral = Account("ephemeral", KeyType::ed25519);
        env.fund(XRP(1000), master, other);
        env.close();

        auto const good = makeManifest(master, ephemeral, 1);

        // Sanity check: the unmodified transaction applies, so every rejection
        // below is attributable to its change and nothing else.
        BEAST_EXPECT(
            applyDirect(env, signedEnvelope(env, good, master)) == tesSUCCESS);

        // Any bit but tfFullyCanonicalSig (0x80000000), which is tfUniversal
        // and so permitted on every transaction.
        BEAST_EXPECT(
            applyDirect(
                env,
                signedEnvelope(
                    env,
                    good,
                    master,
                    std::nullopt,
                    std::nullopt,
                    [](STObject& obj) {
                        obj.setFieldU32(sfFlags, 0x00000001);
                    })) == temINVALID_FLAG);

        // The account signing must be the manifest's master key's account.
        BEAST_EXPECT(
            applyDirect(env, signedEnvelope(env, good, other)) == temMALFORMED);

        // A manifest whose signatures do not check out, including in a dry
        // run (simulate), which skips the outer signature check.
        {
            auto bad = good;
            bad[bad.size() - 1] ^= 0xFF;
            BEAST_EXPECT(
                applyDirect(env, signedEnvelope(env, bad, master)) ==
                temMALFORMED);

            // simulate submits the account's key with an empty signature.
            auto const simulated = std::make_shared<STTx const>(
                ttMANIFEST_SET, [&](STObject& obj) {
                    obj.setAccountID(sfAccount, master.id());
                    obj.setFieldU32(sfSequence, env.seq(master));
                    obj.setFieldU32(sfNetworkID, env.app().config().NETWORK_ID);
                    obj.setFieldAmount(sfFee, XRPAmount{100'000});
                    obj.setFieldVL(sfSigningPubKey, master.pk().slice());
                    obj.setFieldVL(sfTxnSignature, Blob{});
                    SerialIter mit{makeSlice(bad)};
                    obj.peekFieldObject(sfManifest).set(mit);
                });
            BEAST_EXPECT(
                applyDirect(env, simulated, {}, tapDRY_RUN) == temMALFORMED);
        }

        // There is no manifest-only authority: without the account's signature
        // a SetManifest is just an unsigned transaction.
        auto const unsignedTx =
            std::make_shared<STTx const>(ttMANIFEST_SET, [&](STObject& obj) {
                obj.setAccountID(sfAccount, master.id());
                obj.setFieldU32(sfSequence, env.seq(master));
                obj.setFieldU32(sfNetworkID, env.app().config().NETWORK_ID);
                obj.setFieldAmount(sfFee, XRPAmount{100'000});
                obj.setFieldVL(sfSigningPubKey, Blob{});
                obj.setFieldVL(sfTxnSignature, Blob{});
                SerialIter mit{makeSlice(good)};
                obj.peekFieldObject(sfManifest).set(mit);
            });
        BEAST_EXPECT(applyDirect(env, unsignedTx) == temINVALID);

        // Fee semantics are ordinary: at least the minimum, and more is fine.
        auto const priced = signedEnvelope(env, good, master);
        auto const baseFee = priced->getFieldAmount(sfFee).xrp();
        BEAST_EXPECT(
            applyDirect(
                env,
                signedEnvelope(
                    env, good, master, std::nullopt, baseFee - XRPAmount{1})) ==
            telINSUF_FEE_P);
        BEAST_EXPECT(
            applyDirect(
                env,
                signedEnvelope(
                    env,
                    good,
                    master,
                    std::nullopt,
                    baseFee + XRPAmount{100})) == tesSUCCESS);
    }

    void
    testCorruptLedger(FeatureBitset features)
    {
        testcase("corrupt manifest state");
        using namespace jtx;

        Env env{*this, makeConfig(), features};

        auto const master = Account("master", KeyType::ed25519);
        auto const eph1 = Account("eph1", KeyType::ed25519);
        auto const eph2 = Account("eph2", KeyType::ed25519);
        env.fund(XRP(1000), master);
        env.close();

        BEAST_EXPECT(
            engineResult(submit(
                env,
                signedEnvelope(env, makeManifest(master, eph1, 1), master))) ==
            "tesSUCCESS");
        env.close();

        // applyDirect() discards every attempt, so the one transaction (and
        // its account Sequence) can be applied repeatedly.
        auto const update =
            signedEnvelope(env, makeManifest(master, eph2, 2), master);

        // sfManifestID on the account root points at nothing.
        BEAST_EXPECT(applyDirect(env, update, [&](OpenView& view) {
                         rawErase(view, keylet::manifest(master.pk()));
                     }) == tefBAD_LEDGER);

        // The owned master copy survives but its full signing copy is gone.
        BEAST_EXPECT(applyDirect(env, update, [&](OpenView& view) {
                         rawErase(view, keylet::manifest(eph1.pk()));
                     }) == tefBAD_LEDGER);

        // Both copies must name each other; neither direction is merely a
        // coincidental duplicate occupying the expected keylet.
        BEAST_EXPECT(applyDirect(env, update, [&](OpenView& view) {
                         auto replacement =
                             rawCopy(view.read(keylet::manifest(master.pk())));
                         replacement->setFieldH256(
                             sfManifestID, keylet::manifest(eph2.pk()).key);
                         view.rawReplace(replacement);
                     }) == tefBAD_LEDGER);
        BEAST_EXPECT(applyDirect(env, update, [&](OpenView& view) {
                         auto replacement =
                             rawCopy(view.read(keylet::manifest(eph1.pk())));
                         replacement->setFieldH256(
                             sfManifestID, keylet::manifest(eph2.pk()).key);
                         view.rawReplace(replacement);
                     }) == tefBAD_LEDGER);

        // The reverse: the account root has forgotten its manifest while the
        // canonical keylet still holds this account's object. Preclaim
        // refuses that half-present slot before doApply runs.
        BEAST_EXPECT(applyDirect(env, update, [&](OpenView& view) {
                         auto replacement =
                             rawCopy(view.read(keylet::account(master.id())));
                         replacement->makeFieldAbsent(sfManifestID);
                         view.rawReplace(replacement);
                     }) == tefBAD_LEDGER);

        // None of the above was retained, so the ledger is still intact and an
        // ordinary update still works.
        BEAST_EXPECT(applyDirect(env, update) == tesSUCCESS);
    }

    void
    testGossipSelection(FeatureBitset features)
    {
        testcase("gossip selection");
        using namespace jtx;

        Env env{*this, makeConfig(), features};

        std::vector<Account> masters;
        for (int i = 0; i < 4; ++i)
            masters.emplace_back("gm" + std::to_string(i), KeyType::ed25519);

        auto const ephemeral = Account("gossipeph", KeyType::ed25519);

        for (auto const& m : masters)
            env.fund(XRP(1000), m);
        env.close();

        // Each master needs its own ephemeral key: preclaim rejects a key
        // already claimed by another account.
        std::vector<Account> ephs;
        for (int i = 0; i < 4; ++i)
            ephs.emplace_back("ge" + std::to_string(i), KeyType::ed25519);

        for (int i = 0; i < 4; ++i)
        {
            BEAST_EXPECT(
                engineResult(submit(
                    env,
                    signedEnvelope(
                        env,
                        makeManifest(masters[i], ephs[i], 1),
                        masters[i]))) == "tesSUCCESS");
            env.close();
        }

        ManifestCache cache;

        hash_set<PublicKey> all;
        for (auto const& m : masters)
            all.insert(m.pk());

        BEAST_EXPECT(cache.applyLedger(*env.closed(), all) == 4);

        // The raw form is what a republishing node needs: the exact bytes the
        // master key signed, plus the sequence to compare against the ledger.
        auto const raw = cache.getRawManifest(masters[0].pk());
        if (BEAST_EXPECT(raw))
        {
            BEAST_EXPECT(raw->first == 1);
            BEAST_EXPECT(!raw->second.empty());
        }
        BEAST_EXPECT(!cache.getRawManifest(ephemeral.pk()));

        // Pinning bumps the sequence so a cached gossip message is rebuilt,
        // and pinning the same set again does not.
        auto const seq = cache.sequence();
        cache.pin({masters[0].pk(), masters[1].pk()});
        BEAST_EXPECT(cache.sequence() > seq);

        auto const seq2 = cache.sequence();
        cache.pin({masters[0].pk(), masters[1].pk()});
        BEAST_EXPECT(cache.sequence() == seq2);

        // Ledger discoveries remain usable locally, but only listed identities
        // are offered to peers. Reading a cached key cannot promote it to
        // gossip.
        BEAST_EXPECT(cache.getMasterKey(ephs[2].pk()) == masters[2].pk());
        std::size_t reserved = 0;
        std::vector<PublicKey> offered;
        cache.for_each_gossip_manifest(
            [&](std::size_t n) { reserved = n; },
            [&](Manifest const& m) { offered.push_back(m.masterKey); });

        BEAST_EXPECT(reserved == 2);
        BEAST_EXPECT(offered.size() == 2);
        BEAST_EXPECT(
            hash_set<PublicKey>(offered.begin(), offered.end()) ==
            hash_set<PublicKey>({masters[0].pk(), masters[1].pk()}));

        // A pinned key with no manifest is counted in the reservation but not
        // offered, since the reservation is only an upper bound.
        cache.pin({masters[0].pk(), ephemeral.pk()});
        offered.clear();
        cache.for_each_gossip_manifest(
            [&](std::size_t n) { reserved = n; },
            [&](Manifest const& m) { offered.push_back(m.masterKey); });

        BEAST_EXPECT(reserved == 2);
        BEAST_EXPECT(offered.size() == 1);
        BEAST_EXPECT(offered.front() == masters[0].pk());
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
            engineResult(
                submit(env, makeManifest(master, ephemeral, 1), master)) ==
            "temDISABLED");
        env.close();

        BEAST_EXPECT(!env.le(keylet::manifest(master.pk())));
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        // OnChainManifests is not yet Supported::yes, so enable it explicitly
        // for the on-ledger cases.
        auto const sa = supported_amendments() | featureOnChainManifests;

        testSubmission(sa);
        testUpdate(sa);
        testRevocation(sa);
        testOwnership(sa);
        testRetrieval(sa);
        testListedAfterLedgerRevocation(sa);
        testListedAfterLedgerRevocation(sa, true);
        testSigningKeyRetrieval(sa);
        testMalformed(sa);
        testEnvelopeRejections(sa);
        testCorruptLedger(sa);
        testGossipSelection(sa);
        testDisabled(sa);
    }
};

BEAST_DEFINE_TESTSUITE(SetManifest, app, ripple);

}  // namespace test
}  // namespace ripple
