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

    /** The envelope Submit would build for `manifest`, optionally tweaked.

        makeSetManifestTx() rejects any manifest the transactor would reject,
        and checkValidity() rejects any envelope whose signatures do not check
        out, so several of the transactor's own rejections cannot be provoked
        through the submit RPC at all. Building the envelope here and applying
        it straight to the open view reaches them.
    */
    static std::shared_ptr<STTx const>
    envelope(
        jtx::Env& env,
        std::string const& manifest,
        AccountID const& account,
        std::function<void(STObject&)> const& tweak = {})
    {
        auto const build = [&](XRPAmount fee, bool tweaked) {
            return std::make_shared<STTx const>(
                ttMANIFEST_SET, [&](STObject& obj) {
                    obj.setAccountID(sfAccount, account);
                    obj.setFieldU32(sfSequence, 0);
                    obj.setFieldU32(sfNetworkID, env.app().config().NETWORK_ID);
                    obj.setFieldAmount(sfFee, fee);
                    obj.setFieldVL(sfSigningPubKey, Blob{});
                    obj.setFieldVL(sfTxnSignature, Blob{});

                    SerialIter mit{makeSlice(manifest)};
                    obj.peekFieldObject(sfManifest).set(mit);

                    if (tweaked && tweak)
                        tweak(obj);
                });
        };

        // The same two-pass pricing makeSetManifestTx() does: encode once with
        // a placeholder purely to have something to measure, then encode at the
        // ceiling checkFee() will accept.
        auto const base = SetManifest::calculateBaseFee(
            *env.current(), *build(XRPAmount{0}, false));

        return build(mulRatio(base, 12, 10, /*roundUp*/ true), true);
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
        std::function<void(OpenView&)> const& prepare = {})
    {
        TER ret = tesSUCCESS;
        env.app().openLedger().modify([&](OpenView& view, beast::Journal j) {
            if (prepare)
                prepare(view);
            ret = ripple::apply(env.app(), view, *tx, tapNONE, j).ter;
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
            engineResult(submit(env, makeManifest(master, signing, 1))) ==
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
                    master,
                    signing,
                    std::numeric_limits<std::uint32_t>::max()))) ==
            "tesSUCCESS");
        env.close();
        BEAST_EXPECT(
            env.le(keylet::manifest(master.pk()))->getFieldU32(sfSequence) ==
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

        submit(env, makeManifest(master, eph1, 1));
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
        submit(env, makeManifest(master, eph2, 2));
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
                master, eph2, std::numeric_limits<std::uint32_t>::max()));
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

        // Sanity check: the unmodified envelope is the one Submit builds, so
        // every rejection below is attributable to the tweak and nothing else.
        BEAST_EXPECT(
            applyDirect(env, envelope(env, good, master.id())) == tesSUCCESS);

        // Any bit but tfFullyCanonicalSig (0x80000000), which is tfUniversal
        // and so permitted on every transaction.
        BEAST_EXPECT(
            applyDirect(
                env, envelope(env, good, master.id(), [](STObject& obj) {
                    obj.setFieldU32(sfFlags, 0x00000001);
                })) == temINVALID_FLAG);

        // sfManifest parses as an object but not as a manifest.
        BEAST_EXPECT(
            applyDirect(
                env, envelope(env, makeUnparseableManifest(), master.id())) ==
            temMALFORMED);

        // A manifest whose signatures do not check out. checkValidity() would
        // stop this before preflight, so only a direct apply reaches the
        // transactor's own verify() call.
        {
            auto bad = good;
            bad[bad.size() - 1] ^= 0xFF;
            BEAST_EXPECT(
                applyDirect(env, envelope(env, bad, master.id())) ==
                temMALFORMED);
        }

        // The envelope's account must be the manifest's master key.
        BEAST_EXPECT(
            applyDirect(env, envelope(env, good, other.id())) == temMALFORMED);

        // Every envelope field a relayer could otherwise choose is pinned.
        for (auto const& [name, tweak] : std::vector<
                 std::pair<char const*, std::function<void(STObject&)>>>{
                 {"Sequence",
                  [](STObject& obj) { obj.setFieldU32(sfSequence, 1); }},
                 {"AccountTxnID",
                  [](STObject& obj) {
                      obj.setFieldH256(sfAccountTxnID, uint256{1});
                  }},
                 {"TicketSequence",
                  [](STObject& obj) { obj.setFieldU32(sfTicketSequence, 1); }},
                 {"SigningPubKey", [&](STObject& obj) {
                      obj.setFieldVL(sfSigningPubKey, master.pk().slice());
                  }}})
        {
            BEAST_EXPECTS(
                applyDirect(env, envelope(env, good, master.id(), tweak)) ==
                    temMALFORMED,
                name);
        }

        // sfFee is the one envelope field preflight cannot bound, because the
        // base fee is not in scope until preclaim. checkFee() caps it instead.
        auto const priced = envelope(env, good, master.id());
        auto const ceiling = priced->getFieldAmount(sfFee).xrp();

        BEAST_EXPECT(
            applyDirect(
                env, envelope(env, good, master.id(), [&](STObject& obj) {
                    obj.setFieldAmount(sfFee, ceiling + XRPAmount{1});
                })) == temBAD_FEE);

        // At the ceiling exactly, which is what Submit sends.
        BEAST_EXPECT(
            applyDirect(
                env, envelope(env, good, master.id(), [&](STObject& obj) {
                    obj.setFieldAmount(sfFee, ceiling);
                })) == tesSUCCESS);
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
            engineResult(submit(env, makeManifest(master, eph1, 1))) ==
            "tesSUCCESS");
        env.close();

        auto const update =
            envelope(env, makeManifest(master, eph2, 2), master.id());

        // sfManifestID on the account root points at nothing.
        BEAST_EXPECT(applyDirect(env, update, [&](OpenView& view) {
                         rawErase(view, keylet::manifest(master.pk()));
                     }) == tefBAD_LEDGER);

        // The master copy survives but the ephemeral copy it points at is
        // gone.
        BEAST_EXPECT(applyDirect(env, update, [&](OpenView& view) {
                         rawErase(view, keylet::manifest(eph1.pk()));
                     }) == tefBAD_LEDGER);

        // The reverse: the account root has forgotten its manifest, so the
        // erase pass is skipped and the keylet is found occupied.
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
                engineResult(
                    submit(env, makeManifest(masters[i], ephs[i], 1))) ==
                "tesSUCCESS");
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

        // The submitted transaction carries no account signature, so with the
        // amendment off it would otherwise be rejected as unsigned and never
        // reach preflight's temDISABLED. The RPC therefore refuses it up
        // front, and says why.
        auto const result = submit(env, makeManifest(master, ephemeral, 1));
        BEAST_EXPECT(result[jss::error].asString() == "notEnabled");
        BEAST_EXPECT(!result.isMember(jss::engine_result));
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
