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
#include <xrpld/app/ledger/LedgerMaster.h>
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
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/jss.h>

#include <map>

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

        // Verify hook fired successfully with exactly 1 emission.
        {
            auto const m = env.meta();
            BEAST_EXPECT(m);
            BEAST_EXPECT(m->isFieldPresent(sfHookExecutions));

            auto const& execs = m->getFieldArray(sfHookExecutions);
            BEAST_EXPECT(execs.size() == 1);

            // result=3 is ExitType::ACCEPT
            BEAST_EXPECT(execs[0].getFieldU8(sfHookResult) == 3);
            BEAST_EXPECT(execs[0].getFieldU16(sfHookEmitCount) == 1);
            BEAST_EXPECT(execs[0].getFieldU64(sfHookReturnCode) == 0);

            // Emissions metadata should be present.
            BEAST_EXPECT(m->isFieldPresent(sfHookEmissions));
            BEAST_EXPECT(m->getFieldArray(sfHookEmissions).size() == 1);

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

        // The emitted ttEXPORT should now appear in the closed ledger.
        {
            auto const ledger = env.closed();
            int txcount = 0;
            for (auto const& [stx, meta] : ledger->txs)
            {
                BEAST_EXPECT(stx->getTxnType() == ttEXPORT);
                BEAST_EXPECT(stx->isFieldPresent(sfEmitDetails));
                txcount++;
            }
            BEAST_EXPECT(txcount == 1);
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
        auto innerObj =
            buildExportedPayment(alice.id(), carol.id(), seq + 1, seq + 5);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
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
        ConfigVals cfg;
        cfg.noExportSig = true;
        env.app().getRuntimeConfig().setConfig("*", cfg);

        auto const seq = env.current()->seq();
        auto const ticketSeq = std::uint32_t{1};
        auto const lls = seq + 5;
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
    testExportNetworkApplyUsesAgreedSidecar(FeatureBitset features)
    {
        testcase("ttEXPORT network apply uses agreed export sidecar");

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
        auto const lls = seq + 5;
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
        ce.cacheConsensusTxSet(makeRCLTxSet(env.app(), {exportTx}));

        auto const originalSig =
            ExportResultBuilder::signExportedTxn(innerTx, valPK, valSK);
        auto const applySeq = env.closed()->seq() + 1;
        ce.exportSigCollector().addVerifiedSignature(
            txHash, valPK, originalSig, applySeq);
        auto const agreedHash = ce.buildExportSigSet(applySeq);
        BEAST_EXPECT(ce.isSidecarSet(agreedHash));

        // Simulate an asynchronous collector mutation after sidecar agreement.
        // A bad revert to the live collector at apply would assemble this late
        // signature and write a different shadow-ticket hash.
        std::uint8_t const lateBytes[] = {9, 8, 7};
        Buffer const lateSig{lateBytes, sizeof(lateBytes)};
        ce.exportSigCollector().addVerifiedSignature(
            txHash, valPK, lateSig, applySeq);

        ExportResultBuilder::SignatureSnapshot expectedSigs;
        expectedSigs.emplace(valPK, originalSig);
        auto const expectedSignedTxHash =
            ExportResultBuilder::assemble(
                innerTx, expectedSigs, applySeq, txHash)
                .signedTxHash;

        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto next = std::make_shared<Ledger>(
            *parent, env.app().timeKeeper().closeTime());
        OpenView accum(&*next);
        auto const result =
            ripple::apply(env.app(), accum, *exportTx, tapNONE, env.journal);
        BEAST_EXPECT(result.ter == tesSUCCESS);
        BEAST_EXPECT(result.applied);
        accum.apply(*next);

        auto const st = next->read(keylet::shadowTicket(alice.id(), ticketSeq));
        BEAST_EXPECT(st);
        if (st)
            BEAST_EXPECT(
                st->getFieldH256(sfTransactionHash) == expectedSignedTxHash);
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

        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;

        auto const& valPK = valKeys.keys->publicKey;
        auto const& valSK = valKeys.keys->secretKey;
        auto const seq = env.current()->seq();
        auto const ticketSeq = std::uint32_t{1};
        auto const lls = seq + 5;
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
        BEAST_EXPECT(!view->fromUNLReport);
        ce.cacheConsensusTxSet(makeRCLTxSet(env.app(), {exportTx}));

        auto const sig =
            ExportResultBuilder::signExportedTxn(innerTx, valPK, valSK);
        auto const applySeq = env.closed()->seq() + 1;
        ce.exportSigCollector().addVerifiedSignature(
            txHash, valPK, sig, applySeq);
        auto const agreedHash = ce.buildExportSigSet(applySeq);
        BEAST_EXPECT(ce.isSidecarSet(agreedHash));

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
                alice.id(), carol.id(), seq + 1, seq + 50, ticketSeq);

            Json::Value jv;
            jv[jss::TransactionType] = jss::Export;
            jv[jss::Account] = alice.human();
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

        auto submitClosedExport = [&](std::uint32_t ticketSeq, TER expected) {
            auto const seq = env.current()->seq();
            auto innerObj = buildExportedPayment(
                alice.id(), carol.id(), seq + 1, seq + 50, ticketSeq);

            Json::Value jv;
            jv[jss::TransactionType] = jss::Export;
            jv[jss::Account] = alice.human();
            jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

            env(jv, fee(XRP(1)), ter(tesSUCCESS));
            auto const meta = env.meta();
            BEAST_EXPECT(meta);
            BEAST_EXPECT(
                (*meta)[sfTransactionResult] ==
                static_cast<std::uint8_t>(TERtoInt(expected)));

            auto const shadow =
                env.le(keylet::shadowTicket(alice.id(), ticketSeq));
            BEAST_EXPECT((expected == tesSUCCESS) == static_cast<bool>(shadow));
            env.close();
        };

        for (std::uint32_t i = 1; i <= ExportLimits::maxPendingExports; ++i)
            submitClosedExport(i, tesSUCCESS);

        submitClosedExport(ExportLimits::maxPendingExports + 1, tecDIR_FULL);
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

        auto innerObj =
            buildExportedPayment(alice.id(), carol.id(), seq + 1, seq + 5, 42);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

        env(jv, fee(XRP(1)), ter(tesSUCCESS));
        env.close();

        // In single-node test env, no validator sigs → no quorum →
        // shadow ticket not created (only created on quorum success).
        // The export retries or expires without creating the ticket.
    }

    void
    testCancelShadowTicketViaTxn(FeatureBitset features)
    {
        testcase("ttEXPORT cancels shadow ticket via sfCancelTicketSequence");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // Cancel non-existent ticket → tecNO_ENTRY
        Json::Value jvCancel;
        jvCancel[jss::TransactionType] = jss::Export;
        jvCancel[jss::Account] = alice.human();
        jvCancel[sfCancelTicketSequence.jsonName] = 42;

        env(jvCancel, fee(XRP(1)), ter(tecNO_ENTRY));
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
            alice.id(), carol.id(), seq + 1, seq + 5, std::nullopt);

        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
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
        //      shadow ticket + multisigned blob in metadata
        //   2. XRPL:  submit the multisigned blob raw (alice's
        //      SignerList on XRPL contains the Xahau validator keys)
        //   3. Xahau: build XPOP from execution, import it back →
        //      shadow ticket consumed
        //
        // In standalone mode, Export::doApply signs directly with
        // the node's validator keys — no consensus needed.

        auto const xpopCtx = xpop::TestXPOPContext::create(3);

        Account const alice{"alice"};
        Account const carol{"carol"};

        // ── Xahau env: export the inner tx ─────────────────────────────
        Env xahau{*this, xpopCtx.makeEnvConfig(21337), features};

        xahau.fund(XRP(10000), alice, carol);
        xahau.close();

        // Burn XRP so B2M crediting works on import.
        auto const master = Account("masterpassphrase");
        xahau(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
        xahau.close();

        // The inner tx is a payment from alice→carol on XRPL, using
        // TicketSequence (required for exports — avoids sequence jams
        // if the tx bounces on XRPL).
        //
        // OperationLimit tells Import::preflight which network this
        // tx targets (must match Xahau's NETWORK_ID).
        //
        // We'll create the matching ticket on the XRPL side later.
        std::uint32_t const ticketSeq = 2;  // alice's first ticket on XRPL

        // Build the inner tx.  Use wide ledger sequence bounds so
        // it's valid regardless of how many ledgers the XRPL env
        // needs for setup.  Fee must cover multisign overhead:
        // (1 + signerCount) * baseFee = (1+1)*10 = 20.
        STObject innerObj(sfExportedTxn);
        innerObj.setFieldU16(sfTransactionType, ttPAYMENT);
        innerObj.setFieldU32(sfFlags, tfFullyCanonicalSig);
        innerObj.setFieldU32(sfSequence, 0);
        innerObj.setFieldU32(sfTicketSequence, ticketSeq);
        innerObj.setFieldU32(sfLastLedgerSequence, 100);
        innerObj.setFieldAmount(sfAmount, XRPAmount{1000000});
        innerObj.setFieldAmount(sfFee, XRPAmount{20});
        innerObj.setFieldVL(sfSigningPubKey, Blob{});
        innerObj.setAccountID(sfAccount, alice.id());
        innerObj.setAccountID(sfDestination, carol.id());
        // Note: no sfOperationLimit needed — the export callback path
        // (sfTicketSequence present) skips the OperationLimit check
        // in Import::preflight.  OperationLimit is only required for
        // the B2M (burn-to-mint) import path.

        // Submit ttEXPORT — in standalone mode, Export::doApply
        // signs the inner tx with the node's validator keys and
        // puts the multisigned blob in sfExportResult metadata.
        Json::Value jvExport;
        jvExport[jss::TransactionType] = jss::Export;
        jvExport[jss::Account] = alice.human();
        jvExport[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

        xahau(jvExport, fee(XRP(1)), ter(tesSUCCESS));
        xahau.close();

        // Extract the multisigned blob from the Export metadata.
        auto const exportMeta = xahau.meta();
        BEAST_EXPECT(exportMeta);

        Blob multisignedBlob;
        if (exportMeta && exportMeta->isFieldPresent(sfExportResult))
        {
            auto const& result =
                exportMeta->peekAtField(sfExportResult).downcast<STObject>();
            if (result.isFieldPresent(sfExportedTxn))
            {
                // Serialize the nested object to get the raw blob
                // for submission to XRPL.
                auto const& expTxn =
                    const_cast<STObject&>(result).peekFieldObject(
                        sfExportedTxn);
                Serializer s;
                expTxn.add(s);
                multisignedBlob = s.peekData();

                log << "Xahau: ExportResult.ExportedTxn = "
                    << expTxn.getJson(JsonOptions::none).toStyledString()
                    << std::endl;
            }
        }
        log << "Xahau: multisigned blob size = " << multisignedBlob.size()
            << std::endl;
        BEAST_EXPECT(!multisignedBlob.empty());

        // Verify shadow ticket was created.
        auto const stKey = keylet::shadowTicket(alice.id(), ticketSeq);
        BEAST_EXPECT(xahau.current()->exists(stKey));

        // ── XRPL env: submit the multisigned blob ──────────────────────
        Env xrpl{*this};

        xrpl.fund(XRP(10000), alice, carol);
        xrpl.close();

        // Create a ticket on XRPL for the exported tx.
        std::uint32_t const xrplTicketSeq = xrpl.seq(alice) + 1;
        xrpl(ticket::create(alice, 1));
        xrpl.close();
        BEAST_EXPECT(xrplTicketSeq == ticketSeq);

        // Set alice's SignerList on XRPL to the Xahau validator key.
        // The validator key is a "phantom signer" — it doesn't need
        // a funded account on XRPL, just an entry in alice's SignerList.
        auto const valPK = xahau.app().getValidationPublicKey();
        auto const valSK = xahau.app().getValidationSecretKey();
        BEAST_EXPECT(valPK.has_value());

        auto const valAccountID = calcAccountID(*valPK);
        log << "XRPL: validator AccountID = " << toBase58(valAccountID)
            << std::endl;
        log << "XRPL: validator PK = " << strHex(*valPK) << std::endl;

        // Use the jtx signers() helper with a phantom Account whose
        // AccountID matches the validator key.
        // We can't use signers() directly because it takes Account
        // objects.  Build the JSON manually instead.
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

        // Smoke test: submit a simple multisigned noop to verify
        // the SignerList works before trying the exported payment.
        {
            // Build a noop AccountSet, multisigned by the validator.
            STObject noop(sfGeneric);
            noop.setFieldU16(sfTransactionType, ttACCOUNT_SET);
            noop.setFieldU32(sfFlags, tfFullyCanonicalSig);
            noop.setFieldU32(sfSequence, xrpl.seq(alice));
            noop.setFieldAmount(sfFee, XRPAmount{20});  // (1+1)*base
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

        // Now submit the actual multisigned export blob.
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

        // Build XPOP from the XRPL ledger.
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

        // ── Back to Xahau: import the XPOP ────────────────────────────
        auto const feeDrops = xahau.current()->fees().base;

        xahau(
            import::import(alice, xpopJson),
            fee(feeDrops * 10),
            ter(tesSUCCESS));
        xahau.close();

        // Shadow ticket should be consumed after import.
        BEAST_EXPECT(!xahau.current()->exists(stKey));
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
        testXportEmissionLimit(allWithExport);

        // ttEXPORT transactor tests
        testExportTxnOpenLedger(allWithExport);
        testExportNetworkRetryWithoutQuorum(allWithExport);
        testExportNetworkApplyUsesAgreedSidecar(allWithExport);
        testExportNetworkRetryWithoutUNLReport(allWithExport);
        testOpenLedgerExportLimit(allWithExport);
        testShadowTicketLimit(allWithExport);
        testShadowTicketLifecycle(allWithExport);
        testCancelShadowTicketViaTxn(allWithExport);
        testExportRejectsNoTicketSequence(allWithExport);
        testExportRejectsMalformed(allWithExport);

        // Round-trip test
        testExportImportRoundTrip(allWithExport);
    }
};

BEAST_DEFINE_TESTSUITE(Export, app, ripple);

}  // namespace test
}  // namespace ripple
