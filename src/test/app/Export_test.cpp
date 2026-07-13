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
#include <xrpld/app/misc/RuntimeConfig.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/app/tx/detail/ExportLedgerOps.h>
#include <xrpld/app/tx/detail/ExportResultBuilder.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Import.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/jss.h>

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

    static RCLTxSet
    makeRCLTxSet(
        Application& app,
        std::vector<std::shared_ptr<STTx const>> const& txns)
    {
        auto map = std::make_shared<SHAMap>(
            SHAMapType::TRANSACTION, app.getNodeFamily());
        map->setUnbacked();

        for (auto const& tx : txns)
        {
            Serializer s;
            tx->add(s);
            map->addItem(
                SHAMapNodeType::tnTRANSACTION_NM,
                make_shamapitem(tx->getTransactionID(), s.slice()));
        }

        return RCLTxSet{map->snapShot(false)};
    }

    static ExportResultBuilder::SignatureWitnesses
    makeExportSignatureWitnesses(
        uint256 const& exportTxHash,
        ExportResultBuilder::SignatureSnapshot signatures,
        LedgerIndex seq)
    {
        auto witness = ExportResultBuilder::buildSignatureWitness(
            exportTxHash, signatures, seq);
        auto const witnessHash = witness.getTransactionID();

        ExportResultBuilder::SignatureWitnesses witnesses;
        witnesses.emplace(
            exportTxHash,
            ExportResultBuilder::SignatureWitness{
                witnessHash, std::move(signatures)});
        return witnesses;
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
        Json::Value exportedTxnJson;
        std::uint32_t ticketSeq;
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
        innerObj.setFieldAmount(sfFee, XRPAmount{20});
        innerObj.setFieldVL(sfSigningPubKey, Blob{});
        innerObj.setAccountID(sfAccount, alice.id());
        innerObj.setAccountID(sfDestination, carol.id());

        Json::Value jvExport;
        jvExport[jss::TransactionType] = jss::Export;
        jvExport[jss::Account] = alice.human();
        jvExport[jss::LastLedgerSequence] =
            xahau.current()->seq() + ExportLimits::maxRetryLedgers;
        jvExport[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

        xahau(jvExport, fee(XRP(1)), ter(tesSUCCESS));
        xahau.close();

        auto const exportMeta = xahau.meta();
        BEAST_EXPECT(exportMeta);

        Blob multisignedBlob;
        if (exportMeta && exportMeta->isFieldPresent(sfExportResult))
        {
            auto const& result =
                exportMeta->peekAtField(sfExportResult).downcast<STObject>();
            BEAST_EXPECT(result.isFieldPresent(sfExportSignatureHash));
            if (result.isFieldPresent(sfExportSignatureHash))
            {
                auto const witnessHash =
                    result.getFieldH256(sfExportSignatureHash);
                auto const witnessRead = xahau.current()->txRead(witnessHash);
                auto const& witnessTx = witnessRead.first;
                BEAST_EXPECT(witnessTx);
                auto signatures = witnessTx
                    ? ExportResultBuilder::signaturesFromWitness(*witnessTx)
                    : std::nullopt;
                BEAST_EXPECT(signatures);

                STTx const innerTx = makeSTTx(innerObj);
                auto expTxn = ExportResultBuilder::buildMultiSignedExportedTxn(
                    innerTx,
                    signatures ? *signatures
                               : ExportResultBuilder::SignatureSnapshot{});

                Serializer s;
                expTxn.add(s);
                multisignedBlob = s.peekData();

                log << "Xahau: ExportResult.ExportSignatureHash = "
                    << witnessHash << std::endl;
                log << "Xahau: assembled ExportedTxn = "
                    << expTxn.getJson(JsonOptions::none).toStyledString()
                    << std::endl;
            }
        }
        log << "Xahau: multisigned blob size = " << multisignedBlob.size()
            << std::endl;
        BEAST_EXPECT(!multisignedBlob.empty());

        BEAST_EXPECT(xahau.current()->exists(
            keylet::shadowTicket(alice.id(), ticketSeq)));

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

        return CallbackXPOP{
            xpopJson, innerObj.getJson(JsonOptions::none), ticketSeq, vlInfo};
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
                    STTx tx =
                        unl::createUNLReportTx(env.current()->seq(), pk, pk);
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
        LedgerIndex lls)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = account.human();
        jv[jss::LastLedgerSequence] = lls;
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);
        return env.jt(jv, jtx::fee(jtx::XRP(1)), jtx::ter(tesSUCCESS));
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
        extern int64_t xport(uint32_t write_ptr, uint32_t write_len, uint32_t read_ptr, uint32_t read_len);
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
            int64_t xport_result = xport(SBUF(hash), (uint32_t)tx, buf - tx);
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
        extern int64_t xport(uint32_t write_ptr, uint32_t write_len, uint32_t read_ptr, uint32_t read_len);
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
            int64_t xport_result = xport(SBUF(hash), (uint32_t)tx, buf - tx);
            // xport should return EXPORT_FAILURE (-46), ASSERT will rollback
            ASSERT(xport_result == 32);

            return accept(0, 0, 0);
        }
    )[test.hook]"];

    // Helper to build hook params with DST
    static Json::Value
    makeDstParams(AccountID const& dst)
    {
        Json::Value params(Json::arrayValue);
        Json::Value param;
        param[jss::HookParameter] = Json::Value(Json::objectValue);
        param[jss::HookParameter][jss::HookParameterName] =
            strHex(std::string("DST"));
        param[jss::HookParameter][jss::HookParameterValue] = strHex(dst);
        params.append(param);
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

        // Install xport hook on alice
        env(ripple::test::jtx::hook(alice, {{hso(xport_wasm)}}, 0),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        // Trigger hook with payment containing DST parameter
        auto params = makeDstParams(carol.id());

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

        // The emitted ttEXPORT retries and is not included. The retired
        // same-ledger sidecar path no longer synthesizes a signature witness.
        {
            auto const ledger = env.closed();
            int exportCount = 0;
            int witnessCount = 0;
            for (auto const& [stx, meta] : ledger->txs)
            {
                if (stx->getTxnType() == ttEXPORT)
                {
                    BEAST_EXPECT(stx->isFieldPresent(sfEmitDetails));
                    ++exportCount;
                    continue;
                }

                if (stx->getTxnType() == ttEXPORT_SIGNATURES)
                    ++witnessCount;
            }
            BEAST_EXPECT(exportCount == 0);
            BEAST_EXPECT(witnessCount == 0);
        }
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
        auto params = makeDstParams(carol.id());

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
        auto params = makeDstParams(carol.id());

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

        env(ripple::test::jtx::hook(alice, {{hso(xport_wasm)}}, 0),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        auto params = makeDstParams(carol.id());
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

        auto const seq = env.current()->seq();
        auto innerObj = buildExportedPayment(
            alice.id(),
            carol.id(),
            seq + 1,
            seq + ExportLimits::maxRetryLedgers);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[jss::LastLedgerSequence] = seq + ExportLimits::maxRetryLedgers;
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

        env(jv, fee(XRP(1)), ter(tesSUCCESS));
        env.close();
    }

    void
    testExportNetworkRetryWithoutQuorum(FeatureBitset features)
    {
        testcase("ttEXPORT network mode retries without quorum");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();
        forceNonStandalone(env.app());
        BEAST_EXPECT(!env.app().config().standalone());
        ConsensusTestConfig cfg;
        cfg.noExportSig = true;
        env.app().getRuntimeConfig().setGlobalConfig(cfg);

        auto const seq = env.current()->seq();
        auto const ticketSeq = std::uint32_t{1};
        auto const lls = seq + ExportLimits::maxRetryLedgers;
        auto innerObj = buildExportedPayment(
            alice.id(), carol.id(), seq + 1, lls, ticketSeq);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[jss::LastLedgerSequence] = lls;
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

        env(jv, fee(XRP(1)), ter(tesSUCCESS));
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

        BEAST_EXPECT(!closedResult);
        BEAST_EXPECT(!env.le(keylet::shadowTicket(alice.id(), ticketSeq)));
    }

    void
    testExportProposalSigningRequiresBoundedUNLReport(FeatureBitset features)
    {
        testcase("Export proposal signing requires bounded UNLReport");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};
        Account const alice{"alice"};
        Account const carol{"carol"};
        env.fund(XRP(10000), alice, carol);
        env.close();

        auto submitOpenExport = [&](std::uint32_t ticketSeq) {
            auto const seq = env.current()->seq();
            auto innerObj = buildExportedPayment(
                alice.id(),
                carol.id(),
                seq + 1,
                seq + ExportLimits::maxRetryLedgers,
                ticketSeq);
            Json::Value jv;
            jv[jss::TransactionType] = jss::Export;
            jv[jss::Account] = alice.human();
            jv[jss::LastLedgerSequence] = seq + ExportLimits::maxRetryLedgers;
            jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);
            env(jv, fee(XRP(1)), ter(tesSUCCESS));
        };

        auto attach = [&](ConsensusExtensions& ce) {
            protocol::TMProposeSet prop;
            auto const closed = env.app().getLedgerMaster().getClosedLedger();
            RCLCxPeerPos::Proposal proposal{
                closed->info().hash,
                0,
                ExtendedPosition{closed->info().txHash},
                NetClock::time_point{},
                NetClock::time_point{},
                env.app().getValidatorKeys().nodeID};
            ce.attachExportSignatures(prop, proposal);
            return prop.exportsignatures_size();
        };

        auto& ce = env.app().getConsensusExtensions();
        ce.setExportEnabledThisRound(true);
        ce.cacheUNLReport(env.app().getLedgerMaster().getClosedLedger());
        BEAST_EXPECT(!ce.activeValidatorView()->fromUNLReport);

        submitOpenExport(1);
        BEAST_EXPECT(attach(ce) == 0);

        auto const localPK = env.app().getValidationPublicKey();
        BEAST_EXPECT(localPK);
        if (!localPK)
            return;

        seedUNLReportLedger(env, {*localPK});
        ce.cacheUNLReport(env.app().getLedgerMaster().getClosedLedger());
        BEAST_EXPECT(ce.activeValidatorView()->fromUNLReport);
        BEAST_EXPECT(ce.activeValidatorView()->size() == 1);

        submitOpenExport(2);
        BEAST_EXPECT(attach(ce) == 1);

        std::vector<PublicKey> activeKeys{*localPK};
        while (activeKeys.size() <= STTx::maxMultiSigners())
            activeKeys.push_back(randomKeyPair(KeyType::secp256k1).first);

        seedUNLReportLedger(env, activeKeys);
        ce.cacheUNLReport(env.app().getLedgerMaster().getClosedLedger());
        BEAST_EXPECT(ce.activeValidatorView()->fromUNLReport);
        BEAST_EXPECT(
            ce.activeValidatorView()->size() > STTx::maxMultiSigners());

        submitOpenExport(3);
        BEAST_EXPECT(attach(ce) == 0);
    }

    void
    testExportNetworkApplyUsesProvidedWitness(FeatureBitset features)
    {
        testcase("ttEXPORT network apply uses provided witness");

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

        auto const& valPK = valKeys.keys->publicKey;
        auto const& valSK = valKeys.keys->secretKey;
        seedUNLReportLedger(env, {valPK});
        forceNonStandalone(env.app());
        BEAST_EXPECT(!env.app().config().standalone());

        auto const seq = env.current()->seq();
        auto const ticketSeq = std::uint32_t{1};
        auto const lls = seq + ExportLimits::maxRetryLedgers;
        auto innerObj = buildExportedPayment(
            alice.id(), carol.id(), seq + 1, lls, ticketSeq);
        auto const innerTx = makeSTTx(innerObj);
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
        BEAST_EXPECT(view->fromUNLReport);
        auto const originalSig =
            ExportResultBuilder::signExportedTxn(innerTx, valPK, valSK);
        auto const applySeq = env.closed()->seq() + 1;

        ExportResultBuilder::SignatureSnapshot expectedSigs;
        expectedSigs.emplace(valPK, originalSig);
        auto const exportSignatureWitnesses =
            makeExportSignatureWitnesses(txHash, expectedSigs, applySeq);
        ApplyOptions const applyOptions{
            &exportSignatureWitnesses,
            ApplyOptions::ExportWitnessMembership::TrustConsensusMaterialized,
            false,
            nullptr};
        auto const expectedIntentHash =
            ExportResultBuilder::exportIntentHash(innerTx);

        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto next = std::make_shared<Ledger>(
            *parent, env.app().timeKeeper().closeTime());
        OpenView accum(&*next);
        auto const result = ripple::apply(
            env.app(), accum, *exportTx, tapNONE, env.journal, applyOptions);
        BEAST_EXPECT(result.ter == tesSUCCESS);
        BEAST_EXPECT(result.applied);
        accum.apply(*next);

        auto const st = next->read(keylet::shadowTicket(alice.id(), ticketSeq));
        BEAST_EXPECT(st);
        if (st)
            BEAST_EXPECT(st->getFieldH256(sfDigest) == expectedIntentHash);
    }

    void
    testExportNetworkRetriesOversizedActiveView(FeatureBitset features)
    {
        testcase("ttEXPORT retries above target signer cap");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();

        std::vector<std::pair<PublicKey, SecretKey>> validators;
        std::vector<PublicKey> activeKeys;
        validators.reserve(STTx::maxMultiSigners() + 5);
        activeKeys.reserve(STTx::maxMultiSigners() + 5);
        while (validators.size() < STTx::maxMultiSigners() + 5)
        {
            auto keys = randomKeyPair(KeyType::secp256k1);
            activeKeys.push_back(keys.first);
            validators.push_back(std::move(keys));
        }

        seedUNLReportLedger(env, activeKeys);
        forceNonStandalone(env.app());
        BEAST_EXPECT(!env.app().config().standalone());

        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto const applySeq = parent->seq() + 1;
        auto const ticketSeq = std::uint32_t{1};
        auto innerObj = buildExportedPayment(
            alice.id(), carol.id(), applySeq, applySeq + 5, ticketSeq);
        auto const innerTx = makeSTTx(innerObj);
        auto jt = makeExportJTx(env, alice, innerObj, applySeq + 5);
        auto const exportTx = jt.stx;
        BEAST_EXPECT(exportTx);
        if (!exportTx)
            return;
        auto const txHash = exportTx->getTransactionID();

        ExportResultBuilder::SignatureSnapshot signatures;
        for (auto const& [pk, sk] : validators)
            signatures.emplace(
                pk, ExportResultBuilder::signExportedTxn(innerTx, pk, sk));

        BEAST_EXPECT(signatures.size() > STTx::maxMultiSigners());
        auto const exportSignatureWitnesses =
            makeExportSignatureWitnesses(txHash, signatures, applySeq);
        ApplyOptions const applyOptions{
            &exportSignatureWitnesses,
            ApplyOptions::ExportWitnessMembership::TrustConsensusMaterialized,
            false,
            nullptr};

        auto next = std::make_shared<Ledger>(
            *parent, env.app().timeKeeper().closeTime());
        OpenView accum(&*next);
        auto const result = ripple::apply(
            env.app(), accum, *exportTx, tapNONE, env.journal, applyOptions);
        BEAST_EXPECT(result.ter == terRETRY_EXPORT);
        BEAST_EXPECT(!result.applied);

        auto const st = next->read(keylet::shadowTicket(alice.id(), ticketSeq));
        BEAST_EXPECT(!st);
    }

    void
    testExportShadowTicketInsufficientReserve(FeatureBitset features)
    {
        testcase("ttEXPORT shadow ticket requires owner reserve");

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

        auto const& valPK = valKeys.keys->publicKey;
        auto const& valSK = valKeys.keys->secretKey;
        seedUNLReportLedger(env, {valPK});
        forceNonStandalone(env.app());

        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto const applySeq = parent->seq() + 1;
        auto const ticketSeq = std::uint32_t{1};
        auto innerObj = buildExportedPayment(
            alice.id(), carol.id(), applySeq, applySeq + 5, ticketSeq);
        auto const innerTx = makeSTTx(innerObj);
        auto jt = makeExportJTx(env, alice, innerObj, applySeq + 5);
        auto const exportTx = jt.stx;
        BEAST_EXPECT(exportTx);
        if (!exportTx)
            return;
        auto const txHash = exportTx->getTransactionID();

        auto const sig =
            ExportResultBuilder::signExportedTxn(innerTx, valPK, valSK);
        ExportResultBuilder::SignatureSnapshot signatures;
        signatures.emplace(valPK, sig);
        auto const exportSignatureWitnesses =
            makeExportSignatureWitnesses(txHash, signatures, applySeq);
        ApplyOptions const applyOptions{
            &exportSignatureWitnesses,
            ApplyOptions::ExportWitnessMembership::TrustConsensusMaterialized,
            false,
            nullptr};

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
        auto const result = ripple::apply(
            env.app(), accum, *exportTx, tapNONE, env.journal, applyOptions);
        BEAST_EXPECT(result.ter == tecINSUFFICIENT_RESERVE);

        BEAST_EXPECT(!accum.read(keylet::shadowTicket(alice.id(), ticketSeq)));
    }

    void
    testExportHistoricalReplayIgnoresCurrentManifestMap(FeatureBitset features)
    {
        testcase(
            "ttEXPORT historical replay does not require current manifest");

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

        seedUNLReportLedger(env, {valKeys.keys->publicKey});
        forceNonStandalone(env.app());
        BEAST_EXPECT(!env.app().config().standalone());

        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto const applySeq = parent->seq() + 1;
        auto const ticketSeq = std::uint32_t{1};
        auto innerObj = buildExportedPayment(
            alice.id(), carol.id(), applySeq, applySeq + 5, ticketSeq);
        auto const innerTx = makeSTTx(innerObj);
        auto jt = makeExportJTx(env, alice, innerObj, applySeq + 5);
        auto const exportTx = jt.stx;
        BEAST_EXPECT(exportTx);
        if (!exportTx)
            return;
        auto const txHash = exportTx->getTransactionID();

        auto const oldSigningKey = randomKeyPair(KeyType::secp256k1);
        auto const sig = ExportResultBuilder::signExportedTxn(
            innerTx, oldSigningKey.first, oldSigningKey.second);

        ExportResultBuilder::SignatureSnapshot signatures;
        signatures.emplace(oldSigningKey.first, sig);
        auto const exportSignatureWitnesses =
            makeExportSignatureWitnesses(txHash, signatures, applySeq);

        ApplyOptions const liveOptions{
            &exportSignatureWitnesses,
            ApplyOptions::ExportWitnessMembership::FilterLiveManifest,
            false,
            nullptr};
        {
            auto next = std::make_shared<Ledger>(
                *parent, env.app().timeKeeper().closeTime());
            OpenView accum(&*next);
            auto const result = ripple::apply(
                env.app(), accum, *exportTx, tapNONE, env.journal, liveOptions);
            BEAST_EXPECT(result.ter == terRETRY_EXPORT);
            BEAST_EXPECT(!result.applied);
        }

        auto const expectedIntentHash =
            ExportResultBuilder::exportIntentHash(innerTx);

        // A live consensus build only reaches Export::doApply after onPreBuild
        // has replaced witnesses with material from the accepted sidecar root.
        // That tx-stream witness is the membership source; the current
        // ManifestCache is not.
        ApplyOptions const consensusOptions{
            &exportSignatureWitnesses,
            ApplyOptions::ExportWitnessMembership::TrustConsensusMaterialized,
            false,
            nullptr};
        {
            auto next = std::make_shared<Ledger>(
                *parent, env.app().timeKeeper().closeTime());
            OpenView accum(&*next);
            auto const result = ripple::apply(
                env.app(),
                accum,
                *exportTx,
                tapNONE,
                env.journal,
                consensusOptions);
            BEAST_EXPECT(result.ter == tesSUCCESS);
            BEAST_EXPECT(result.applied);
            accum.apply(*next);

            auto const st =
                next->read(keylet::shadowTicket(alice.id(), ticketSeq));
            BEAST_EXPECT(st);
            if (st)
                BEAST_EXPECT(st->getFieldH256(sfDigest) == expectedIntentHash);
        }

        ApplyOptions const missingParentReplayOptions{
            &exportSignatureWitnesses,
            ApplyOptions::ExportWitnessMembership::TrustHistoricalReplay,
            true,
            nullptr};
        {
            auto next = std::make_shared<Ledger>(
                *parent, env.app().timeKeeper().closeTime());
            OpenView accum(&*next);
            auto const result = ripple::apply(
                env.app(),
                accum,
                *exportTx,
                tapNONE,
                env.journal,
                missingParentReplayOptions);
            BEAST_EXPECT(result.ter == terRETRY_EXPORT);
            BEAST_EXPECT(!result.applied);
        }

        // Historical replay reconstructs an already-validated ledger. The
        // persisted witness supplies the historical signing-key membership;
        // current ManifestCache may no longer know a rotated signing key. The
        // replay parent must be threaded explicitly so apply never consults
        // process-global ledger state for the validator view.
        ApplyOptions const replayOptions{
            &exportSignatureWitnesses,
            ApplyOptions::ExportWitnessMembership::TrustHistoricalReplay,
            true,
            parent};

        auto replayed = std::make_shared<Ledger>(
            *parent, env.app().timeKeeper().closeTime());
        OpenView accum(&*replayed);
        auto const replayResult = ripple::apply(
            env.app(), accum, *exportTx, tapNONE, env.journal, replayOptions);
        BEAST_EXPECT(replayResult.ter == tesSUCCESS);
        BEAST_EXPECT(replayResult.applied);
        accum.apply(*replayed);

        auto const st =
            replayed->read(keylet::shadowTicket(alice.id(), ticketSeq));
        BEAST_EXPECT(st);
        if (st)
            BEAST_EXPECT(st->getFieldH256(sfDigest) == expectedIntentHash);
    }

    void
    testBuildLedgerReplayPrescansExportWitness(FeatureBitset features)
    {
        testcase("BuildLedger replay pre-scans persisted export witness");

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

        seedUNLReportLedger(env, {valKeys.keys->publicKey});
        forceNonStandalone(env.app());
        BEAST_EXPECT(!env.app().config().standalone());

        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto const applySeq = parent->seq() + 1;
        auto const ticketSeq = std::uint32_t{1};
        auto innerObj = buildExportedPayment(
            alice.id(), carol.id(), applySeq, applySeq + 5, ticketSeq);
        auto const innerTx = makeSTTx(innerObj);
        auto jt = makeExportJTx(env, alice, innerObj, applySeq + 5);
        auto const exportTx = jt.stx;
        BEAST_EXPECT(exportTx);
        if (!exportTx)
            return;
        auto const txHash = exportTx->getTransactionID();

        ExportResultBuilder::SignatureSnapshot signatures;
        signatures.emplace(
            valKeys.keys->publicKey,
            ExportResultBuilder::signExportedTxn(
                innerTx, valKeys.keys->publicKey, valKeys.keys->secretKey));
        auto const witnessTx = std::make_shared<STTx const>(
            ExportResultBuilder::buildSignatureWitness(
                txHash, signatures, applySeq));

        CanonicalTXSet txns{parent->info().hash};
        txns.insert(exportTx);
        txns.insert(witnessTx);
        std::set<TxID> failed;

        auto const built = buildLedger(
            parent,
            env.app().timeKeeper().closeTime(),
            true,
            parent->info().closeTimeResolution,
            env.app(),
            txns,
            failed,
            env.journal);
        BEAST_EXPECT(txns.empty());
        BEAST_EXPECT(failed.empty());

        auto const expectedIntentHash =
            ExportResultBuilder::exportIntentHash(innerTx);
        auto const builtTicket =
            built->read(keylet::shadowTicket(alice.id(), ticketSeq));
        BEAST_EXPECT(builtTicket);
        if (builtTicket)
            BEAST_EXPECT(
                builtTicket->getFieldH256(sfDigest) == expectedIntentHash);

        auto const replayed = buildLedger(
            LedgerReplay(parent, built), tapNONE, env.app(), env.journal);
        BEAST_EXPECT(replayed->info().hash == built->info().hash);
    }

    void
    testExportNetworkRetryWithoutUNLReport(FeatureBitset features)
    {
        testcase("ttEXPORT network mode retries without UNLReport view");

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
        auto const lls = seq + ExportLimits::maxRetryLedgers;
        auto innerObj = buildExportedPayment(
            alice.id(), carol.id(), seq + 1, lls, ticketSeq);
        auto jt = makeExportJTx(env, alice, innerObj, lls);
        auto const exportTx = jt.stx;
        BEAST_EXPECT(exportTx);
        if (!exportTx)
            return;
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

        BEAST_EXPECT(result.ter == terRETRY_EXPORT);
        BEAST_EXPECT(!result.applied);
        BEAST_EXPECT(!next->read(keylet::shadowTicket(alice.id(), ticketSeq)));
    }

    void
    testExportNetworkLastLedgerSequenceBoundary(FeatureBitset features)
    {
        testcase("ttEXPORT network mode LastLedgerSequence boundary");

        using namespace jtx;

        auto applyAtLastLedger = [&](bool withQuorum) {
            Env env{*this, exportTestConfig(), features};

            Account const alice{"alice"};
            Account const carol{"carol"};

            env.fund(XRP(10000), alice, carol);
            env.close();

            auto const& valKeys = env.app().getValidatorKeys();
            BEAST_EXPECT(valKeys.keys);
            if (!valKeys.keys)
                return;

            auto const& valPK = valKeys.keys->publicKey;
            auto const& valSK = valKeys.keys->secretKey;
            seedUNLReportLedger(env, {valPK});
            forceNonStandalone(env.app());
            BEAST_EXPECT(!env.app().config().standalone());

            auto const parent = env.app().getLedgerMaster().getClosedLedger();
            auto const applySeq = parent->seq() + 1;
            auto const ticketSeq =
                withQuorum ? std::uint32_t{1} : std::uint32_t{2};
            auto innerObj = buildExportedPayment(
                alice.id(), carol.id(), applySeq, applySeq, ticketSeq);
            auto const innerTx = makeSTTx(innerObj);
            auto jt = makeExportJTx(env, alice, innerObj, applySeq);
            auto const exportTx = jt.stx;
            BEAST_EXPECT(exportTx);
            if (!exportTx)
                return;
            auto const txHash = exportTx->getTransactionID();

            auto& ce = env.app().getConsensusExtensions();
            ce.setExportEnabledThisRound(true);
            ce.cacheUNLReport(parent);
            auto const view = ce.activeValidatorView();
            BEAST_EXPECT(view->fromUNLReport);
            ExportResultBuilder::SignatureWitnesses exportSignatureWitnesses;

            if (withQuorum)
            {
                auto const sig =
                    ExportResultBuilder::signExportedTxn(innerTx, valPK, valSK);
                ExportResultBuilder::SignatureSnapshot signatures;
                signatures.emplace(valPK, sig);
                exportSignatureWitnesses =
                    makeExportSignatureWitnesses(txHash, signatures, applySeq);
            }
            ApplyOptions const applyOptions{
                &exportSignatureWitnesses,
                ApplyOptions::ExportWitnessMembership::
                    TrustConsensusMaterialized,
                false,
                nullptr};

            auto next = std::make_shared<Ledger>(
                *parent, env.app().timeKeeper().closeTime());
            BEAST_EXPECT(next->seq() == applySeq);
            OpenView accum(&*next);
            auto const result = ripple::apply(
                env.app(),
                accum,
                *exportTx,
                tapNONE,
                env.journal,
                applyOptions);

            if (withQuorum)
            {
                BEAST_EXPECT(result.ter == tesSUCCESS);
                BEAST_EXPECT(result.applied);
                accum.apply(*next);
                BEAST_EXPECT(
                    next->read(keylet::shadowTicket(alice.id(), ticketSeq)));
            }
            else
            {
                BEAST_EXPECT(result.ter == tecEXPORT_EXPIRED);
                // tecEXPORT_EXPIRED is a terminal tec result: it applies to
                // consume the sequence/fee, but must not create export state.
                BEAST_EXPECT(result.applied);
                accum.apply(*next);
                BEAST_EXPECT(
                    !next->read(keylet::shadowTicket(alice.id(), ticketSeq)));
            }
        };

        // Quorum at ledger == LastLedgerSequence still succeeds; no quorum at
        // the same boundary expires cleanly instead of falling through to a
        // later tefMAX_LEDGER preclaim.
        applyAtLastLedger(true);
        applyAtLastLedger(false);
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

        auto submitExport = [&](std::uint32_t ticketSeq, TER expected) {
            auto const seq = env.current()->seq();
            auto innerObj = buildExportedPayment(
                alice.id(),
                carol.id(),
                seq + 1,
                seq + ExportLimits::maxRetryLedgers,
                ticketSeq);

            Json::Value jv;
            jv[jss::TransactionType] = jss::Export;
            jv[jss::Account] = alice.human();
            jv[jss::LastLedgerSequence] = seq + ExportLimits::maxRetryLedgers;
            jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

            env(jv, fee(XRP(1)), ter(expected));
        };

        for (std::uint32_t i = 1; i <= ExportLimits::maxPendingExports; ++i)
            submitExport(i, tesSUCCESS);

        submitExport(ExportLimits::maxPendingExports + 1, tecDIR_FULL);
    }

    void
    testShadowTicketLimit(FeatureBitset features)
    {
        testcase("shadow ticket pending export limit");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();

        auto submitClosedExport = [&](std::uint32_t ticketSeq,
                                      TER expected,
                                      bool expectShadow) {
            auto const seq = env.current()->seq();
            auto innerObj = buildExportedPayment(
                alice.id(),
                carol.id(),
                seq + 1,
                seq + ExportLimits::maxRetryLedgers,
                ticketSeq);

            Json::Value jv;
            jv[jss::TransactionType] = jss::Export;
            jv[jss::Account] = alice.human();
            jv[jss::LastLedgerSequence] = seq + ExportLimits::maxRetryLedgers;
            jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

            env(jv, fee(XRP(1)), ter(tesSUCCESS));
            auto const meta = env.meta();
            BEAST_EXPECT(meta);
            BEAST_EXPECT(
                (*meta)[sfTransactionResult] ==
                static_cast<std::uint8_t>(TERtoInt(expected)));

            auto const shadow =
                env.le(keylet::shadowTicket(alice.id(), ticketSeq));
            BEAST_EXPECT(expectShadow == static_cast<bool>(shadow));
            env.close();
        };

        for (std::uint32_t i = 1; i <= ExportLimits::maxPendingExports; ++i)
            submitClosedExport(i, tesSUCCESS, true);

        submitClosedExport(1, tecDUPLICATE, true);
        submitClosedExport(
            ExportLimits::maxPendingExports + 1, tecDIR_FULL, false);
    }

    void
    testShadowTicketLifecycle(FeatureBitset features)
    {
        testcase("Shadow ticket lifecycle");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();

        auto const seq = env.current()->seq();

        std::uint32_t constexpr ticketSeq = 42;
        auto innerObj = buildExportedPayment(
            alice.id(),
            carol.id(),
            seq + 1,
            seq + ExportLimits::maxRetryLedgers,
            ticketSeq);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[jss::LastLedgerSequence] = seq + ExportLimits::maxRetryLedgers;
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

        env(jv, fee(XRP(1)), ter(tesSUCCESS));
        auto const meta = env.meta();
        BEAST_EXPECT(meta);
        BEAST_EXPECT(
            (*meta)[sfTransactionResult] ==
            static_cast<std::uint8_t>(TERtoInt(tesSUCCESS)));

        auto const shadow = env.le(keylet::shadowTicket(alice.id(), ticketSeq));
        BEAST_EXPECT(shadow);
        if (shadow)
        {
            BEAST_EXPECT(shadow->getAccountID(sfAccount) == alice.id());
            BEAST_EXPECT(shadow->getFieldU32(sfTicketSequence) == ticketSeq);
            BEAST_EXPECT(shadow->isFieldPresent(sfDigest));
            BEAST_EXPECT(
                shadow->getFieldH256(sfDigest) ==
                ExportResultBuilder::exportIntentHash(makeSTTx(innerObj)));
        }

        env.close();
    }

    void
    testCancelShadowTicketViaTxn(FeatureBitset features)
    {
        testcase("ttEXPORT cancels shadow ticket via sfCancelTicketSequence");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};
        env.fund(XRP(10000), alice, carol);
        env.close();

        // Cancel non-existent ticket → tecNO_ENTRY
        Json::Value jvCancel;
        jvCancel[jss::TransactionType] = jss::Export;
        jvCancel[jss::Account] = alice.human();
        jvCancel[sfCancelTicketSequence.jsonName] = 42;

        env(jvCancel, fee(XRP(1)), ter(tecNO_ENTRY));
        env.close();

        std::uint32_t constexpr ticketSeq = 42;
        auto const seq = env.current()->seq();
        auto innerObj = buildExportedPayment(
            alice.id(),
            carol.id(),
            seq + 1,
            seq + ExportLimits::maxRetryLedgers,
            ticketSeq);

        Json::Value jvExport;
        jvExport[jss::TransactionType] = jss::Export;
        jvExport[jss::Account] = alice.human();
        jvExport[jss::LastLedgerSequence] = seq + ExportLimits::maxRetryLedgers;
        jvExport[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

        env(jvExport, fee(XRP(1)), ter(tesSUCCESS));
        auto const exportMeta = env.meta();
        BEAST_EXPECT(exportMeta);
        BEAST_EXPECT(
            (*exportMeta)[sfTransactionResult] ==
            static_cast<std::uint8_t>(TERtoInt(tesSUCCESS)));
        BEAST_EXPECT(env.le(keylet::shadowTicket(alice.id(), ticketSeq)));

        Json::Value jvCancelExisting;
        jvCancelExisting[jss::TransactionType] = jss::Export;
        jvCancelExisting[jss::Account] = alice.human();
        jvCancelExisting[sfCancelTicketSequence.jsonName] = ticketSeq;

        env(jvCancelExisting, fee(XRP(1)), ter(tesSUCCESS));
        auto const cancelMeta = env.meta();
        BEAST_EXPECT(cancelMeta);
        BEAST_EXPECT(
            (*cancelMeta)[sfTransactionResult] ==
            static_cast<std::uint8_t>(TERtoInt(tesSUCCESS)));
        BEAST_EXPECT(!env.le(keylet::shadowTicket(alice.id(), ticketSeq)));

        env.close();
    }

    void
    testExportRejectsNoTicketSequence(FeatureBitset features)
    {
        testcase("ttEXPORT rejects export without TicketSequence");

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
            seq + ExportLimits::maxRetryLedgers,
            std::nullopt);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[jss::LastLedgerSequence] = seq + ExportLimits::maxRetryLedgers;
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
            seq + ExportLimits::maxRetryLedgers);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

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
            seq + ExportLimits::maxRetryLedgers);
        innerObj.setFieldVL(sfSigningPubKey, alice.pk().slice());

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[jss::LastLedgerSequence] = seq + ExportLimits::maxRetryLedgers;
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

        env(jv, fee(XRP(1)), ter(temMALFORMED));
    }

    void
    testExportRejectsLongRetryWindow(FeatureBitset features)
    {
        testcase("ttEXPORT rejects LastLedgerSequence beyond retry cap");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();

        auto const seq = env.current()->seq();
        auto const lls = seq + ExportLimits::maxRetryLedgers + 1;
        auto innerObj =
            buildExportedPayment(alice.id(), carol.id(), seq + 1, lls);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[jss::LastLedgerSequence] = lls;
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

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
        //      shadow ticket + metadata pointing to the signature witness
        //   2. XRPL:  submit the multisigned blob raw (alice's
        //      SignerList on XRPL contains the Xahau validator keys)
        //   3. Xahau: build XPOP from execution, import it back →
        //      shadow ticket consumed
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

        xahau(
            import::import(alice, callback.xpopJson),
            fee(feeDrops * 10),
            ter(tesSUCCESS));
        xahau.close();

        // Shadow ticket should be consumed after import.
        BEAST_EXPECT(!xahau.current()->exists(
            keylet::shadowTicket(alice.id(), callback.ticketSeq)));
        if (callback.vlInfo)
        {
            BEAST_EXPECT(
                importVLSequence(xahau, callback.vlInfo->second) ==
                callback.vlInfo->first);
        }
    }

    void
    testExportImportWaitsForShadowTicket(FeatureBitset features)
    {
        testcase("Export callback waits for shadow ticket");

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
        cancel[sfCancelTicketSequence.jsonName] = callback.ticketSeq;
        xahau(cancel, fee(XRP(1)), ter(tesSUCCESS));
        xahau.close();
        BEAST_EXPECT(!xahau.current()->exists(
            keylet::shadowTicket(alice.id(), callback.ticketSeq)));

        auto const importFee = xahau.current()->fees().base * 10;
        xahau(
            import::import(alice, callback.xpopJson),
            fee(importFee),
            ter(telSHADOW_TICKET_REQUIRED));

        Json::Value rearm;
        rearm[jss::TransactionType] = jss::Export;
        rearm[jss::Account] = alice.human();
        rearm[jss::LastLedgerSequence] =
            xahau.current()->seq() + ExportLimits::maxRetryLedgers;
        rearm[sfExportedTxn.jsonName] = callback.exportedTxnJson;
        xahau(rearm, fee(XRP(1)), ter(tesSUCCESS));
        xahau.close();
        BEAST_EXPECT(xahau.current()->exists(
            keylet::shadowTicket(alice.id(), callback.ticketSeq)));

        xahau(
            import::import(alice, callback.xpopJson),
            fee(importFee),
            ter(tesSUCCESS));
        xahau.close();
        BEAST_EXPECT(!xahau.current()->exists(
            keylet::shadowTicket(alice.id(), callback.ticketSeq)));
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
            keylet::shadowTicket(alice.id(), callback.ticketSeq)));
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
        testXportEmissionLimit(allWithExport);

        // ttEXPORT transactor tests
        testExportTxnOpenLedger(allWithExport);
        testExportNetworkRetryWithoutQuorum(allWithExport);
        testExportNetworkApplyUsesProvidedWitness(allWithExport);
        testExportNetworkRetriesOversizedActiveView(allWithExport);
        testExportShadowTicketInsufficientReserve(allWithExport);
        testExportHistoricalReplayIgnoresCurrentManifestMap(allWithExport);
        testBuildLedgerReplayPrescansExportWitness(allWithExport);
        testExportNetworkRetryWithoutUNLReport(allWithExport);
        testExportNetworkLastLedgerSequenceBoundary(allWithExport);
        testOpenLedgerExportLimit(allWithExport);
        testExportRejectsNoTicketSequence(allWithExport);
        testExportRejectsMissingLastLedgerSequence(allWithExport);
        testExportRejectsSignedInnerTransaction(allWithExport);
        testExportRejectsLongRetryWindow(allWithExport);
        testExportRejectsMalformed(allWithExport);

    }
};

BEAST_DEFINE_TESTSUITE(Export, app, ripple);

}  // namespace test
}  // namespace ripple
