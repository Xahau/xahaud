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
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/app/tx/detail/SetManifest.h>
#include <xrpld/core/Config.h>
#include <xrpld/ledger/OpenView.h>
#include <xrpl/basics/StringUtilities.h>
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

    /** Submits an already formed SetManifest transaction. */
    static Json::Value
    submit(jtx::Env& env, std::shared_ptr<STTx const> const& tx)
    {
        Serializer s;
        tx->add(s);
        return env.rpc("submit", strHex(s.slice()))[jss::result];
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

        SerialIter mit{makeSlice(manifest)};
        STObject const manifestObject{mit, sfManifest};
        return build(
            canonicalUnsignedSetManifestFee(
                env.current()->rules(), manifestObject),
            true);
    }

    /** An ordinary account-signed SetManifest envelope.

        This is the only lane allowed to create an account's first manifest
        slot. It uses the account's current Sequence unless a test overrides
        it, and its outer signature authenticates the complete transaction.
    */
    static std::shared_ptr<STTx const>
    signedEnvelope(
        jtx::Env& env,
        std::string const& manifest,
        jtx::Account const& account,
        std::optional<std::uint32_t> sequence = std::nullopt,
        std::optional<XRPAmount> fee = std::nullopt)
    {
        auto const build = [&](XRPAmount fee) {
            auto tx =
                std::make_shared<STTx>(ttMANIFEST_SET, [&](STObject& obj) {
                    obj.setAccountID(sfAccount, account.id());
                    obj.setFieldU32(
                        sfSequence, sequence.value_or(env.seq(account)));
                    obj.setFieldU32(sfNetworkID, env.app().config().NETWORK_ID);
                    obj.setFieldAmount(sfFee, fee);
                    obj.setFieldVL(sfSigningPubKey, account.pk().slice());

                    SerialIter mit{makeSlice(manifest)};
                    obj.peekFieldObject(sfManifest).set(mit);
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

        auto const first = makeManifest(master, ephemeral, 1);

        // Possessing a valid manifest does not authorize creation of its
        // account-backed slot. The refusal claims neither a fee nor Sequence.
        auto const balanceBefore = env.balance(master);
        auto const sequenceBefore = env.seq(master);
        BEAST_EXPECT(!onLedgerManifestSequence(*env.current(), master.pk()));
        BEAST_EXPECT(engineResult(submit(env, first)) == "tefBAD_AUTH");
        BEAST_EXPECT(!env.le(keylet::manifest(master.pk())));
        BEAST_EXPECT(!onLedgerManifestSequence(*env.current(), master.pk()));
        BEAST_EXPECT(env.balance(master) == balanceBefore);
        BEAST_EXPECT(env.seq(master) == sequenceBefore);

        // First registration is an ordinary account-signed transaction.
        BEAST_EXPECT(
            engineResult(submit(env, signedEnvelope(env, first, master))) ==
            "tesSUCCESS");
        BEAST_EXPECT(
            onLedgerManifestSequence(*env.current(), master.pk()) == 1);
        env.close();

        // The full manifest is canonical at the master key. The active
        // signing key gets only the cold-lookup pointer.
        auto const byMaster = env.le(keylet::manifest(master.pk()));
        auto const byEphemeral =
            env.le(keylet::manifestSigningKey(ephemeral.pk()));
        if (!BEAST_EXPECT(byMaster))
            return;
        if (!BEAST_EXPECT(byEphemeral))
            return;

        BEAST_EXPECT(byMaster->getAccountID(sfAccount) == master.id());
        BEAST_EXPECT(byMaster->getType() == ltMANIFEST);
        BEAST_EXPECT(byEphemeral->getType() == ltMANIFEST_SIGNING_KEY);
        BEAST_EXPECT(byMaster->isFieldPresent(sfOwnerNode));
        BEAST_EXPECT(byMaster->getFieldU32(sfSequence) == 1);
        BEAST_EXPECT(!byMaster->isFieldPresent(sfManifestID));
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

        // Rotation rewrites the canonical object and replaces the thin index.
        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(master, eph2, 2))) ==
            "tesSUCCESS");
        env.close();

        BEAST_EXPECT(!env.le(keylet::manifestSigningKey(eph1.pk())));
        if (!BEAST_EXPECT(env.le(keylet::manifestSigningKey(eph2.pk()))))
            return;
        BEAST_EXPECT(
            env.le(keylet::manifest(master.pk()))->getFieldU32(sfSequence) ==
            2);
        BEAST_EXPECT(
            env.le(keylet::manifest(master.pk()))->getFieldU64(sfOwnerNode) ==
            ownerNode);
        BEAST_EXPECT(env.ownerCount(master) == 1);
        BEAST_EXPECT(env.seq(master) == accountSequence);

        // The account-signed lane uses ordinary replay protection. A stale
        // outer Sequence is rejected before the manifest sequence matters;
        // the correct one both rotates the manifest and advances the account.
        BEAST_EXPECT(
            engineResult(submit(
                env,
                signedEnvelope(
                    env,
                    makeManifest(master, eph3, 3),
                    master,
                    accountSequence - 1))) == "tefPAST_SEQ");
        BEAST_EXPECT(!env.le(keylet::manifestSigningKey(eph3.pk())));

        BEAST_EXPECT(
            engineResult(submit(
                env,
                signedEnvelope(env, makeManifest(master, eph3, 3), master))) ==
            "tesSUCCESS");
        env.close();
        BEAST_EXPECT(env.seq(master) == accountSequence + 1);
        BEAST_EXPECT(!env.le(keylet::manifestSigningKey(eph2.pk())));
        BEAST_EXPECT(env.le(keylet::manifestSigningKey(eph3.pk())));
        BEAST_EXPECT(
            env.le(keylet::manifest(master.pk()))->getFieldU64(sfOwnerNode) ==
            ownerNode);
        BEAST_EXPECT(env.ownerCount(master) == 1);

        // Replaying older manifests through the unsigned lane is rejected by
        // manifest sequence, independently of the account Sequence.
        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(master, eph2, 2))) ==
            "tefPAST_MANIFEST_SEQ");
        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(master, eph1, 1))) ==
            "tefPAST_MANIFEST_SEQ");
    }

    void
    testCanonicalFee(FeatureBitset features)
    {
        testcase("canonical unsigned fee");
        using namespace jtx;

        auto const master = Account("master", KeyType::ed25519);
        auto const otherMaster = Account("other-master", KeyType::ed25519);
        auto const eph1 = Account("eph1", KeyType::ed25519);
        auto const eph2 = Account("eph2", KeyType::ed25519);
        auto const otherEph = Account("other-eph", KeyType::ed25519);
        auto const registration = makeManifest(master, eph1, 1);
        auto const otherRegistration = makeManifest(otherMaster, otherEph, 1);
        auto const update = makeManifest(master, eph2, 2);

        std::optional<std::string> ordinaryHex;
        {
            Env ordinary{*this, makeConfig("10"), features};
            ordinaryHex = makeSetManifestTx(
                makeSlice(update),
                ordinary.app().config().NETWORK_ID,
                ordinary.current()->rules(),
                ordinary.app().journal("SetManifest_test"));
            if (!BEAST_EXPECT(ordinaryHex))
                return;

            auto const bytes = strUnHex(*ordinaryHex);
            if (!BEAST_EXPECT(bytes))
                return;
            SerialIter txIter{makeSlice(*bytes)};
            STTx const tx{std::ref(txIter)};
            auto const& manifestObject =
                const_cast<STTx&>(tx).getField(sfManifest).downcast<STObject>();
            BEAST_EXPECT(
                tx[sfFee].xrp() ==
                canonicalUnsignedSetManifestFee(
                    ordinary.current()->rules(), manifestObject));
            BEAST_EXPECT(
                tx[sfFee].xrp().drops() ==
                1'000 +
                    100 *
                        static_cast<std::int64_t>(
                            manifestObject.getSerializer().getDataLength()));
        }

        // The canonical wrapper is independent of the current ledger's voted
        // reference fee. At a higher minimum the exact same txid waits; the
        // anti-entropy loop retries it rather than minting fee variants.
        Env expensive{*this, makeConfig("100000"), features};
        auto const expensiveHex = makeSetManifestTx(
            makeSlice(update),
            expensive.app().config().NETWORK_ID,
            expensive.current()->rules(),
            expensive.app().journal("SetManifest_test"));
        BEAST_EXPECT(expensiveHex == ordinaryHex);

        expensive.fund(XRP(1000), master);
        expensive.close();
        BEAST_EXPECT(
            engineResult(submit(
                expensive, signedEnvelope(expensive, registration, master))) ==
            "tesSUCCESS");
        expensive.close();
        BEAST_EXPECT(
            engineResult(submit(expensive, update)) == "telINSUF_FEE_P");

        // Sequence 0 is not permission to jump an escalated open ledger. A
        // canonical rotation is direct-only, pays its fixed canonical Fee,
        // and retries with the same txid once ordinary load subsides.
        auto escalatedConfig = makeConfig("10");
        escalatedConfig->section("transaction_queue")
            .set("minimum_txn_in_ledger_standalone", "3");
        Env escalated{*this, std::move(escalatedConfig), features};
        escalated.fund(XRP(1000), master, otherMaster);
        escalated.close();
        BEAST_EXPECT(
            engineResult(submit(
                escalated, signedEnvelope(escalated, registration, master))) ==
            "tesSUCCESS");
        BEAST_EXPECT(
            engineResult(submit(
                escalated,
                signedEnvelope(escalated, otherRegistration, otherMaster))) ==
            "tesSUCCESS");
        escalated.close();

        // The live cache already knows that otherMaster is a master key, so
        // the collision below is rejected by both the manifest plane and the
        // ledger without contaminating the later rotation control.
        auto otherHeld = deserializeManifest(otherRegistration);
        BEAST_EXPECT(otherHeld);
        if (!otherHeld)
            return;
        BEAST_EXPECT(
            escalated.app().validatorManifests().applyManifest(
                std::move(*otherHeld)) == ManifestDisposition::accepted);

        // High-fee ordinary traffic establishes load without itself queuing.
        escalated(noop(master), fee(XRP(1)));
        escalated(noop(master), fee(XRP(1)));
        escalated(noop(master), fee(XRP(1)));
        escalated(noop(master), fee(XRP(1)));
        escalated(noop(master), fee(XRP(1)));

        // A genuine preclaim failure remains precise under load; it must not
        // be rewritten as fee pressure merely because it could claim a fee.
        BEAST_EXPECT(
            engineResult(
                submit(escalated, makeManifest(master, otherMaster, 2))) ==
            "tecDUPLICATE");
        BEAST_EXPECT(
            engineResult(submit(escalated, update)) == "telINSUF_FEE_P");

        // The manifest authority is useful immediately even though ordinary
        // load has postponed its durable transaction. Run only the manifest
        // job lane before inspecting the cache.
        escalated.app().getJobQueue().rendezvous();
        auto const held =
            escalated.app().validatorManifests().getRawManifest(master.pk());
        BEAST_EXPECT(held && held->first == 2);
        BEAST_EXPECT(
            escalated.le(keylet::manifest(master.pk()))
                ->getFieldU32(sfSequence) == 1);
        BEAST_EXPECT(
            escalated.app().getTxQ().getMetrics(*escalated.current()).txCount ==
            0);

        // Local transaction retention retries this exact txid while opening
        // the next ledger; no fee variant or TxQ entry is involved.
        escalated.close();
        BEAST_EXPECT(
            escalated.le(keylet::manifest(master.pk()))
                ->getFieldU32(sfSequence) == 2);
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
                    std::numeric_limits<std::uint32_t>::max()))) ==
            "tesSUCCESS");
        env.close();

        // A revocation remains canonical, but has no active signing-key index.
        auto const byMaster = env.le(keylet::manifest(master.pk()));
        if (!BEAST_EXPECT(byMaster))
            return;
        BEAST_EXPECT(!env.le(keylet::manifestSigningKey(ephemeral.pk())));
        BEAST_EXPECT(!byMaster->isFieldPresent(sfManifestID));
        BEAST_EXPECT(!byMaster->isFieldPresent(sfSigningPubKey));
        BEAST_EXPECT(
            byMaster->getFieldU32(sfSequence) ==
            std::numeric_limits<std::uint32_t>::max());
        BEAST_EXPECT(env.ownerCount(master) == 1);

        // Nothing supersedes a revocation.
        BEAST_EXPECT(
            engineResult(submit(env, makeManifest(master, ephemeral, 2))) ==
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
            BEAST_EXPECT(!env.le(keylet::manifestSigningKey(ephemeral.pk())));
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
                        std::numeric_limits<std::uint32_t>::max()))) ==
                "tesSUCCESS");
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

        // Isolate ledger retrieval from the live application cache, which is
        // now intentionally freshened at transaction ingress.
        ManifestCache cache{env.app().journal("SetManifest_test")};

        // Gossip may arrive first. A different valid manifest at the same
        // sequence is provisional until a validated ledger breaks the tie.
        auto conflict = deserializeManifest(makeManifest(master, eph2, 1));
        BEAST_EXPECT(conflict);
        if (!conflict)
            return;
        BEAST_EXPECT(
            cache.applyManifest(std::move(*conflict)) ==
            ManifestDisposition::accepted);
        BEAST_EXPECT(cache.getSigningKey(master.pk()) == eph2.pk());

        // Reading the ledger resolves both directions of the mapping. The
        // validated ledger wins the equal-sequence conflict.
        BEAST_EXPECT(cache.applyLedger(*env.closed(), {master.pk()}) == 1);
        BEAST_EXPECT(cache.getMasterKey(eph1.pk()) == master.pk());
        BEAST_EXPECT(cache.getSigningKey(master.pk()) == eph1.pk());
        BEAST_EXPECT(cache.getSequence(master.pk()) == 1);
        BEAST_EXPECT(cache.getMasterKey(eph2.pk()) == eph2.pk());

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

        // Isolate cold signing-key retrieval from the live application cache,
        // which is now intentionally freshened at transaction ingress.
        ManifestCache cache{env.app().journal("SetManifest_test")};

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
        BEAST_EXPECT(!env.le(keylet::manifestSigningKey(eph1.pk())));
        BEAST_EXPECT(!cache.applyLedgerSigningKey(*env.closed(), eph1.pk()));

        // Asking again is answered from the cache, ahead of the per-ledger
        // probe bookkeeping.
        BEAST_EXPECT(
            cache.applyLedgerSigningKey(*env.closed(), eph2.pk()) ==
            master.pk());

        // A master key is not a signing key and therefore has no inverse
        // index. Master-key reconciliation uses applyLedger() directly.
        BEAST_EXPECT(!cache.applyLedgerSigningKey(*env.closed(), master.pk()));

        // A revoked master publishes no ephemeral object at all, so this
        // direction goes quiet. The revocation still reaches the cache by
        // master key, which is what applyLedger() is for.
        submit(
            env,
            makeManifest(
                master, eph2, std::numeric_limits<std::uint32_t>::max()));
        env.close();

        BEAST_EXPECT(!env.le(keylet::manifestSigningKey(eph2.pk())));
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
        submit(
            env,
            signedEnvelope(env, makeManifest(master, ephemeral, 1), master));
        env.close();

        BEAST_EXPECT(
            engineResult(submit(
                env,
                signedEnvelope(
                    env, makeManifest(other, ephemeral, 1), other))) ==
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

        // Establish the slot through the account-authorized lane so the
        // unsigned envelopes below exercise update-envelope validation.
        BEAST_EXPECT(
            engineResult(submit(
                env,
                signedEnvelope(
                    env, makeManifest(master, ephemeral, 0), master))) ==
            "tesSUCCESS");
        env.close();

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

        // Outer authority is checked before manifest parsing. An unsigned
        // envelope delegates that check to the embedded manifest, so malformed
        // or badly signed manifest bytes are signature failures here.
        BEAST_EXPECT(
            applyDirect(
                env, envelope(env, makeUnparseableManifest(), master.id())) ==
            temINVALID);

        // A manifest whose signatures do not check out.
        {
            auto bad = good;
            bad[bad.size() - 1] ^= 0xFF;
            BEAST_EXPECT(
                applyDirect(env, envelope(env, bad, master.id())) ==
                temINVALID);

            // Envelope shape is cheaper than either manifest signature. Even
            // at RPC/overlay ingress, the same bad manifest plus a Memo is
            // rejected for its envelope before signature work.
            auto const badWithMemo =
                envelope(env, bad, master.id(), [](STObject& obj) {
                    obj.setFieldArray(sfMemos, STArray(sfMemos, 1));
                    STObject memo{sfMemo};
                    memo.setFieldVL(sfMemoData, Blob{0x01});
                    obj.peekFieldArray(sfMemos).emplace_back(std::move(memo));
                });
            auto const [validity, reason] = checkValidity(
                env.app().getHashRouter(),
                *badWithMemo,
                env.current()->rules(),
                env.app().config());
            BEAST_EXPECT(validity == Validity::SigBad);
            BEAST_EXPECT(
                reason == "Manifest-authorized envelope is not canonical");
            BEAST_EXPECT(applyDirect(env, badWithMemo) == temMALFORMED);
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
                 {"Memos",
                  [](STObject& obj) {
                      obj.setFieldArray(sfMemos, STArray(sfMemos, 1));
                      STObject memo{sfMemo};
                      memo.setFieldVL(sfMemoData, Blob{0x01});
                      obj.peekFieldArray(sfMemos).emplace_back(std::move(memo));
                  }},
                 {"TicketSequence",
                  [](STObject& obj) { obj.setFieldU32(sfTicketSequence, 1); }}})
        {
            BEAST_EXPECTS(
                applyDirect(env, envelope(env, good, master.id(), tweak)) ==
                    temMALFORMED,
                name);
        }

        // A non-empty signing key changes lanes. Without the corresponding
        // account signature this is an invalid ordinary signed transaction,
        // not a canonical unsigned envelope.
        BEAST_EXPECT(
            applyDirect(
                env, envelope(env, good, master.id(), [&](STObject& obj) {
                    obj.setFieldVL(sfSigningPubKey, master.pk().slice());
                })) == temINVALID);

        BEAST_EXPECT(
            applyDirect(
                env, envelope(env, good, master.id(), [&](STObject& obj) {
                    obj.setFieldU32(sfLastLedgerSequence, env.current()->seq());
                })) == temMALFORMED);

        // sfFee is mirrored by the shape check and pinned to one ruleset-fixed
        // base-plus-payload value in checkFee().
        auto const priced = envelope(env, good, master.id());
        auto const canonicalFee = priced->getFieldAmount(sfFee).xrp();

        auto const nonCanonicalFee =
            envelope(env, good, master.id(), [&](STObject& obj) {
                obj.setFieldAmount(sfFee, canonicalFee + XRPAmount{1});
            });
        auto const [feeValidity, feeReason] = checkValidity(
            env.app().getHashRouter(),
            *nonCanonicalFee,
            env.current()->rules(),
            env.app().config());
        BEAST_EXPECT(feeValidity == Validity::SigBad);
        BEAST_EXPECT(
            feeReason == "Manifest-authorized envelope has non-canonical fee");
        BEAST_EXPECT(applyDirect(env, nonCanonicalFee) == temBAD_FEE);

        // The canonical envelope has one txid, so its manifest signatures and
        // local checks use the ordinary HashRouter receipts across ingress
        // layers rather than being repeated in NetworkOPs.
        auto const cacheProbe =
            envelope(env, makeManifest(master, ephemeral, 1234), master.id());
        auto const [cacheValidity, cacheReason] = checkValidity(
            env.app().getHashRouter(),
            *cacheProbe,
            env.current()->rules(),
            env.app().config());
        BEAST_EXPECT(cacheValidity == Validity::Valid);
        BEAST_EXPECT(cacheReason.empty());
        auto const cacheFlags =
            env.app().getHashRouter().getFlags(cacheProbe->getTransactionID());
        BEAST_EXPECT(cacheFlags & SF_PRIVATE2);
        BEAST_EXPECT(cacheFlags & SF_PRIVATE4);

        BEAST_EXPECT(
            applyDirect(
                env, envelope(env, good, master.id(), [&](STObject& obj) {
                    obj.setFieldAmount(sfFee, canonicalFee - XRPAmount{1});
                })) == temBAD_FEE);

        // At the canonical value exactly, which is what Submit sends.
        BEAST_EXPECT(
            applyDirect(
                env, envelope(env, good, master.id(), [&](STObject& obj) {
                    obj.setFieldAmount(sfFee, canonicalFee);
                })) == tesSUCCESS);

        // The account signature authenticates its envelope, so the signed
        // lane deliberately uses ordinary configurable-fee semantics rather
        // than the canonical unsigned Fee.
        BEAST_EXPECT(
            applyDirect(
                env,
                signedEnvelope(
                    env,
                    makeManifest(master, ephemeral, 2),
                    master,
                    std::nullopt,
                    canonicalFee + XRPAmount{100})) == tesSUCCESS);
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

        auto const update =
            envelope(env, makeManifest(master, eph2, 2), master.id());

        // sfManifestID on the account root points at nothing.
        BEAST_EXPECT(applyDirect(env, update, [&](OpenView& view) {
                         rawErase(view, keylet::manifest(master.pk()));
                     }) == tefBAD_LEDGER);

        // The canonical object survives but its thin signing index is gone.
        BEAST_EXPECT(applyDirect(env, update, [&](OpenView& view) {
                         rawErase(view, keylet::manifestSigningKey(eph1.pk()));
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

        // Everything is offered: two pinned plus two under the gossip limit.
        std::size_t reserved = 0;
        std::vector<PublicKey> offered;
        cache.for_each_gossip_manifest(
            [&](std::size_t n) { reserved = n; },
            [&](Manifest const& m) { offered.push_back(m.masterKey); });

        BEAST_EXPECT(reserved == 4);
        BEAST_EXPECT(offered.size() == 4);
        BEAST_EXPECT(
            hash_set<PublicKey>(offered.begin(), offered.end()) == all);

        // A pinned key with no manifest is counted in the reservation but not
        // offered, since the reservation is only an upper bound.
        cache.pin({masters[0].pk(), ephemeral.pk()});
        offered.clear();
        cache.for_each_gossip_manifest(
            [&](std::size_t n) { reserved = n; },
            [&](Manifest const& m) { offered.push_back(m.masterKey); });

        BEAST_EXPECT(reserved == 5);
        BEAST_EXPECT(offered.size() == 4);
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
        testCanonicalFee(sa);
        testRevocation(sa);
        testOwnership(sa);
        testRetrieval(sa);
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
