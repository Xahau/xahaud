//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

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

#include <test/app/Export_test_hooks.h>
#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <test/jtx/import.h>
#include <test/jtx/unl.h>
#include <test/jtx/xpop.h>
#include <xrpld/app/consensus/ConsensusExtensions.h>
#include <xrpld/app/ledger/BuildLedger.h>
#include <xrpld/app/ledger/InboundTransactions.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/ledger/LedgerReplay.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/misc/CanonicalTXSet.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/misc/RuntimeConfig.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/app/tx/detail/ExportLedgerOps.h>
#include <xrpld/app/tx/detail/ExportResultBuilder.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/ExportCommittee.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/ExportOriginMemo.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Import.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/jss.h>

#include <algorithm>
#include <map>
#include <set>

namespace ripple {
namespace test {

using TestHook = std::vector<uint8_t> const&;

// Large fee for hook operations
#define HSFEE fee(100'000'000)

struct Export_test : public beast::unit_test::suite
{
    static std::unique_ptr<Config>
    exportTestConfig()
    {
        auto cfg = jtx::envconfig(jtx::validator, "");
        cfg->NETWORK_ID = 21337;
        return cfg;
    }

    static void
    forceNonStandalone(Application& app)
    {
        const_cast<Config&>(app.config()).setupControl(true, true, false);
    }

    static STTx
    makeSTTx(STObject const& obj)
    {
        Serializer s;
        obj.add(s);
        SerialIter sit{s.slice()};
        return STTx{std::ref(sit)};
    }

    static std::uint32_t
    importVLSequence(jtx::Env const& env, PublicKey const& pk)
    {
        auto const sle = env.le(keylet::import_vlseq(pk));
        if (sle && sle->isFieldPresent(sfImportSequence))
            return (*sle)[sfImportSequence];
        return 0;
    }

    struct CallbackXPOP
    {
        Json::Value xpopJson;
        std::uint32_t ticketSeq;
        uint256 originTxn;
        std::optional<std::pair<std::uint32_t, PublicKey>> vlInfo;
    };

    CallbackXPOP
    buildExportCallbackXPOP(
        jtx::Env& xahau,
        jtx::xpop::TestXPOPContext const& xpopCtx,
        jtx::Account const& alice,
        jtx::Account const& carol,
        std::uint32_t targetNetworkID,
        std::uint32_t ticketSeq)
    {
        using namespace jtx;

        STObject innerObj(sfExportedTxn);
        innerObj.setFieldU16(sfTransactionType, ttPAYMENT);
        innerObj.setFieldU32(sfFlags, tfFullyCanonicalSig);
        innerObj.setFieldU32(sfSequence, 0);
        innerObj.setFieldU32(sfTicketSequence, ticketSeq);
        innerObj.setFieldU32(sfNetworkID, targetNetworkID);
        innerObj.setFieldU32(sfLastLedgerSequence, 100);
        innerObj.setFieldAmount(sfAmount, XRPAmount{1000000});
        innerObj.setFieldAmount(sfFee, XRPAmount{1000});
        innerObj.setFieldVL(sfSigningPubKey, Blob{});
        innerObj.setAccountID(sfAccount, alice.id());
        innerObj.setAccountID(sfDestination, carol.id());

        Json::Value jvExport;
        jvExport[jss::TransactionType] = jss::Export;
        jvExport[jss::Account] = alice.human();
        jvExport[jss::LastLedgerSequence] =
            xahau.current()->seq() + ExportLimits::maxAdmissionWindowLedgers;
        jvExport[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

        auto const& valKeys = xahau.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return {};
        seedUNLReportLedger(xahau, {valKeys.keys->masterPublicKey});
        bindExportAuthority(xahau, jvExport);

        xahau(jvExport, fee(XRP(1)), ter(tesSUCCESS));
        auto const origin = xahau.tx()->getTransactionID();
        xahau.close();

        auto const latchKey = keylet::exportLatch(alice.id(), origin);
        BEAST_EXPECT(xahau.current()->exists(latchKey));

        Blob multisignedBlob;
        xahau.close();
        for (auto const& [witnessTx, _] : xahau.closed()->txs)
        {
            if (witnessTx->getTxnType() != ttEXPORT_SIGNATURES ||
                witnessTx->getFieldH256(sfTransactionHash) != origin)
                continue;

            BEAST_EXPECT(!witnessTx->isFieldPresent(sfSigners));
            BEAST_EXPECT(
                witnessTx->getFieldVL(sfExportContributors) == Blob{0x01});
            auto const& base =
                witnessTx->peekAtField(sfExportedTxn).downcast<STObject>();
            auto const positioned =
                ExportResultBuilder::signaturesFromWitness(*witnessTx);
            BEAST_EXPECT(positioned);
            if (!positioned)
                continue;
            ExportResultBuilder::SignatureSnapshot signatures;
            for (auto const& [_, signature] : *positioned)
                signatures.emplace(signature.signingKey, signature.signature);
            auto const assembled =
                ExportResultBuilder::buildMultiSignedExportedTxn(
                    makeSTTx(base), signatures);
            Serializer s;
            assembled.add(s);
            multisignedBlob = s.peekData();

            log << "Xahau: Export witness = " << witnessTx->getTransactionID()
                << std::endl;
            log << "Xahau: assembled ExportedTxn = "
                << assembled.getJson(JsonOptions::none).toStyledString()
                << std::endl;
        }
        log << "Xahau: multisigned blob size = " << multisignedBlob.size()
            << std::endl;
        BEAST_EXPECT(!multisignedBlob.empty());

        auto const releasedLatch = xahau.current()->read(latchKey);
        BEAST_EXPECT(releasedLatch);
        if (releasedLatch)
            BEAST_EXPECT(releasedLatch->isFieldPresent(sfExportSignatureHash));

        Env xrpl{*this, xpopCtx.makeEnvConfig(targetNetworkID)};

        xrpl.fund(XRP(10000), alice, carol);
        xrpl.close();

        std::uint32_t const xrplTicketSeq = xrpl.seq(alice) + 1;
        xrpl(ticket::create(alice, 1));
        xrpl.close();
        BEAST_EXPECT(xrplTicketSeq == ticketSeq);

        auto const valPK = xahau.app().getValidationPublicKey();
        auto const valSK = xahau.app().getValidationSecretKey();
        BEAST_EXPECT(valPK.has_value());

        auto const valAccountID = calcAccountID(*valPK);
        log << "XRPL: validator AccountID = " << toBase58(valAccountID)
            << std::endl;
        log << "XRPL: validator PK = " << strHex(*valPK) << std::endl;

        {
            Json::Value jvSigners;
            jvSigners[jss::TransactionType] = jss::SignerListSet;
            jvSigners[jss::Account] = alice.human();
            jvSigners[sfSignerQuorum.jsonName] = 1;

            Json::Value signerEntry(Json::objectValue);
            signerEntry[sfAccount.jsonName] = toBase58(valAccountID);
            signerEntry[sfSignerWeight.jsonName] = 1;

            Json::Value entryWrapper(Json::objectValue);
            entryWrapper[sfSignerEntry.jsonName] = signerEntry;

            Json::Value entries(Json::arrayValue);
            entries.append(entryWrapper);
            jvSigners[sfSignerEntries.jsonName] = entries;

            log << "XRPL: SignerListSet JSON = " << jvSigners.toStyledString()
                << std::endl;

            xrpl(jvSigners, fee(XRP(1)), ter(tesSUCCESS));
            log << "XRPL: SignerListSet result = " << transToken(xrpl.ter())
                << std::endl;
            xrpl.close();
        }

        {
            STObject noop(sfGeneric);
            noop.setFieldU16(sfTransactionType, ttACCOUNT_SET);
            noop.setFieldU32(sfFlags, tfFullyCanonicalSig);
            noop.setFieldU32(sfSequence, xrpl.seq(alice));
            noop.setFieldU32(sfNetworkID, targetNetworkID);
            noop.setFieldAmount(sfFee, XRPAmount{20});
            noop.setFieldVL(sfSigningPubKey, Blob{});
            noop.setAccountID(sfAccount, alice.id());

            auto const sigData = buildMultiSigningData(noop, valAccountID);
            auto const sig = ripple::sign(*valPK, valSK, sigData.slice());

            STArray signers(sfSigners);
            STObject signer(sfSigner);
            signer.setAccountID(sfAccount, valAccountID);
            signer.setFieldVL(sfSigningPubKey, valPK->slice());
            signer.setFieldVL(sfTxnSignature, sig);
            signers.push_back(std::move(signer));

            noop.setFieldArray(sfSigners, signers);

            Serializer ser;
            noop.add(ser);
            auto const jr = xrpl.rpc("submit", strHex(ser.slice()));
            log << "XRPL: smoke test noop result = "
                << jr[jss::result][jss::engine_result].asString() << std::endl;
            xrpl.close();
        }

        {
            auto const jr =
                xrpl.rpc("submit", strHex(makeSlice(multisignedBlob)));
            log << "XRPL: export blob submit result = "
                << jr[jss::result][jss::engine_result].asString() << std::endl;
            if (jr[jss::result][jss::engine_result].asString() != "tesSUCCESS")
            {
                log << "XRPL: full result = " << jr.toStyledString()
                    << std::endl;
            }
        }
        xrpl.close();

        auto const xrplLcl = xrpl.app().getLedgerMaster().getClosedLedger();
        BEAST_EXPECT(xrplLcl);

        uint256 xrplMapKey;
        xrplLcl->txMap().visitLeaves(
            [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
                xrplMapKey = item->key();
            });

        auto const xpopJson = xpopCtx.buildXPOP(*xrplLcl, xrplMapKey);
        log << "XPOP null? " << xpopJson.isNull() << std::endl;
        BEAST_EXPECT(!xpopJson.isNull());

        auto const nullJournal = beast::Journal{beast::Journal::getNullSink()};
        auto const vlInfo = getVLInfo(xpopJson, nullJournal);
        BEAST_EXPECT(vlInfo);

        return CallbackXPOP{xpopJson, ticketSeq, origin, vlInfo};
    }

    void
    ratchetImportVLWithBurnToMint(
        jtx::Env& dstEnv,
        jtx::xpop::TestXPOPContext const& xpopCtx,
        std::uint32_t dstNetworkID,
        jtx::Account const& account,
        std::uint32_t vlSequence)
    {
        using namespace jtx;

        auto ratchetCtx = xpopCtx;
        ratchetCtx.vlData =
            ratchetCtx.publisher.buildVLData(ratchetCtx.validators, vlSequence);

        Env srcEnv{*this};
        Account const sink{"ratchetSink"};

        srcEnv.fund(XRP(10000), account, sink);
        srcEnv.close();

        Json::Value payTx;
        payTx[jss::TransactionType] = jss::Payment;
        payTx[jss::Account] = account.human();
        payTx[jss::Destination] = sink.human();
        payTx[jss::Amount] = "100000000";
        payTx[sfOperationLimit.jsonName] = dstNetworkID;
        srcEnv(payTx, fee(XRP(1)), ter(tesSUCCESS));
        srcEnv.close();

        auto const srcLcl = srcEnv.app().getLedgerMaster().getClosedLedger();
        BEAST_EXPECT(srcLcl);

        uint256 paymentHash;
        srcLcl->txMap().visitLeaves(
            [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
                paymentHash = item->key();
            });

        auto const xpopJson = ratchetCtx.buildXPOP(*srcLcl, paymentHash);
        BEAST_EXPECT(!xpopJson.isNull());

        dstEnv.fund(XRP(1000), account);
        dstEnv.close();

        auto const feeDrops = dstEnv.current()->fees().base;
        dstEnv(
            import::import(account, xpopJson),
            fee(feeDrops * 10),
            ter(tesSUCCESS));
        dstEnv.close();
    }

    void
    seedUNLReportLedger(jtx::Env& env, std::vector<PublicKey> const& activeKeys)
    {
        BEAST_EXPECT(!activeKeys.empty());

        env.app().openLedger().modify(
            [&](OpenView& view, beast::Journal) -> bool {
                for (auto const& pk : activeKeys)
                {
                    STTx tx(ttUNL_REPORT, [&](auto& obj) {
                        obj.setFieldU32(sfLedgerSequence, env.current()->seq());
                        auto active =
                            std::make_unique<STObject>(sfActiveValidator);
                        active->setFieldVL(sfPublicKey, pk);
                        obj.set(std::move(active));
                    });
                    auto txID = tx.getTransactionID();
                    auto s = std::make_shared<Serializer>();
                    tx.add(*s);
                    env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                    view.rawTxInsert(txID, std::move(s), nullptr);
                }
                return true;
            });

        BEAST_EXPECT(env.close(
            env.now() + std::chrono::seconds{5}, std::chrono::milliseconds{0}));
        BEAST_EXPECT(env.le(keylet::UNLReport()));
    }

    static jtx::JTx
    makeExportJTx(
        jtx::Env& env,
        jtx::Account const& account,
        STObject const& innerObj,
        LedgerIndex lls,
        Blob committee = {})
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = account.human();
        jv[jss::LastLedgerSequence] = lls;
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);
        bindExportAuthority(env, jv, std::move(committee));
        return env.jt(jv, jtx::fee(jtx::XRP(1)), jtx::ter(tesSUCCESS));
    }

    static Blob
    defaultExportCommittee(jtx::Env& env)
    {
        auto const& valKeys = env.app().getValidatorKeys();
        if (!valKeys.keys)
            return {};
        return serializeExportCommittee({valKeys.keys->masterPublicKey});
    }

    static void
    bindExportAuthority(jtx::Env& env, Json::Value& jv, Blob committee = {})
    {
        if (committee.empty())
            committee = defaultExportCommittee(env);
        auto const canonical =
            canonicalizeExportCommittee(makeSlice(committee));
        jv[sfExportCommitteeHash.jsonName] = to_string(
            canonical ? exportCommitteeHash(makeSlice(*canonical)) : uint256{});
        jv[sfExportCommittee.jsonName] = strHex(committee);
    }

    static uint256
    createExportCommittee(
        jtx::Env& env,
        jtx::Account const& account,
        Blob committee = {})
    {
        if (committee.empty())
            committee = defaultExportCommittee(env);

        Json::Value setup;
        setup[jss::TransactionType] = jss::Export;
        setup[jss::Account] = account.human();
        setup[sfExportCommittee.jsonName] = strHex(committee);
        env(setup, jtx::fee(jtx::XRP(1)), jtx::ter(tesSUCCESS));
        env.close();
        return exportCommitteeHash(makeSlice(committee));
    }

    // Build a minimal unsigned Payment STObject suitable for sfExportedTxn.
    static STObject
    buildExportedPayment(
        AccountID const& src,
        AccountID const& dst,
        std::uint32_t fls,
        std::uint32_t lls,
        std::optional<std::uint32_t> ticketSeq = 1)
    {
        STObject obj(sfExportedTxn);
        obj.setFieldU16(sfTransactionType, ttPAYMENT);
        obj.setFieldU32(sfFlags, tfFullyCanonicalSig);
        obj.setFieldU32(sfSequence, 0);
        if (ticketSeq)
            obj.setFieldU32(sfTicketSequence, *ticketSeq);
        obj.setFieldU32(sfFirstLedgerSequence, fls);
        obj.setFieldU32(sfLastLedgerSequence, lls);
        obj.setFieldAmount(sfAmount, XRPAmount{1000000});
        obj.setFieldAmount(sfFee, XRPAmount{10});
        obj.setFieldVL(sfSigningPubKey, Blob{});
        obj.setAccountID(sfAccount, src);
        obj.setAccountID(sfDestination, dst);
        return obj;
    }

    // Hook that exports a payment using xport (for cross-chain export)
    TestHook xport_wasm = export_test_wasm[R"[test.hook](
        #include <stdint.h>
        extern int32_t _g(uint32_t id, uint32_t maxiter);
        extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
        extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
        extern int64_t xport(uint32_t write_ptr, uint32_t write_len, uint32_t read_ptr, uint32_t read_len, uint32_t committee_hash_ptr, uint32_t committee_hash_len);
        extern int64_t xport_reserve(uint32_t count);
        extern int64_t hook_account(uint32_t write_ptr, uint32_t write_len);
        extern int64_t otxn_param(uint32_t write_ptr, uint32_t write_len, uint32_t name_ptr, uint32_t name_len);
        extern int64_t otxn_type(void);
        extern int64_t ledger_seq(void);

        #define SBUF(x) (uint32_t)(x), sizeof(x)
        #define ASSERT(x) if (!(x)) rollback((uint32_t)#x, sizeof(#x), __LINE__)

        #define ttPAYMENT 0
        #define tfCANONICAL 0x80000000UL

        #define amAMOUNT 1
        #define amFEE 8
        #define atACCOUNT 1
        #define atDESTINATION 3

        #define ENCODE_TT(buf_out, tt) \
            buf_out[0] = 0x12U; \
            buf_out[1] = (tt >> 8) & 0xFFU; \
            buf_out[2] = tt & 0xFFU; \
            buf_out += 3;

        #define ENCODE_FLAGS(buf_out, flags) \
            buf_out[0] = 0x22U; \
            buf_out[1] = (flags >> 24) & 0xFFU; \
            buf_out[2] = (flags >> 16) & 0xFFU; \
            buf_out[3] = (flags >> 8) & 0xFFU; \
            buf_out[4] = flags & 0xFFU; \
            buf_out += 5;

        #define ENCODE_SEQUENCE(buf_out, seq) \
            buf_out[0] = 0x24U; \
            buf_out[1] = (seq >> 24) & 0xFFU; \
            buf_out[2] = (seq >> 16) & 0xFFU; \
            buf_out[3] = (seq >> 8) & 0xFFU; \
            buf_out[4] = seq & 0xFFU; \
            buf_out += 5;

        #define ENCODE_FLS(buf_out, fls) \
            buf_out[0] = 0x20U; \
            buf_out[1] = 0x1AU; \
            buf_out[2] = (fls >> 24) & 0xFFU; \
            buf_out[3] = (fls >> 16) & 0xFFU; \
            buf_out[4] = (fls >> 8) & 0xFFU; \
            buf_out[5] = fls & 0xFFU; \
            buf_out += 6;

        #define ENCODE_LLS(buf_out, lls) \
            buf_out[0] = 0x20U; \
            buf_out[1] = 0x1BU; \
            buf_out[2] = (lls >> 24) & 0xFFU; \
            buf_out[3] = (lls >> 16) & 0xFFU; \
            buf_out[4] = (lls >> 8) & 0xFFU; \
            buf_out[5] = lls & 0xFFU; \
            buf_out += 6;

        #define ENCODE_DROPS(buf_out, drops, amt_type) \
            buf_out[0] = 0x60U + amt_type; \
            buf_out[1] = 0x40U + ((drops >> 56) & 0x3FU); \
            buf_out[2] = (drops >> 48) & 0xFFU; \
            buf_out[3] = (drops >> 40) & 0xFFU; \
            buf_out[4] = (drops >> 32) & 0xFFU; \
            buf_out[5] = (drops >> 24) & 0xFFU; \
            buf_out[6] = (drops >> 16) & 0xFFU; \
            buf_out[7] = (drops >> 8) & 0xFFU; \
            buf_out[8] = drops & 0xFFU; \
            buf_out += 9;

        #define ENCODE_SIGNING_PUBKEY_EMPTY(buf_out) \
            buf_out[0] = 0x73U; \
            buf_out[1] = 0x00U; \
            buf_out += 2;

        #define ENCODE_ACCOUNT(buf_out, acc, acc_type) \
            buf_out[0] = 0x80U + acc_type; \
            buf_out[1] = 0x14U; \
            for (int i = 0; i < 20; ++i) buf_out[2+i] = acc[i]; \
            buf_out += 22;

        #define PREPARE_PAYMENT_SIMPLE_SIZE 270U

        int64_t hook(uint32_t reserved) {
            _g(1, 1);

            if (otxn_type() != ttPAYMENT)
                return accept(0, 0, 0);

            ASSERT(xport_reserve(1) == 1);

            uint8_t dst[20];
            int64_t dst_len = otxn_param(SBUF(dst), "DST", 3);
            ASSERT(dst_len == 20);

            uint8_t acc[20];
            ASSERT(hook_account(SBUF(acc)) == 20);

            uint8_t committee[32];
            ASSERT(otxn_param(SBUF(committee), "COMMITTEE", 9) == 32);

            uint32_t cls = (uint32_t)ledger_seq();

            uint8_t tx[PREPARE_PAYMENT_SIMPLE_SIZE];
            uint8_t* buf = tx;

            ENCODE_TT(buf, ttPAYMENT);
            ENCODE_FLAGS(buf, tfCANONICAL);
            ENCODE_SEQUENCE(buf, 0);
            ENCODE_FLS(buf, cls + 1);
            ENCODE_LLS(buf, cls + 5);
            // sfTicketSequence = UINT32 field 41 = 0x20 0x29
            buf[0] = 0x20U; buf[1] = 0x29U;
            buf[2] = 0; buf[3] = 0; buf[4] = 0; buf[5] = 1;
            buf += 6;

            uint64_t drops = 1000000;
            ENCODE_DROPS(buf, drops, amAMOUNT);
            ENCODE_DROPS(buf, 10, amFEE);

            ENCODE_SIGNING_PUBKEY_EMPTY(buf);
            ENCODE_ACCOUNT(buf, acc, atACCOUNT);
            ENCODE_ACCOUNT(buf, dst, atDESTINATION);

            uint8_t hash[32];
            int64_t xport_result = xport(
                SBUF(hash), (uint32_t)tx, buf - tx, SBUF(committee));
            ASSERT(xport_result == 32);

            return accept(0, 0, 0);
        }
    )[test.hook]"];

    // Hook that exports a payment WITH sfNetworkID matching local network.
    // Should be rejected by the NetworkID self-target guard.
    TestHook xport_self_target_wasm = export_test_wasm[R"[test.hook](
        #include <stdint.h>
        extern int32_t _g(uint32_t id, uint32_t maxiter);
        extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
        extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
        extern int64_t xport(uint32_t write_ptr, uint32_t write_len, uint32_t read_ptr, uint32_t read_len, uint32_t committee_hash_ptr, uint32_t committee_hash_len);
        extern int64_t xport_reserve(uint32_t count);
        extern int64_t hook_account(uint32_t write_ptr, uint32_t write_len);
        extern int64_t otxn_param(uint32_t write_ptr, uint32_t write_len, uint32_t name_ptr, uint32_t name_len);
        extern int64_t otxn_type(void);
        extern int64_t ledger_seq(void);

        #define SBUF(x) (uint32_t)(x), sizeof(x)
        #define ASSERT(x) if (!(x)) rollback((uint32_t)#x, sizeof(#x), __LINE__)

        #define ttPAYMENT 0
        #define tfCANONICAL 0x80000000UL

        #define amAMOUNT 1
        #define amFEE 8
        #define atACCOUNT 1
        #define atDESTINATION 3

        #define ENCODE_TT(buf_out, tt) \
            buf_out[0] = 0x12U; \
            buf_out[1] = (tt >> 8) & 0xFFU; \
            buf_out[2] = tt & 0xFFU; \
            buf_out += 3;

        #define ENCODE_FLAGS(buf_out, flags) \
            buf_out[0] = 0x22U; \
            buf_out[1] = (flags >> 24) & 0xFFU; \
            buf_out[2] = (flags >> 16) & 0xFFU; \
            buf_out[3] = (flags >> 8) & 0xFFU; \
            buf_out[4] = flags & 0xFFU; \
            buf_out += 5;

        #define ENCODE_SEQUENCE(buf_out, seq) \
            buf_out[0] = 0x24U; \
            buf_out[1] = (seq >> 24) & 0xFFU; \
            buf_out[2] = (seq >> 16) & 0xFFU; \
            buf_out[3] = (seq >> 8) & 0xFFU; \
            buf_out[4] = seq & 0xFFU; \
            buf_out += 5;

        // sfNetworkID = UINT32 field 1 = 0x21
        #define ENCODE_NETWORK_ID(buf_out, id) \
            buf_out[0] = 0x21U; \
            buf_out[1] = (id >> 24) & 0xFFU; \
            buf_out[2] = (id >> 16) & 0xFFU; \
            buf_out[3] = (id >> 8) & 0xFFU; \
            buf_out[4] = id & 0xFFU; \
            buf_out += 5;

        #define ENCODE_FLS(buf_out, fls) \
            buf_out[0] = 0x20U; \
            buf_out[1] = 0x1AU; \
            buf_out[2] = (fls >> 24) & 0xFFU; \
            buf_out[3] = (fls >> 16) & 0xFFU; \
            buf_out[4] = (fls >> 8) & 0xFFU; \
            buf_out[5] = fls & 0xFFU; \
            buf_out += 6;

        #define ENCODE_LLS(buf_out, lls) \
            buf_out[0] = 0x20U; \
            buf_out[1] = 0x1BU; \
            buf_out[2] = (lls >> 24) & 0xFFU; \
            buf_out[3] = (lls >> 16) & 0xFFU; \
            buf_out[4] = (lls >> 8) & 0xFFU; \
            buf_out[5] = lls & 0xFFU; \
            buf_out += 6;

        #define ENCODE_DROPS(buf_out, drops, amt_type) \
            buf_out[0] = 0x60U + amt_type; \
            buf_out[1] = 0x40U + ((drops >> 56) & 0x3FU); \
            buf_out[2] = (drops >> 48) & 0xFFU; \
            buf_out[3] = (drops >> 40) & 0xFFU; \
            buf_out[4] = (drops >> 32) & 0xFFU; \
            buf_out[5] = (drops >> 24) & 0xFFU; \
            buf_out[6] = (drops >> 16) & 0xFFU; \
            buf_out[7] = (drops >> 8) & 0xFFU; \
            buf_out[8] = drops & 0xFFU; \
            buf_out += 9;

        #define ENCODE_SIGNING_PUBKEY_EMPTY(buf_out) \
            buf_out[0] = 0x73U; \
            buf_out[1] = 0x00U; \
            buf_out += 2;

        #define ENCODE_ACCOUNT(buf_out, acc, acc_type) \
            buf_out[0] = 0x80U + acc_type; \
            buf_out[1] = 0x14U; \
            for (int i = 0; i < 20; ++i) buf_out[2+i] = acc[i]; \
            buf_out += 22;

        #define PREPARE_PAYMENT_SIMPLE_SIZE 270U

        int64_t hook(uint32_t reserved) {
            _g(1, 1);

            if (otxn_type() != ttPAYMENT)
                return accept(0, 0, 0);

            ASSERT(xport_reserve(1) == 1);

            uint8_t dst[20];
            int64_t dst_len = otxn_param(SBUF(dst), "DST", 3);
            ASSERT(dst_len == 20);

            uint8_t acc[20];
            ASSERT(hook_account(SBUF(acc)) == 20);

            uint8_t committee[32];
            ASSERT(otxn_param(SBUF(committee), "COMMITTEE", 9) == 32);

            uint32_t cls = (uint32_t)ledger_seq();

            uint8_t tx[PREPARE_PAYMENT_SIMPLE_SIZE];
            uint8_t* buf = tx;

            ENCODE_TT(buf, ttPAYMENT);
            ENCODE_NETWORK_ID(buf, 21337);  // must precede Sequence (canonical order)
            ENCODE_FLAGS(buf, tfCANONICAL);
            ENCODE_SEQUENCE(buf, 0);
            ENCODE_FLS(buf, cls + 1);
            ENCODE_LLS(buf, cls + 5);

            uint64_t drops = 1000000;
            ENCODE_DROPS(buf, drops, amAMOUNT);
            ENCODE_DROPS(buf, 10, amFEE);

            ENCODE_SIGNING_PUBKEY_EMPTY(buf);
            ENCODE_ACCOUNT(buf, acc, atACCOUNT);
            ENCODE_ACCOUNT(buf, dst, atDESTINATION);

            uint8_t hash[32];
            int64_t xport_result = xport(
                SBUF(hash), (uint32_t)tx, buf - tx, SBUF(committee));
            // xport should return EXPORT_FAILURE (-46), ASSERT will rollback
            ASSERT(xport_result == 32);

            return accept(0, 0, 0);
        }
    )[test.hook]"];

    // Helper to build hook params with DST
    static Json::Value
    makeDstParams(AccountID const& dst, uint256 const& committeeHash = {})
    {
        Json::Value params(Json::arrayValue);
        Json::Value param;
        param[jss::HookParameter] = Json::Value(Json::objectValue);
        param[jss::HookParameter][jss::HookParameterName] =
            strHex(std::string("DST"));
        param[jss::HookParameter][jss::HookParameterValue] = strHex(dst);
        params.append(param);
        if (!committeeHash.isZero())
        {
            Json::Value committeeParam;
            committeeParam[jss::HookParameter] = Json::Value(Json::objectValue);
            committeeParam[jss::HookParameter][jss::HookParameterName] =
                strHex(std::string("COMMITTEE"));
            committeeParam[jss::HookParameter][jss::HookParameterValue] =
                strHex(committeeHash);
            params.append(committeeParam);
        }
        return params;
    }

    void
    testXportPayment(FeatureBitset features)
    {
        testcase("Xport Payment (hook emits ttEXPORT)");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, bob, carol);
        env.close();
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;
        seedUNLReportLedger(env, {valKeys.keys->masterPublicKey});
        auto const committeeHash = createExportCommittee(env, alice);

        // Install xport hook on alice
        env(ripple::test::jtx::hook(alice, {{hso(xport_wasm)}}, 0),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        // Trigger hook with payment containing DST parameter
        auto params = makeDstParams(carol.id(), committeeHash);

        env(pay(bob, alice, XRP(100)),
            fee(XRP(1)),
            json(jss::HookParameters, params),
            ter(tesSUCCESS));

        // Verify hook fired successfully with exactly 1 export.
        {
            auto const m = env.meta();
            BEAST_EXPECT(m);
            BEAST_EXPECT(m->isFieldPresent(sfHookExecutions));

            auto const& execs = m->getFieldArray(sfHookExecutions);
            BEAST_EXPECT(execs.size() == 1);

            // result=3 is ExitType::ACCEPT
            BEAST_EXPECT(execs[0].getFieldU8(sfHookResult) == 3);
            BEAST_EXPECT(execs[0].getFieldU16(sfHookEmitCount) == 0);
            BEAST_EXPECT(execs[0].getFieldU16(sfHookExportCount) == 1);
            BEAST_EXPECT(execs[0].getFieldU64(sfHookReturnCode) == 0);

            // HookEmissions tracks normal emitted transactions. Export wrappers
            // use the emitted directory for scheduling but are counted
            // separately as HookExportCount.
            BEAST_EXPECT(!m->isFieldPresent(sfHookEmissions));

            // The emitted dir should NOT be empty (ttEXPORT is in it).
            BEAST_EXPECT(!dirIsEmpty(*env.current(), keylet::emittedDir()));

            // Find the emitted ttEXPORT in AffectedNodes.
            bool foundEmitted = false;
            for (auto const& node : m->getFieldArray(sfAffectedNodes))
            {
                if (node.getFName() == sfCreatedNode &&
                    node.getFieldU16(sfLedgerEntryType) == ltEMITTED_TXN)
                {
                    foundEmitted = true;
                    break;
                }
            }
            BEAST_EXPECT(foundEmitted);
        }

        // env.meta() above did an implicit close (hook fires, emitted dir
        // populated, TxQ rawTxInserts into open ledger).  One more close
        // applies the emitted ttEXPORT through the transactor.
        env.close();

        std::optional<uint256> origin;
        for (auto const& [stx, _] : env.closed()->txs)
        {
            if (stx->getTxnType() == ttEXPORT)
            {
                BEAST_EXPECT(stx->isFieldPresent(sfEmitDetails));
                origin = stx->getTransactionID();
            }
            BEAST_EXPECT(stx->getTxnType() != ttEXPORT_SIGNATURES);
        }
        BEAST_EXPECT(origin);
        if (!origin)
            return;

        auto const latchKey = keylet::exportLatch(alice.id(), *origin);
        auto const pendingLatch = env.closed()->read(latchKey);
        BEAST_EXPECT(pendingLatch);
        if (pendingLatch)
            BEAST_EXPECT(!pendingLatch->isFieldPresent(sfExportSignatureHash));

        // Validation of the origin ledger releases signatures. The assembled
        // witness is therefore materialized in a subsequent ledger.
        env.app().getJobQueue().rendezvous();
        auto const releasedShares = env.app()
                                        .getConsensusExtensions()
                                        .postValidationExportSigCollector()
                                        .fullUnionSnapshot();
        BEAST_EXPECT(releasedShares.contains(*origin));
        if (auto const it = releasedShares.find(*origin);
            it != releasedShares.end())
            BEAST_EXPECT(it->second.size() == 1);
        std::shared_ptr<STTx const> witness;
        for (std::size_t attempt = 0;
             attempt < ExportLimits::maxAdmissionWindowLedgers && !witness;
             ++attempt)
        {
            env.close();
            for (auto const& [stx, _] : env.closed()->txs)
            {
                if (stx->getTxnType() == ttEXPORT_SIGNATURES &&
                    stx->getFieldH256(sfTransactionHash) == *origin)
                    witness = stx;
            }
        }
        BEAST_EXPECT(witness);
        if (!witness)
            return;
        BEAST_EXPECT(!witness->isFieldPresent(sfSigners));
        BEAST_EXPECT(witness->getFieldVL(sfExportContributors) == Blob{0x01});
        auto const& assembled =
            witness->peekAtField(sfExportedTxn).downcast<STObject>();
        BEAST_EXPECT(!assembled.isFieldPresent(sfSigners));
        BEAST_EXPECT(witness->getFieldArray(sfExportSigners).size() == 1);

        auto const releasedLatch = env.closed()->read(latchKey);
        BEAST_EXPECT(releasedLatch);
        if (releasedLatch)
            BEAST_EXPECT(
                releasedLatch->getFieldH256(sfExportSignatureHash) ==
                witness->getTransactionID());

        // Witnessed latches still resolve their immutable committee during
        // replay. The committee therefore remains undeletable until the latch
        // itself reaches terminal cleanup.
        Json::Value eraseCommittee;
        eraseCommittee[jss::TransactionType] = jss::Export;
        eraseCommittee[jss::Account] = alice.human();
        eraseCommittee[jss::Flags] = tfExportEraseCommittee;
        eraseCommittee[sfExportCommitteeHash.jsonName] =
            to_string(committeeHash);
        env(eraseCommittee, fee(XRP(1)), ter(tecHAS_OBLIGATIONS));
    }

    void
    testXportRejectsLocalNetworkID(FeatureBitset features)
    {
        testcase("Xport rejects export targeting local NetworkID");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, bob, carol);
        env.close();

        // Install hook that exports a tx with NetworkID=21337
        env(ripple::test::jtx::hook(alice, {{hso(xport_self_target_wasm)}}, 0),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        // Trigger: xport() should reject because exported tx's NetworkID
        // matches the local network → EXPORT_FAILURE → hook rollback
        auto params = makeDstParams(carol.id(), uint256{1});

        env(pay(bob, alice, XRP(100)),
            fee(XRP(1)),
            json(jss::HookParameters, params),
            ter(tecHOOK_REJECTED));
        env.close();

        // Verify no emitted ttEXPORT was created
        auto const emittedDirKey = keylet::emittedDir();
        BEAST_EXPECT(dirIsEmpty(*env.current(), emittedDirKey));
    }

    void
    testXportRejectsUnconfiguredNetworkID(FeatureBitset features)
    {
        testcase("Xport rejects export when NETWORK_ID is unconfigured");

        using namespace jtx;

        // Default NETWORK_ID=0
        Env env{*this, envconfig(jtx::validator, ""), features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, bob, carol);
        env.close();

        // Install the normal xport hook (no NetworkID in exported tx)
        env(ripple::test::jtx::hook(alice, {{hso(xport_wasm)}}, 0),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        // Trigger: xport() should reject because NETWORK_ID=0 and the
        // exported tx has no sfNetworkID → can't verify it's cross-chain
        auto params = makeDstParams(carol.id(), uint256{1});

        env(pay(bob, alice, XRP(100)),
            fee(XRP(1)),
            json(jss::HookParameters, params),
            ter(tecHOOK_REJECTED));
        env.close();

        // Verify no emitted ttEXPORT was created
        auto const emittedDirKey = keylet::emittedDir();
        BEAST_EXPECT(dirIsEmpty(*env.current(), emittedDirKey));
    }

    void
    testExportRejectsAmbiguousAbsentNetworkID()
    {
        testcase("Export rejects ambiguous absent target NetworkID");

        using namespace jtx;

        Account const alice{"alice"};
        Account const carol{"carol"};
        auto innerObj = buildExportedPayment(alice.id(), carol.id(), 2, 6);
        auto const innerTx = makeSTTx(innerObj);
        auto const j = beast::Journal{beast::Journal::getNullSink()};

        // A source chain that also omits sfNetworkID cannot distinguish an
        // absent target ID from self-target.
        BEAST_EXPECT(
            ExportLedgerOps::validateNetworkID(innerTx, 0, j) == temMALFORMED);
        BEAST_EXPECT(
            ExportLedgerOps::validateNetworkID(
                innerTx, maxNetworkIDWithoutTxField, j) == temMALFORMED);

        // A source chain that requires sfNetworkID for itself can export a
        // target transaction using the no-NetworkID/XRPL-mainnet encoding.
        BEAST_EXPECT(
            ExportLedgerOps::validateNetworkID(
                innerTx, maxNetworkIDWithoutTxField + 1, j) == tesSUCCESS);
    }

    void
    testExportRejectsInvalidOriginMemoProjection()
    {
        testcase("Export validates future origin Memo projection");

        jtx::Account const alice{"alice"};
        jtx::Account const carol{"carol"};
        auto const j = beast::Journal{beast::Journal::getNullSink()};
        auto const sourceNetworkID = maxNetworkIDWithoutTxField + 1;

        auto innerObj = buildExportedPayment(alice.id(), carol.id(), 2, 6);
        BEAST_EXPECT(
            ExportLedgerOps::validateOriginMemoProjection(
                makeSTTx(innerObj), sourceNetworkID, j) == tesSUCCESS);

        STArray memos{sfMemos};
        STObject reservedMemo{sfMemo};
        reservedMemo.setFieldVL(
            sfMemoType,
            Blob(
                ExportOriginMemo::memoType.begin(),
                ExportOriginMemo::memoType.end()));
        reservedMemo.setFieldVL(
            sfMemoData, Blob(ExportOriginMemo::identityBytes));
        memos.emplace_back(std::move(reservedMemo));
        innerObj.setFieldArray(sfMemos, memos);
        BEAST_EXPECT(
            ExportLedgerOps::validateOriginMemoProjection(
                makeSTTx(innerObj), sourceNetworkID, j) == temMALFORMED);

        memos.clear();
        STObject fullMemo{sfMemo};
        fullMemo.setFieldVL(sfMemoType, Blob{'u', 's', 'e', 'r'});
        fullMemo.setFieldVL(sfMemoData, Blob(930, 0x5A));
        memos.emplace_back(std::move(fullMemo));
        innerObj.setFieldArray(sfMemos, memos);
        std::string reason;
        BEAST_EXPECT(passesLocalChecks(makeSTTx(innerObj), reason));
        BEAST_EXPECT(
            ExportLedgerOps::validateOriginMemoProjection(
                makeSTTx(innerObj), sourceNetworkID, j) == temMALFORMED);
    }

    void
    testXportEmissionLimit(FeatureBitset features)
    {
        testcase("Xport emitted export limit");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, bob, carol);
        env.close();

        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;
        seedUNLReportLedger(env, {valKeys.keys->masterPublicKey});
        auto const committeeHash = createExportCommittee(env, alice);

        env(ripple::test::jtx::hook(alice, {{hso(xport_wasm)}}, 0),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        auto params = makeDstParams(carol.id(), committeeHash);
        for (std::uint32_t i = 0; i <= ExportLimits::maxPendingExports; ++i)
        {
            env(pay(bob, alice, XRP(1)),
                fee(XRP(1)),
                json(jss::HookParameters, params),
                ter(tesSUCCESS));
        }

        env.close();

        std::uint32_t accepted = 0;
        std::uint32_t capped = 0;
        for (auto const& [stx, meta] : env.closed()->txs)
        {
            if (stx->getTxnType() != ttPAYMENT ||
                stx->getAccountID(sfAccount) != bob.id())
            {
                continue;
            }

            if ((*meta)[sfTransactionResult] ==
                static_cast<std::uint8_t>(TERtoInt(tesSUCCESS)))
                ++accepted;
            else if (
                (*meta)[sfTransactionResult] ==
                static_cast<std::uint8_t>(TERtoInt(tecDIR_FULL)))
                ++capped;
        }

        BEAST_EXPECT(accepted == ExportLimits::maxPendingExports);
        BEAST_EXPECT(capped == 1);
        BEAST_EXPECT(
            ExportLedgerOps::pendingExportEmissionCount(*env.current()) ==
            ExportLimits::maxPendingExports);
    }

    void
    testExportTxnOpenLedger(FeatureBitset features)
    {
        testcase("ttEXPORT succeeds on open ledger (provisional)");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;
        seedUNLReportLedger(env, {valKeys.keys->masterPublicKey});

        auto const seq = env.current()->seq();
        auto innerObj = buildExportedPayment(
            alice.id(),
            carol.id(),
            seq + 1,
            seq + ExportLimits::maxAdmissionWindowLedgers);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[jss::LastLedgerSequence] =
            seq + ExportLimits::maxAdmissionWindowLedgers;
        bindExportAuthority(env, jv);
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

        env(jv, fee(XRP(1)), ter(tesSUCCESS));
        env.close();
    }

    void
    testExportNetworkAdmitsIntentWithoutQuorum(FeatureBitset features)
    {
        testcase("ttEXPORT admits intent before signature quorum");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;
        seedUNLReportLedger(env, {valKeys.keys->masterPublicKey});
        forceNonStandalone(env.app());
        BEAST_EXPECT(!env.app().config().standalone());
        ConsensusTestConfig cfg;
        cfg.noExportSig = true;
        env.app().getRuntimeConfig().setGlobalConfig(cfg);

        auto const seq = env.current()->seq();
        auto const ticketSeq = std::uint32_t{1};
        auto const lls = seq + ExportLimits::maxAdmissionWindowLedgers;
        auto innerObj = buildExportedPayment(
            alice.id(), carol.id(), seq + 1, lls, ticketSeq);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[jss::LastLedgerSequence] = lls;
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);
        bindExportAuthority(env, jv);

        env(jv, fee(XRP(1)), ter(tesSUCCESS));
        auto const origin = env.tx()->getTransactionID();
        BEAST_EXPECT(env.close(
            env.now() + std::chrono::seconds{5}, std::chrono::milliseconds{0}));

        std::optional<TER> closedResult;
        for (auto const& [stx, meta] : env.closed()->txs)
        {
            if (!stx || stx->getTxnType() != ttEXPORT ||
                stx->getAccountID(sfAccount) != alice.id())
            {
                continue;
            }

            auto const& exported =
                stx->peekAtField(sfExportedTxn).downcast<STObject>();
            if (exported.getFieldU32(sfTicketSequence) != ticketSeq)
                continue;

            if (meta)
                closedResult = TER::fromInt((*meta)[sfTransactionResult]);
        }

        BEAST_EXPECT(closedResult == tesSUCCESS);
        auto const latch = env.le(keylet::exportLatch(alice.id(), origin));
        BEAST_EXPECT(latch);
        if (latch)
            BEAST_EXPECT(!latch->isFieldPresent(sfExportSignatureHash));
    }

    void
    testExportLatchInsufficientReserve(FeatureBitset features)
    {
        testcase("ttEXPORT Export latch requires owner reserve");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        {
            uint256 const nftId0{token::getNextID(env, alice, 0u)};
            env(token::mint(alice, 0u));
            env(token::burn(alice, nftId0));
        }
        env.close();

        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        seedUNLReportLedger(env, {valKeys.keys->masterPublicKey});
        forceNonStandalone(env.app());

        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto const applySeq = parent->seq() + 1;
        auto const ticketSeq = std::uint32_t{1};
        auto innerObj = buildExportedPayment(
            alice.id(), carol.id(), applySeq, applySeq + 5, ticketSeq);
        auto jt = makeExportJTx(env, alice, innerObj, applySeq + 5);
        auto const exportTx = jt.stx;
        BEAST_EXPECT(exportTx);
        if (!exportTx)
            return;
        auto const txHash = exportTx->getTransactionID();

        auto next = std::make_shared<Ledger>(
            *parent, env.app().timeKeeper().closeTime());
        auto const account = next->read(keylet::account(alice.id()));
        BEAST_EXPECT(account);
        if (!account)
            return;

        auto replacement = std::make_shared<SLE>(*account, account->key());
        auto const insufficient = next->fees().accountReserve(
                                      account->getFieldU32(sfOwnerCount) + 1) -
            drops(1);
        replacement->setFieldAmount(sfBalance, insufficient);
        next->rawReplace(replacement);

        OpenView accum(&*next);
        auto const result =
            ripple::apply(env.app(), accum, *exportTx, tapNONE, env.journal);
        BEAST_EXPECT(result.ter == tecINSUFFICIENT_RESERVE);

        BEAST_EXPECT(!accum.read(keylet::exportLatch(alice.id(), txHash)));
    }

    void
    testLaterLedgerWitnessTransitionAndReplay(FeatureBitset features)
    {
        testcase("D-ATTRIB witness apply and rotated-key replay");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();

        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        auto const oldSigningKey = randomKeyPair(KeyType::secp256k1);
        auto const newSigningKey = randomKeyPair(KeyType::secp256k1);
        auto installManifest = [&](auto const& signingKey,
                                   std::uint32_t sequence) {
            auto manifest = deserializeManifest(xpop::makeManifestRaw(
                valKeys.keys->masterPublicKey,
                valKeys.keys->secretKey,
                signingKey.first,
                signingKey.second,
                sequence));
            BEAST_EXPECT(manifest);
            return manifest &&
                env.app().validatorManifests().applyManifest(
                    std::move(*manifest)) == ManifestDisposition::accepted;
        };

        auto const oldManifestInstalled = installManifest(oldSigningKey, 1);
        BEAST_EXPECT(oldManifestInstalled);
        if (!oldManifestInstalled)
            return;
        BEAST_EXPECT(
            env.app().validatorManifests().getMasterKey(oldSigningKey.first) ==
            valKeys.keys->masterPublicKey);

        std::vector<std::pair<PublicKey, SecretKey>> validatorMasters;
        validatorMasters.emplace_back(
            valKeys.keys->masterPublicKey, valKeys.keys->secretKey);
        for (std::size_t i = 1; i < 4; ++i)
            validatorMasters.push_back(randomKeyPair(KeyType::secp256k1));

        std::vector<PublicKey> activeMasterKeys;
        for (auto const& [masterPublic, _] : validatorMasters)
            activeMasterKeys.push_back(masterPublic);
        seedUNLReportLedger(env, activeMasterKeys);
        forceNonStandalone(env.app());
        BEAST_EXPECT(!env.app().config().standalone());

        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto const validatorView =
            env.app().getConsensusExtensions().makeActiveValidatorView(parent);
        BEAST_EXPECT(validatorView->fromUNLReport);
        BEAST_EXPECT(
            validatorView->orderedOriginalMasterKeys.size() ==
            validatorMasters.size());
        if (!validatorView->fromUNLReport ||
            validatorView->orderedOriginalMasterKeys.size() !=
                validatorMasters.size())
            return;

        std::optional<std::uint16_t> localPosition;
        for (std::size_t i = 0;
             i < validatorView->orderedOriginalMasterKeys.size();
             ++i)
        {
            if (validatorView->orderedOriginalMasterKeys[i] ==
                valKeys.keys->masterPublicKey)
                localPosition = static_cast<std::uint16_t>(i);
        }
        BEAST_EXPECT(localPosition);
        if (!localPosition)
            return;

        std::vector<std::uint16_t> selectedPositions{*localPosition};
        for (std::uint16_t i = 0;
             i < validatorView->orderedOriginalMasterKeys.size() &&
             selectedPositions.size() < 3;
             ++i)
        {
            if (i != *localPosition)
                selectedPositions.push_back(i);
        }
        std::vector<PublicKey> committeeMasters;
        for (auto const position : selectedPositions)
            committeeMasters.push_back(
                validatorView->orderedOriginalMasterKeys[position]);
        auto const canonicalRoster = serializeExportCommittee(committeeMasters);
        Blob committeeRoster;
        for (auto it = committeeMasters.rbegin(); it != committeeMasters.rend();
             ++it)
            committeeRoster.insert(
                committeeRoster.end(), it->begin(), it->end());
        auto const committee =
            resolveExportCommittee(makeSlice(canonicalRoster));
        BEAST_EXPECT(committee);
        if (!committee)
            return;
        std::vector<std::uint16_t> committeePositions;
        for (auto const& master : committee->members)
            committeePositions.push_back(*committee->position(master));
        std::vector<std::uint16_t> const expectedPositions{0, 1, 2};
        BEAST_EXPECT(committeePositions == expectedPositions);
        BEAST_EXPECT(ExportLimits::committeeQuorumThreshold(3) == 3);

        auto const countExportWork = [](std::shared_ptr<SLE const> const& sle) {
            return sle && sle->isFieldPresent(sfExportCount)
                ? sle->getFieldU16(sfExportCount)
                : std::uint16_t{0};
        };
        auto const parentPendingCount =
            countExportWork(parent->read(keylet::pendingExports()));
        auto const parentAccount = parent->read(keylet::account(alice.id()));
        BEAST_EXPECT(parentAccount);
        if (!parentAccount)
            return;
        auto const parentAccountExportCount = countExportWork(parentAccount);
        auto const parentOwnerCount = parentAccount->getFieldU32(sfOwnerCount);
        auto const originSeq = parent->seq() + 1;
        auto const ticketSeq = std::uint32_t{1};
        auto innerObj = buildExportedPayment(
            alice.id(), carol.id(), originSeq, originSeq + 5, ticketSeq);
        auto const innerTx = makeSTTx(innerObj);
        // The outer LastLedgerSequence expires at admission. The durable
        // publication window starts from the ledger that actually admits the
        // intent and therefore extends independently beyond that outer bound.
        auto jt =
            makeExportJTx(env, alice, innerObj, originSeq, committeeRoster);
        auto const exportTx = jt.stx;
        BEAST_EXPECT(exportTx);
        if (!exportTx)
            return;
        auto const origin = exportTx->getTransactionID();

        CanonicalTXSet originTxs{parent->info().hash};
        originTxs.insert(exportTx);
        std::set<TxID> originFailed;
        auto const originLedger = buildLedger(
            parent,
            env.app().timeKeeper().closeTime(),
            true,
            parent->info().closeTimeResolution,
            env.app(),
            originTxs,
            originFailed,
            env.journal);
        BEAST_EXPECT(originTxs.empty());
        BEAST_EXPECT(originFailed.empty());

        auto const latchKey = keylet::exportLatch(alice.id(), origin);
        auto const pendingLatch = originLedger->read(latchKey);
        BEAST_EXPECT(pendingLatch);
        if (!pendingLatch)
            return;
        BEAST_EXPECT(!pendingLatch->isFieldPresent(sfExportSignatureHash));
        BEAST_EXPECT(pendingLatch->isFieldPresent(sfExportNode));
        BEAST_EXPECT(!pendingLatch->isFieldPresent(sfExportCommittee));
        auto const committeeSLE = originLedger->read(keylet::exportCommittee(
            alice.id(), exportCommitteeHash(makeSlice(canonicalRoster))));
        BEAST_EXPECT(committeeSLE);
        if (committeeSLE)
            BEAST_EXPECT(
                committeeSLE->getFieldVL(sfExportCommittee) == canonicalRoster);
        BEAST_EXPECT(
            pendingLatch->getFieldU32(sfLastLedgerSequence) ==
            originLedger->seq() + ExportLimits::maxPublicationLedgers);
        auto const originPendingRoot =
            originLedger->read(keylet::pendingExports());
        auto const originAccount =
            originLedger->read(keylet::account(alice.id()));
        BEAST_EXPECT(originPendingRoot);
        BEAST_EXPECT(originAccount);
        if (!originPendingRoot || !originAccount)
            return;
        BEAST_EXPECT(
            countExportWork(originPendingRoot) == parentPendingCount + 1);
        BEAST_EXPECT(
            countExportWork(originAccount) == parentAccountExportCount + 1);
        BEAST_EXPECT(
            originAccount->getFieldU32(sfOwnerCount) == parentOwnerCount + 2);

        auto release = ExportOriginMemo::releaseForm(
            innerTx,
            ExportOriginMemo::Origin{env.app().config().NETWORK_ID, 0, origin},
            ExportOriginMemo::Anchor{
                originLedger->info().seq, originLedger->info().hash});
        BEAST_EXPECT(release);
        if (!release)
            return;

        auto signerAt = [&](std::uint16_t position)
            -> std::pair<PublicKey, SecretKey> const& {
            auto const& master = committee->members[position];
            if (master == valKeys.keys->masterPublicKey)
                return oldSigningKey;
            for (auto const& validator : validatorMasters)
                if (validator.first == master)
                    return validator;
            Throw<std::logic_error>("missing Export test validator key");
        };
        auto makeSignatures = [&](std::vector<std::uint16_t> const& positions,
                                  bool wrongMessage = false) {
            ExportResultBuilder::PositionedSignatureSnapshot result;
            for (auto const position : positions)
            {
                auto const& signer = signerAt(position);
                auto const& target =
                    wrongMessage && position == positions.front()
                    ? innerTx
                    : release.value();
                result.emplace(
                    position,
                    ExportResultBuilder::PositionedSignature{
                        signer.first,
                        ExportResultBuilder::signExportedTxn(
                            target, signer.first, signer.second)});
            }
            return result;
        };

        auto const signatures = makeSignatures(committeePositions);
        auto const witnessSeq = originLedger->seq() + 1;
        auto const validWitness = ExportResultBuilder::buildSignatureWitness(
            origin,
            release.value(),
            signatures,
            committee->members.size(),
            witnessSeq);

        BEAST_EXPECT(!validWitness.isFieldPresent(sfSigners));
        BEAST_EXPECT(
            validWitness.getFieldVL(sfExportContributors) == Blob{0x07});
        auto const& assembled =
            validWitness.peekAtField(sfExportedTxn).downcast<STObject>();
        BEAST_EXPECT(!assembled.isFieldPresent(sfSigners));
        BEAST_EXPECT(
            validWitness.getFieldArray(sfExportSigners).size() ==
            committeePositions.size());

        enum class WitnessFault {
            width,
            unusedBit,
            committeeSubset,
            popcount,
            quorum,
            wrongSignature
        };
        struct RejectedWitnessCase
        {
            char const* name;
            WitnessFault fault;
            TER expected;
        };
        RejectedWitnessCase const rejectedCases[]{
            {"bitmap width", WitnessFault::width, tefFAILURE},
            {"unused high bit", WitnessFault::unusedBit, tefFAILURE},
            {"non-committee contributor",
             WitnessFault::committeeSubset,
             tefFAILURE},
            {"bitmap/signer popcount", WitnessFault::popcount, temMALFORMED},
            {"below qC", WitnessFault::quorum, tefFAILURE},
            {"wrong cryptographic signature",
             WitnessFault::wrongSignature,
             tefFAILURE},
        };
        auto makeRejectedWitness = [&](WitnessFault fault) -> STTx {
            if (fault == WitnessFault::quorum)
            {
                auto subQuorum = committeePositions;
                subQuorum.pop_back();
                return ExportResultBuilder::buildSignatureWitness(
                    origin,
                    release.value(),
                    makeSignatures(subQuorum),
                    committee->members.size(),
                    witnessSeq);
            }
            if (fault == WitnessFault::wrongSignature)
                return ExportResultBuilder::buildSignatureWitness(
                    origin,
                    release.value(),
                    makeSignatures(committeePositions, true),
                    committee->members.size(),
                    witnessSeq);

            auto witness = validWitness;
            auto contributors = witness.getFieldVL(sfExportContributors);
            auto const removed = committeePositions.back();
            auto const removedBit =
                static_cast<std::uint8_t>(1u << (removed % 8));
            switch (fault)
            {
                case WitnessFault::width:
                    contributors.push_back(0);
                    break;
                case WitnessFault::unusedBit:
                    contributors[0] &= static_cast<std::uint8_t>(~removedBit);
                    contributors[0] |= 0x80u;
                    break;
                case WitnessFault::committeeSubset:
                    contributors[0] &= static_cast<std::uint8_t>(~removedBit);
                    contributors[0] |= 0x08u;
                    break;
                case WitnessFault::popcount:
                    contributors[0] &= static_cast<std::uint8_t>(~removedBit);
                    break;
                case WitnessFault::quorum:
                case WitnessFault::wrongSignature:
                    Throw<std::logic_error>("handled Export witness fault");
            }
            witness.setFieldVL(sfExportContributors, contributors);
            return makeSTTx(witness);
        };

        auto const pendingLatchBytes = pendingLatch->getSerializer().peekData();
        for (auto const& rejectedCase : rejectedCases)
        {
            log << "D-ATTRIB rejected witness: " << rejectedCase.name
                << std::endl;
            auto const rejected = makeRejectedWitness(rejectedCase.fault);
            auto next = std::make_shared<Ledger>(
                *originLedger, env.app().timeKeeper().closeTime());
            BEAST_EXPECT(next->seq() == witnessSeq);
            OpenView accum(&*next);
            auto const result = ripple::apply(
                env.app(),
                accum,
                rejected,
                tapNONE,
                env.journal,
                ApplyOptions{originLedger});
            BEAST_EXPECT(result.ter == rejectedCase.expected);
            BEAST_EXPECT(!result.applied);

            auto const after = accum.read(latchKey);
            BEAST_EXPECT(after);
            if (after)
            {
                BEAST_EXPECT(after->isFieldPresent(sfExportNode));
                BEAST_EXPECT(!after->isFieldPresent(sfExportSignatureHash));
                BEAST_EXPECT(
                    after->getSerializer().peekData() == pendingLatchBytes);
            }
        }

        auto applyWitnessAfterErase = [&](STTx const& witness, TER expected) {
            auto next = std::make_shared<Ledger>(
                *originLedger, env.app().timeKeeper().closeTime());
            OpenView accum(&*next);
            auto const existing = accum.read(latchKey);
            BEAST_EXPECT(existing);
            if (!existing)
                return;
            accum.rawErase(std::make_shared<SLE>(*existing));
            BEAST_EXPECT(!accum.read(latchKey));

            auto const result = ripple::apply(
                env.app(),
                accum,
                witness,
                tapNONE,
                env.journal,
                ApplyOptions{originLedger});
            BEAST_EXPECT(result.ter == expected);
            BEAST_EXPECT(result.applied == isTesSuccess(expected));
            BEAST_EXPECT(!accum.read(latchKey));
        };

        // An erase ordered before the witness removes only the state
        // transition. The witness must still validate against the immutable
        // parent; malformed evidence cannot become an evidence-only success.
        applyWitnessAfterErase(
            makeRejectedWitness(WitnessFault::wrongSignature), tefFAILURE);
        applyWitnessAfterErase(validWitness, tesSUCCESS);

        auto const witnessTx = std::make_shared<STTx const>(validWitness);

        CanonicalTXSet txns{originLedger->info().hash};
        txns.insert(witnessTx);
        std::set<TxID> failed;

        auto const built = buildLedger(
            originLedger,
            env.app().timeKeeper().closeTime(),
            true,
            originLedger->info().closeTimeResolution,
            env.app(),
            txns,
            failed,
            env.journal);
        BEAST_EXPECT(txns.empty());
        BEAST_EXPECT(failed.empty());

        auto const releasedLatch = built->read(latchKey);
        BEAST_EXPECT(releasedLatch);
        if (releasedLatch)
        {
            BEAST_EXPECT(
                releasedLatch->getFieldH256(sfExportSignatureHash) ==
                witnessTx->getTransactionID());
            BEAST_EXPECT(!releasedLatch->isFieldPresent(sfExportNode));
        }

        auto const releasedPendingRoot = built->read(keylet::pendingExports());
        auto const releasedAccount = built->read(keylet::account(alice.id()));
        BEAST_EXPECT(releasedPendingRoot);
        BEAST_EXPECT(releasedAccount);
        if (releasedPendingRoot && releasedAccount)
        {
            BEAST_EXPECT(
                countExportWork(releasedPendingRoot) == parentPendingCount);
            BEAST_EXPECT(
                countExportWork(releasedAccount) ==
                parentAccountExportCount + 1);
            BEAST_EXPECT(
                releasedAccount->getFieldU32(sfOwnerCount) ==
                parentOwnerCount + 2);
        }

        auto const persisted = built->txRead(witnessTx->getTransactionID());
        BEAST_EXPECT(persisted.first);
        auto const persistedSignatures = persisted.first
            ? ExportResultBuilder::signaturesFromWitness(*persisted.first)
            : std::nullopt;
        BEAST_EXPECT(persistedSignatures);
        bool persistedOldKey = false;
        if (persistedSignatures)
        {
            for (auto const& [_, signature] : *persistedSignatures)
                persistedOldKey |= signature.signingKey == oldSigningKey.first;
        }
        BEAST_EXPECT(persistedOldKey);

        // Rotation removes the old ephemeral-to-master binding. Historical
        // replay must therefore validate only the persisted witness and state.
        auto const newManifestInstalled = installManifest(newSigningKey, 2);
        BEAST_EXPECT(newManifestInstalled);
        if (!newManifestInstalled)
            return;
        auto const currentSigning =
            env.app().validatorManifests().getSigningKey(
                valKeys.keys->masterPublicKey);
        BEAST_EXPECT(currentSigning && *currentSigning == newSigningKey.first);
        BEAST_EXPECT(
            env.app().validatorManifests().getMasterKey(newSigningKey.first) ==
            valKeys.keys->masterPublicKey);
        BEAST_EXPECT(
            env.app().validatorManifests().getMasterKey(oldSigningKey.first) ==
            oldSigningKey.first);
        BEAST_EXPECT(
            env.app().validatorManifests().getMasterKey(oldSigningKey.first) !=
            valKeys.keys->masterPublicKey);

        auto const replayed = buildLedger(
            LedgerReplay(originLedger, built), tapNONE, env.app(), env.journal);
        BEAST_EXPECT(replayed->info().hash == built->info().hash);
    }

    void
    testExportCommitteeSetupWithoutUNLReport(FeatureBitset features)
    {
        testcase("Export committee setup and use-time membership");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};
        Account const alice{"alice"};
        Account const carol{"carol"};
        env.fund(XRP(10000), alice, carol);
        env.close();

        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        auto const roster = defaultExportCommittee(env);
        auto const digest = exportCommitteeHash(makeSlice(roster));
        Json::Value setup;
        setup[jss::TransactionType] = jss::Export;
        setup[jss::Account] = alice.human();
        setup[sfExportCommittee.jsonName] = strHex(roster);
        env(setup, fee(XRP(1)), ter(tesSUCCESS));
        env.close();
        BEAST_EXPECT(
            env.current()->read(keylet::exportCommittee(alice.id(), digest)));

        auto const accountAfterCreate = env.le(keylet::account(alice.id()));
        BEAST_EXPECT(accountAfterCreate);
        if (!accountAfterCreate)
            return;
        auto const ownerCountAfterCreate =
            accountAfterCreate->getFieldU32(sfOwnerCount);

        // Repeating the same setup is idempotent and does not charge reserve.
        env(setup, fee(XRP(1)), ter(tesSUCCESS));
        env.close();
        auto const accountAfterRepeat = env.le(keylet::account(alice.id()));
        BEAST_EXPECT(accountAfterRepeat);
        if (!accountAfterRepeat)
            return;
        BEAST_EXPECT(
            accountAfterRepeat->getFieldU32(sfOwnerCount) ==
            ownerCountAfterCreate);

        // An account may provision more than one immutable committee.
        auto const otherKey = randomKeyPair(KeyType::secp256k1).first;
        auto const otherRoster =
            serializeExportCommittee({valKeys.keys->masterPublicKey, otherKey});
        auto const otherDigest = exportCommitteeHash(makeSlice(otherRoster));
        auto otherSetup = setup;
        otherSetup[sfExportCommittee.jsonName] = strHex(otherRoster);
        env(otherSetup, fee(XRP(1)), ter(tesSUCCESS));
        env.close();
        BEAST_EXPECT(env.current()->read(
            keylet::exportCommittee(alice.id(), otherDigest)));

        Json::Value eraseOther;
        eraseOther[jss::TransactionType] = jss::Export;
        eraseOther[jss::Account] = alice.human();
        eraseOther[jss::Flags] = tfExportEraseCommittee;
        eraseOther[sfExportCommitteeHash.jsonName] = to_string(otherDigest);
        env(eraseOther, fee(XRP(1)), ter(tesSUCCESS));
        env.close();
        BEAST_EXPECT(!env.current()->read(
            keylet::exportCommittee(alice.id(), otherDigest)));
        auto const accountAfterDelete = env.le(keylet::account(alice.id()));
        BEAST_EXPECT(accountAfterDelete);
        if (!accountAfterDelete)
            return;
        BEAST_EXPECT(
            accountAfterDelete->getFieldU32(sfOwnerCount) ==
            ownerCountAfterCreate);

        // Setup is independent of the live view. Actual use is not.
        seedUNLReportLedger(env, {otherKey});
        auto const seq = env.current()->seq();
        auto const innerObj = buildExportedPayment(
            alice.id(),
            carol.id(),
            seq + 1,
            seq + ExportLimits::maxAdmissionWindowLedgers);
        Json::Value intent;
        intent[jss::TransactionType] = jss::Export;
        intent[jss::Account] = alice.human();
        intent[jss::LastLedgerSequence] =
            seq + ExportLimits::maxAdmissionWindowLedgers;
        intent[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);
        intent[sfExportCommitteeHash.jsonName] = to_string(digest);
        env(intent, fee(XRP(1)), ter(tecEXPORT_COMMITTEE_UNAVAILABLE));
        env.close();
    }

    void
    testExportNetworkRejectsWithoutUNLReport(FeatureBitset features)
    {
        testcase("ttEXPORT rejects without source UNLReport committee");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();
        forceNonStandalone(env.app());
        BEAST_EXPECT(!env.app().config().standalone());

        auto const seq = env.current()->seq();
        auto const ticketSeq = std::uint32_t{1};
        auto const lls = seq + ExportLimits::maxAdmissionWindowLedgers;
        auto innerObj = buildExportedPayment(
            alice.id(), carol.id(), seq + 1, lls, ticketSeq);
        auto jt = makeExportJTx(env, alice, innerObj, lls);
        auto const exportTx = jt.stx;
        BEAST_EXPECT(exportTx);
        if (!exportTx)
            return;
        auto const txHash = exportTx->getTransactionID();

        auto& ce = env.app().getConsensusExtensions();
        ce.setExportEnabledThisRound(true);
        ce.cacheUNLReport(env.app().getLedgerMaster().getClosedLedger());
        auto const view = ce.activeValidatorView();
        BEAST_EXPECT(!view->fromUNLReport);

        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto next = std::make_shared<Ledger>(
            *parent, env.app().timeKeeper().closeTime());
        OpenView accum(&*next);
        auto const result =
            ripple::apply(env.app(), accum, *exportTx, tapNONE, env.journal);

        BEAST_EXPECT(result.ter == tecEXPORT_COMMITTEE_UNAVAILABLE);
        BEAST_EXPECT(result.applied);
        BEAST_EXPECT(!next->read(keylet::exportLatch(alice.id(), txHash)));
    }

    void
    testExportNetworkLastLedgerSequenceBoundary(FeatureBitset features)
    {
        testcase("ttEXPORT admits intent at LastLedgerSequence boundary");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};
        Account const alice{"alice"};
        Account const carol{"carol"};
        env.fund(XRP(10000), alice, carol);
        env.close();

        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;
        seedUNLReportLedger(env, {valKeys.keys->masterPublicKey});
        forceNonStandalone(env.app());

        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto const applySeq = parent->seq() + 1;
        auto const ticketSeq = std::uint32_t{1};
        auto innerObj = buildExportedPayment(
            alice.id(), carol.id(), applySeq, applySeq, ticketSeq);
        auto const exportTx = makeExportJTx(env, alice, innerObj, applySeq).stx;
        BEAST_EXPECT(exportTx);
        if (!exportTx)
            return;

        auto next = std::make_shared<Ledger>(
            *parent, env.app().timeKeeper().closeTime());
        OpenView accum(&*next);
        auto const result =
            ripple::apply(env.app(), accum, *exportTx, tapNONE, env.journal);
        BEAST_EXPECT(result.ter == tesSUCCESS);
        BEAST_EXPECT(result.applied);
        accum.apply(*next);
        BEAST_EXPECT(next->read(
            keylet::exportLatch(alice.id(), exportTx->getTransactionID())));
    }

    void
    testOpenLedgerExportLimit(FeatureBitset features)
    {
        testcase("ttEXPORT open ledger limit");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();

        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;
        seedUNLReportLedger(env, {valKeys.keys->masterPublicKey});

        auto submitExport = [&](std::uint32_t ticketSeq, TER expected) {
            auto const seq = env.current()->seq();
            auto innerObj = buildExportedPayment(
                alice.id(),
                carol.id(),
                seq + 1,
                seq + ExportLimits::maxAdmissionWindowLedgers,
                ticketSeq);

            Json::Value jv;
            jv[jss::TransactionType] = jss::Export;
            jv[jss::Account] = alice.human();
            jv[jss::LastLedgerSequence] =
                seq + ExportLimits::maxAdmissionWindowLedgers;
            jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);
            bindExportAuthority(env, jv);

            env(jv, fee(XRP(1)), ter(expected));
        };

        for (std::uint32_t i = 1; i <= ExportLimits::maxPendingExports; ++i)
            submitExport(i, tesSUCCESS);

        submitExport(ExportLimits::maxPendingExports + 1, tecDIR_FULL);
    }

    void
    testExportLatchLimit(FeatureBitset features)
    {
        testcase("Export latch pending export limit");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();

        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;
        seedUNLReportLedger(env, {valKeys.keys->masterPublicKey});

        std::map<std::uint32_t, uint256> origins;

        auto submitClosedExport = [&](std::uint32_t ticketSeq,
                                      TER expected,
                                      bool expectLatch) {
            auto const seq = env.current()->seq();
            auto innerObj = buildExportedPayment(
                alice.id(),
                carol.id(),
                seq + 1,
                seq + ExportLimits::maxAdmissionWindowLedgers,
                ticketSeq);

            Json::Value jv;
            jv[jss::TransactionType] = jss::Export;
            jv[jss::Account] = alice.human();
            jv[jss::LastLedgerSequence] =
                seq + ExportLimits::maxAdmissionWindowLedgers;
            jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);
            bindExportAuthority(env, jv);

            env(jv, fee(XRP(1)), ter(expected));
            auto const submittedOrigin = env.tx()->getTransactionID();
            auto const meta = env.meta();
            BEAST_EXPECT(meta);
            BEAST_EXPECT(
                (*meta)[sfTransactionResult] ==
                static_cast<std::uint8_t>(TERtoInt(expected)));

            if (expected == tesSUCCESS)
                origins.emplace(ticketSeq, submittedOrigin);
            auto const found = origins.find(ticketSeq);
            auto const latch = found == origins.end()
                ? nullptr
                : env.le(keylet::exportLatch(alice.id(), found->second));
            BEAST_EXPECT(expectLatch == static_cast<bool>(latch));
            env.close();
        };

        for (std::uint32_t i = 1; i <= ExportLimits::maxPendingExports; ++i)
            submitClosedExport(i, tesSUCCESS, true);

        submitClosedExport(1, tecDIR_FULL, true);
        submitClosedExport(
            ExportLimits::maxPendingExports + 1, tecDIR_FULL, false);
    }

    void
    testExportLatchLifecycle(FeatureBitset features)
    {
        testcase("Export latch lifecycle");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();

        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;
        seedUNLReportLedger(env, {valKeys.keys->masterPublicKey});

        auto const seq = env.current()->seq();

        std::uint32_t constexpr ticketSeq = 42;
        auto innerObj = buildExportedPayment(
            alice.id(),
            carol.id(),
            seq + 1,
            seq + ExportLimits::maxAdmissionWindowLedgers,
            ticketSeq);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[jss::LastLedgerSequence] =
            seq + ExportLimits::maxAdmissionWindowLedgers;
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);
        bindExportAuthority(env, jv);

        env(jv, fee(XRP(1)), ter(tesSUCCESS));
        auto const origin = env.tx()->getTransactionID();
        auto const meta = env.meta();
        BEAST_EXPECT(meta);
        BEAST_EXPECT(
            (*meta)[sfTransactionResult] ==
            static_cast<std::uint8_t>(TERtoInt(tesSUCCESS)));

        auto const latch = env.le(keylet::exportLatch(alice.id(), origin));
        BEAST_EXPECT(latch);
        if (latch)
        {
            BEAST_EXPECT(latch->getAccountID(sfAccount) == alice.id());
            BEAST_EXPECT(latch->getFieldU32(sfTicketSequence) == ticketSeq);
            BEAST_EXPECT(latch->getFieldH256(sfTransactionHash) == origin);
            BEAST_EXPECT(latch->isFieldPresent(sfDigest));
            BEAST_EXPECT(latch->isFieldPresent(sfExportNode));
            BEAST_EXPECT(!latch->isFieldPresent(sfExportSignatureHash));
            BEAST_EXPECT(latch->getFieldH256(sfDigest) == [&] {
                auto const identity = ExportOriginMemo::identityForm(
                    makeSTTx(innerObj),
                    ExportOriginMemo::Origin{
                        env.app().config().NETWORK_ID, 0, origin});
                BEAST_EXPECT(identity);
                return identity
                    ? ExportResultBuilder::exportIntentHash(identity.value())
                    : uint256{};
            }());
        }

        env.close();
    }

    void
    testControlExportLatchViaTxn(FeatureBitset features)
    {
        testcase("ttEXPORT controls Export latch via origin W");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};
        env.fund(XRP(10000), alice, carol);
        env.close();

        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;
        seedUNLReportLedger(env, {valKeys.keys->masterPublicKey});

        // Control of a non-existent W returns tecNO_ENTRY.
        Json::Value jvControl;
        jvControl[jss::TransactionType] = jss::Export;
        jvControl[jss::Account] = alice.human();
        jvControl[sfTransactionHash.jsonName] = to_string(uint256{42});

        env(jvControl, fee(XRP(1)), ter(tecNO_ENTRY));

        auto zeroControl = jvControl;
        zeroControl[sfTransactionHash.jsonName] = to_string(uint256{});
        env(zeroControl, fee(XRP(1)), ter(temMALFORMED));

        Json::Value emptyControl;
        emptyControl[jss::TransactionType] = jss::Export;
        emptyControl[jss::Account] = alice.human();
        env(emptyControl, fee(XRP(1)), ter(temMALFORMED));
        env.close();

        std::uint32_t constexpr ticketSeq = 42;
        auto const seq = env.current()->seq();
        auto innerObj = buildExportedPayment(
            alice.id(),
            carol.id(),
            seq + 1,
            seq + ExportLimits::maxAdmissionWindowLedgers,
            ticketSeq);

        Json::Value jvExport;
        jvExport[jss::TransactionType] = jss::Export;
        jvExport[jss::Account] = alice.human();
        jvExport[jss::LastLedgerSequence] =
            seq + ExportLimits::maxAdmissionWindowLedgers;
        jvExport[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);
        auto const committeeRoster = defaultExportCommittee(env);
        auto const committeeDigest =
            exportCommitteeHash(makeSlice(committeeRoster));
        bindExportAuthority(env, jvExport, committeeRoster);

        auto mixed = jvExport;
        mixed[sfTransactionHash.jsonName] = to_string(uint256{42});
        env(mixed, fee(XRP(1)), ter(temMALFORMED));

        auto eraseCreation = jvExport;
        eraseCreation[jss::Flags] = tfExportEraseLatch;
        env(eraseCreation, fee(XRP(1)), ter(temMALFORMED));

        auto unknownFlag = jvControl;
        unknownFlag[jss::Flags] = 0x00040000;
        env(unknownFlag, fee(XRP(1)), ter(temINVALID_FLAG));

        env(jvExport, fee(XRP(1)), ter(tesSUCCESS));
        auto const origin = env.tx()->getTransactionID();
        auto const exportMeta = env.meta();
        BEAST_EXPECT(exportMeta);
        BEAST_EXPECT(
            (*exportMeta)[sfTransactionResult] ==
            static_cast<std::uint8_t>(TERtoInt(tesSUCCESS)));
        auto const latchKey = keylet::exportLatch(alice.id(), origin);
        auto const latchBeforeCancel = env.le(latchKey);
        auto const accountBeforeCancel = env.le(keylet::account(alice.id()));
        auto const pendingBeforeCancel = env.le(keylet::pendingExports());
        BEAST_EXPECT(latchBeforeCancel);
        BEAST_EXPECT(accountBeforeCancel);
        BEAST_EXPECT(pendingBeforeCancel);
        if (!latchBeforeCancel || !accountBeforeCancel || !pendingBeforeCancel)
            return;
        auto const accountExportCount =
            accountBeforeCancel->getFieldU16(sfExportCount);
        auto const ownerCount = accountBeforeCancel->getFieldU32(sfOwnerCount);
        auto const reserveBeforeCancel =
            env.current()->fees().accountReserve(ownerCount);
        auto const pendingExportCount =
            pendingBeforeCancel->getFieldU16(sfExportCount);
        BEAST_EXPECT(pendingExportCount > 0);

        Json::Value eraseCommittee;
        eraseCommittee[jss::TransactionType] = jss::Export;
        eraseCommittee[jss::Account] = alice.human();
        eraseCommittee[jss::Flags] = tfExportEraseCommittee;
        eraseCommittee[sfExportCommitteeHash.jsonName] =
            to_string(committeeDigest);
        env(eraseCommittee, fee(XRP(1)), ter(tecHAS_OBLIGATIONS));

        Json::Value jvRetain;
        jvRetain[jss::TransactionType] = jss::Export;
        jvRetain[jss::Account] = alice.human();
        jvRetain[sfTransactionHash.jsonName] = to_string(origin);

        env(jvRetain, fee(XRP(1)), ter(tesSUCCESS));
        auto const cancelMeta = env.meta();
        BEAST_EXPECT(cancelMeta);
        BEAST_EXPECT(
            (*cancelMeta)[sfTransactionResult] ==
            static_cast<std::uint8_t>(TERtoInt(tesSUCCESS)));

        auto const canceledLatch = env.le(latchKey);
        auto const accountAfterCancel = env.le(keylet::account(alice.id()));
        auto const pendingAfterCancel = env.le(keylet::pendingExports());
        BEAST_EXPECT(canceledLatch);
        BEAST_EXPECT(accountAfterCancel);
        BEAST_EXPECT(pendingAfterCancel);
        if (!canceledLatch || !accountAfterCancel || !pendingAfterCancel)
            return;
        auto const flags = canceledLatch->isFieldPresent(sfFlags)
            ? canceledLatch->getFieldU32(sfFlags)
            : std::uint32_t{0};
        BEAST_EXPECT((flags & lsfExportCanceled) != 0);
        BEAST_EXPECT(!canceledLatch->isFieldPresent(sfExportNode));
        BEAST_EXPECT(canceledLatch->isFieldPresent(sfOwnerNode));
        BEAST_EXPECT(
            accountAfterCancel->getFieldU16(sfExportCount) ==
            accountExportCount);
        BEAST_EXPECT(
            accountAfterCancel->getFieldU32(sfOwnerCount) == ownerCount);
        BEAST_EXPECT(
            env.current()->fees().accountReserve(ownerCount) ==
            reserveBeforeCancel);
        BEAST_EXPECT(
            pendingAfterCancel->getFieldU16(sfExportCount) ==
            pendingExportCount - 1);

        auto jvErase = jvRetain;
        jvErase[jss::Flags] = tfExportEraseLatch;
        env(jvErase, fee(XRP(1)), ter(tesSUCCESS));

        auto const accountAfterErase = env.le(keylet::account(alice.id()));
        auto const pendingAfterErase = env.le(keylet::pendingExports());
        BEAST_EXPECT(!env.le(latchKey));
        BEAST_EXPECT(accountAfterErase);
        BEAST_EXPECT(pendingAfterErase);
        if (!accountAfterErase || !pendingAfterErase)
            return;
        BEAST_EXPECT(
            accountAfterErase->getFieldU16(sfExportCount) ==
            accountExportCount - 1);
        BEAST_EXPECT(
            accountAfterErase->getFieldU32(sfOwnerCount) == ownerCount - 1);
        BEAST_EXPECT(
            pendingAfterErase->getFieldU16(sfExportCount) ==
            pendingExportCount - 1);

        env(eraseCommittee, fee(XRP(1)), ter(tesSUCCESS));
        BEAST_EXPECT(
            !env.le(keylet::exportCommittee(alice.id(), committeeDigest)));

        env.close();
    }

    void
    testExportRejectsInvalidTicketSequence(FeatureBitset features)
    {
        testcase("ttEXPORT requires a nonzero TicketSequence");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();

        auto const seq = env.current()->seq();
        auto innerObj = buildExportedPayment(
            alice.id(),
            carol.id(),
            seq + 1,
            seq + ExportLimits::maxAdmissionWindowLedgers,
            std::nullopt);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[jss::LastLedgerSequence] =
            seq + ExportLimits::maxAdmissionWindowLedgers;
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);
        bindExportAuthority(env, jv);

        env(jv, fee(XRP(1)), ter(temMALFORMED));

        innerObj.setFieldU32(sfTicketSequence, 0);
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);
        env(jv, fee(XRP(1)), ter(temMALFORMED));
        env.close();
    }

    void
    testExportRejectsMissingLastLedgerSequence(FeatureBitset features)
    {
        testcase("ttEXPORT rejects export without LastLedgerSequence");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();

        auto const seq = env.current()->seq();
        auto innerObj = buildExportedPayment(
            alice.id(),
            carol.id(),
            seq + 1,
            seq + ExportLimits::maxAdmissionWindowLedgers);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);
        bindExportAuthority(env, jv);

        env(jv, fee(XRP(1)), ter(temMALFORMED));
        env.close();
    }

    void
    testExportRejectsSignedInnerTransaction(FeatureBitset features)
    {
        testcase("ttEXPORT rejects signed inner transaction");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};
        Account const alice{"alice"};
        Account const carol{"carol"};
        env.fund(XRP(10000), alice, carol);
        env.close();

        auto const seq = env.current()->seq();
        auto innerObj = buildExportedPayment(
            alice.id(),
            carol.id(),
            seq + 1,
            seq + ExportLimits::maxAdmissionWindowLedgers);
        innerObj.setFieldVL(sfSigningPubKey, alice.pk().slice());

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[jss::LastLedgerSequence] =
            seq + ExportLimits::maxAdmissionWindowLedgers;
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);
        bindExportAuthority(env, jv);

        env(jv, fee(XRP(1)), ter(temMALFORMED));
    }

    void
    testExportRejectsLongAdmissionWindow(FeatureBitset features)
    {
        testcase("ttEXPORT rejects LastLedgerSequence beyond admission cap");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();

        auto const seq = env.current()->seq();
        auto const lls = seq + ExportLimits::maxAdmissionWindowLedgers + 1;
        auto innerObj =
            buildExportedPayment(alice.id(), carol.id(), seq + 1, lls);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[jss::LastLedgerSequence] = lls;
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);
        bindExportAuthority(env, jv);

        env(jv, fee(XRP(1)), ter(temMALFORMED));
        env.close();
    }

    void
    testExportRejectsMalformed(FeatureBitset features)
    {
        testcase("ttEXPORT rejects when neither export nor cancel present");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();

        env(jv, fee(XRP(1)), ter(temMALFORMED));
        env.close();
    }

    void
    testExportImportRoundTrip(FeatureBitset features)
    {
        testcase("Export → XPOP → Import round-trip");

        using namespace jtx;

        // The export round-trip is a 3-way handshake:
        //   1. Xahau: ttEXPORT → validators sign the inner tx →
        //      Export latch + metadata pointing to the signature witness
        //   2. XRPL:  submit the multisigned blob raw (alice's
        //      SignerList on XRPL contains the Xahau validator keys)
        //   3. Xahau: build XPOP from execution, import it back →
        //      Export latch consumed
        //
        // In standalone mode, the same ttEXPORT_SIGNATURES witness is
        // synthesized locally from the node's validator key — no consensus
        // proposals needed.

        auto const xpopCtx = xpop::TestXPOPContext::create(3);

        Account const alice{"alice"};
        Account const carol{"carol"};
        std::uint32_t const xahauNetworkID = 21337;
        std::uint32_t const targetNetworkID = 31337;

        // ── Xahau env: export the inner tx ─────────────────────────────
        Env xahau{*this, xpopCtx.makeEnvConfig(xahauNetworkID), features};

        xahau.fund(XRP(10000), alice, carol);
        xahau.close();

        // Burn XRP so B2M crediting works on import.
        auto const master = Account("masterpassphrase");
        xahau(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
        xahau.close();

        std::uint32_t const ticketSeq = 2;  // alice's first ticket on XRPL

        auto const callback = buildExportCallbackXPOP(
            xahau, xpopCtx, alice, carol, targetNetworkID, ticketSeq);
        if (callback.vlInfo)
        {
            BEAST_EXPECT(importVLSequence(xahau, callback.vlInfo->second) == 0);
        }

        // ── Back to Xahau: import the XPOP ────────────────────────────
        auto const feeDrops = xahau.current()->fees().base;

        // A valid XPOP is not a permissionless callback capability. The
        // normally authorized outer Import account must still match the
        // target transaction account and latch owner.
        xahau(
            import::import(carol, callback.xpopJson),
            fee(feeDrops * 10),
            ter(temMALFORMED));
        BEAST_EXPECT(xahau.current()->exists(
            keylet::exportLatch(alice.id(), callback.originTxn)));
        if (callback.vlInfo)
            BEAST_EXPECT(
                importVLSequence(xahau, callback.vlInfo->second) == 0);

        xahau(
            import::import(alice, callback.xpopJson),
            fee(feeDrops * 10),
            ter(tesSUCCESS));
        xahau.close();

        // Export latch should be consumed after import.
        BEAST_EXPECT(!xahau.current()->exists(
            keylet::exportLatch(alice.id(), callback.originTxn)));
        if (callback.vlInfo)
        {
            BEAST_EXPECT(
                importVLSequence(xahau, callback.vlInfo->second) ==
                callback.vlInfo->first);
        }
    }

    void
    testCanceledExportAcceptsMatchingImport(FeatureBitset features)
    {
        testcase("canceled Export accepts matching callback");

        using namespace jtx;

        auto const xpopCtx = xpop::TestXPOPContext::create(3);
        Account const alice{"alice"};
        Account const carol{"carol"};
        std::uint32_t const xahauNetworkID = 21337;
        std::uint32_t const targetNetworkID = 31337;

        Env xahau{*this, xpopCtx.makeEnvConfig(xahauNetworkID), features};
        xahau.fund(XRP(10000), alice, carol);
        xahau.close();

        auto const callback = buildExportCallbackXPOP(
            xahau, xpopCtx, alice, carol, targetNetworkID, 2);

        Json::Value cancel;
        cancel[jss::TransactionType] = jss::Export;
        cancel[jss::Account] = alice.human();
        cancel[sfTransactionHash.jsonName] = to_string(callback.originTxn);
        xahau(cancel, fee(XRP(1)), ter(tesSUCCESS));
        xahau.close();

        auto const latchKey =
            keylet::exportLatch(alice.id(), callback.originTxn);
        auto const canceledLatch = xahau.current()->read(latchKey);
        BEAST_EXPECT(canceledLatch);
        if (!canceledLatch)
            return;
        auto const flags = canceledLatch->isFieldPresent(sfFlags)
            ? canceledLatch->getFieldU32(sfFlags)
            : std::uint32_t{0};
        BEAST_EXPECT((flags & lsfExportCanceled) != 0);
        BEAST_EXPECT(!canceledLatch->isFieldPresent(sfExportNode));
        BEAST_EXPECT(canceledLatch->isFieldPresent(sfExportSignatureHash));

        auto const importFee = xahau.current()->fees().base * 10;
        xahau(
            import::import(alice, callback.xpopJson),
            fee(importFee),
            ter(tesSUCCESS));
        xahau.close();
        BEAST_EXPECT(!xahau.current()->exists(latchKey));
    }

    void
    testExportImportRejectsStaleImportVL(FeatureBitset features)
    {
        testcase("Export callback rejects stale ImportVL");

        using namespace jtx;

        auto const xpopCtx = xpop::TestXPOPContext::create(3);

        Account const alice{"alice"};
        Account const carol{"carol"};
        Account const dave{"dave"};
        std::uint32_t const xahauNetworkID = 21337;
        std::uint32_t const targetNetworkID = 31337;
        std::uint32_t const ticketSeq = 2;

        Env xahau{*this, xpopCtx.makeEnvConfig(xahauNetworkID), features};

        xahau.fund(XRP(10000), alice, carol);
        xahau.close();

        auto const master = Account("masterpassphrase");
        xahau(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
        xahau.close();

        auto const callback = buildExportCallbackXPOP(
            xahau, xpopCtx, alice, carol, targetNetworkID, ticketSeq);
        BEAST_EXPECT(callback.vlInfo);
        if (!callback.vlInfo)
            return;

        auto const freshSeq = callback.vlInfo->first + 1;

        ratchetImportVLWithBurnToMint(
            xahau, xpopCtx, xahauNetworkID, dave, freshSeq);
        BEAST_EXPECT(
            importVLSequence(xahau, callback.vlInfo->second) == freshSeq);

        auto const feeDrops = xahau.current()->fees().base;
        xahau(
            import::import(alice, callback.xpopJson),
            fee(feeDrops * 10),
            ter(tefPAST_IMPORT_VL_SEQ));
        xahau.close();

        BEAST_EXPECT(xahau.current()->exists(
            keylet::exportLatch(alice.id(), callback.originTxn)));
        BEAST_EXPECT(
            importVLSequence(xahau, callback.vlInfo->second) == freshSeq);
    }

    // Override focused_test() to run a specific test in isolation.
    // Set FOCUSED_TEST=1 to skip the full suite and run only this.
    void
    focused_test(FeatureBitset const& features)
    {
        testXportPayment(features);
    }

    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        FeatureBitset const allWithExport{all | featureExport};

        if (std::getenv("FOCUSED_TEST"))
        {
            focused_test(allWithExport);
            return;
        }

        // Hook-based xport tests
        testXportPayment(allWithExport);
        testXportRejectsLocalNetworkID(allWithExport);
        testXportRejectsUnconfiguredNetworkID(allWithExport);
        testExportRejectsAmbiguousAbsentNetworkID();
        testExportRejectsInvalidOriginMemoProjection();
        testXportEmissionLimit(allWithExport);

        // ttEXPORT transactor tests
        testExportTxnOpenLedger(allWithExport);
        testExportNetworkAdmitsIntentWithoutQuorum(allWithExport);
        testExportLatchInsufficientReserve(allWithExport);
        testLaterLedgerWitnessTransitionAndReplay(allWithExport);
        testExportCommitteeSetupWithoutUNLReport(allWithExport);
        testExportNetworkRejectsWithoutUNLReport(allWithExport);
        testExportNetworkLastLedgerSequenceBoundary(allWithExport);
        testOpenLedgerExportLimit(allWithExport);
        testExportLatchLimit(allWithExport);
        testExportLatchLifecycle(allWithExport);
        testControlExportLatchViaTxn(allWithExport);
        testExportRejectsInvalidTicketSequence(allWithExport);
        testExportRejectsMissingLastLedgerSequence(allWithExport);
        testExportRejectsSignedInnerTransaction(allWithExport);
        testExportRejectsLongAdmissionWindow(allWithExport);
        testExportRejectsMalformed(allWithExport);

        // Round-trip test
        testExportImportRoundTrip(allWithExport);
        testCanceledExportAcceptsMatchingImport(allWithExport);
        testExportImportRejectsStaleImportVL(allWithExport);
    }
};

BEAST_DEFINE_TESTSUITE(Export, app, ripple);

}  // namespace test
}  // namespace ripple
