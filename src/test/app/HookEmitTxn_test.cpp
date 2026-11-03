//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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

#include <test/app/SetHook_wasm.h>
#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <xrpld/app/tx/detail/SetHook.h>
#include <xrpl/protocol/STParsedJSON.h>
#include <xrpl/protocol/jss.h>

namespace ripple {
namespace test {

using TestHook = std::vector<uint8_t> const&;

#define HASH_WASM(x)                                                           \
    [[maybe_unused]] uint256 const x##_hash =                                  \
        ripple::sha512Half_s(ripple::Slice(x##_wasm.data(), x##_wasm.size())); \
    [[maybe_unused]] std::string const x##_hash_str = to_string(x##_hash);     \
    [[maybe_unused]] Keylet const x##_keylet = keylet::hookDefinition(x##_hash);

class HookEmitTxn_test : public beast::unit_test::suite
{
private:
    // metadata of the last Invoke submitted by emitTxn()
    std::shared_ptr<STObject const> invokeMeta_;

    // LedgerIndex of the first affected node of kind `nodeKind` and `type`
    static std::optional<uint256>
    nodeIndex(
        STObject const& meta,
        SField const& nodeKind,
        LedgerEntryType type)
    {
        for (auto const& node : meta.getFieldArray(sfAffectedNodes))
            if (node.getFName() == nodeKind &&
                node.getFieldU16(sfLedgerEntryType) == type)
                return node.getFieldH256(sfLedgerIndex);
        return std::nullopt;
    }

    static bool
    hasNode(STObject const& meta, SField const& nodeKind, LedgerEntryType type)
    {
        return nodeIndex(meta, nodeKind, type).has_value();
    }

    // Installs the emit_tx hook on `account`. `f` may edit the HookSetObject
    // (e.g. to set HookCanEmit).
    void
    installEmitHook(
        jtx::Env& env,
        jtx::Account const& account,
        void (*f)(Json::Value&) = nullptr)
    {
        using namespace jtx;
        env(hook(account, {{hso(emit_tx_wasm, f)}}, 0),
            fee(XRP(1)),
            ter(tesSUCCESS));
        env.close();
    }

    // Emits `jv` from alice's emit_tx hook (via Invoke.Blob) and returns the
    // metadata of the emitted txn once it has been applied. Account and Fee
    // are filled in by the hook (hook_account / etxn_fee_base).
    std::shared_ptr<STObject const>
    emitTxn(jtx::Env& env, jtx::Account const& alice, Json::Value jv)
    {
        using namespace jtx;
        jv.removeMember(jss::Account);
        jv.removeMember(jss::Fee);
        jv[jss::Sequence] = 0;
        jv[jss::SigningPubKey] = "";
        STParsedJSONObject parsed("tx", jv);
        if (!BEAST_EXPECT(parsed.object))
            return nullptr;
        Serializer s;
        parsed.object->add(s);

        env(invoke::invoke(alice),
            invoke::blob(strHex(s.slice())),
            fee(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        invokeMeta_ = env.meta();
        if (!BEAST_EXPECT(invokeMeta_))
        {
            log << "invoke result: " << transHuman(env.ter()) << std::endl;
            return nullptr;
        }
        auto const& emissions = invokeMeta_->getFieldArray(sfHookEmissions);
        if (!BEAST_EXPECT(emissions.size() == 1))
            return nullptr;
        auto const emittedTxnID = emissions[0].getFieldH256(sfEmittedTxnID);
        env.close();

        auto const [txn, meta] = env.closed()->txRead(emittedTxnID);
        if (!BEAST_EXPECT(txn && meta))
            return nullptr;
        auto const result = TER::fromInt(meta->getFieldU8(sfTransactionResult));
        if (!BEAST_EXPECT(result == tesSUCCESS))
            log << "emitted txn result: " << transHuman(result) << std::endl;
        return meta;
    }

    void
    expectCreated(
        jtx::Env& env,
        jtx::Account const& alice,
        Json::Value const& jv,
        LedgerEntryType type)
    {
        if (auto const meta = emitTxn(env, alice, jv))
        {
            BEAST_EXPECT(hasNode(*meta, sfCreatedNode, type));
        }
        else
        {
            BEAST_EXPECTS(
                false,
                "Expected " + std::to_string(uint32_t(type)) +
                    " to be created, but failed to emit txn");
        }
    }

    // For entries keyed by the txn sequence: emitted txns all carry
    // Sequence 0, so seqID() keys them by TransactionID instead. Emitting
    // the same txn twice must succeed and create two distinct entries.
    void
    expectCreatedTwice(
        jtx::Env& env,
        jtx::Account const& alice,
        Json::Value const& jv,
        LedgerEntryType type)
    {
        std::optional<uint256> first, second;
        if (auto const meta = emitTxn(env, alice, jv))
            first = nodeIndex(*meta, sfCreatedNode, type);
        if (auto const meta = emitTxn(env, alice, jv))
            second = nodeIndex(*meta, sfCreatedNode, type);
        BEAST_EXPECT(first);
        BEAST_EXPECT(second);
        if (first && second)
            BEAST_EXPECT(*first != *second);
    }

    // Emitted SetHook: allowed only when HookCanEmit permits ttHOOK_SET.
    std::shared_ptr<STObject const>
    emitSetHook(FeatureBitset const& features)
    {
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();
        installEmitHook(env, alice, [](Json::Value& jv) {
            // ttHOOK_SET bit is active high (see hook::canHook)
            jv[jss::HookCanEmit] = std::string(58, '0') + "400000";
        });
        // position 0 keeps emit_tx, position 1 creates a new definition
        return emitTxn(
            env,
            alice,
            hook(alice, {{Json::objectValue, hso(makestate_wasm)}}, 0));
    }

    void
    testNFTokenOffer(FeatureBitset const& features)
    {
        testcase("NFTokenOffer");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();
        // mint before installing the hook: emit_tx rolls back any txn
        // from alice that carries no Blob
        auto const nftID = token::getNextID(env, alice, 0);
        env(token::mint(alice, 0));
        env.close();
        installEmitHook(env, alice);

        auto jv = token::createOffer(alice, nftID, XRP(1));
        jv[jss::Flags] = tfSellNFToken;
        expectCreatedTwice(env, alice, jv, ltNFTOKEN_OFFER);
    }

    void
    testCron(FeatureBitset const& features)
    {
        testcase("Cron");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();
        installEmitHook(env, alice);

        auto jv = cron::set(alice);
        jv[sfStartTime.jsonName] = 0;
        jv[sfDelaySeconds.jsonName] = 100;
        jv[sfRepeatCount.jsonName] = 1;
        expectCreated(env, alice, jv, ltCRON);
    }

    void
    testCheck(FeatureBitset const& features)
    {
        testcase("Check");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();
        installEmitHook(env, alice);
        expectCreatedTwice(
            env, alice, check::create(alice, bob, XRP(100)), ltCHECK);
    }

    void
    testHookDefinition(FeatureBitset const& features)
    {
        testcase("HookDefinition");
        if (auto const meta = emitSetHook(features))
            BEAST_EXPECT(hasNode(*meta, sfCreatedNode, ltHOOK_DEFINITION));
    }

    void
    testEmittedTxn(FeatureBitset const& features)
    {
        testcase("EmittedTxn");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();
        installEmitHook(env, alice);
        emitTxn(env, alice, pay(alice, bob, XRP(1)));
        // the EmittedTxn entry is created by the emitting (Invoke) txn
        if (BEAST_EXPECT(invokeMeta_))
            BEAST_EXPECT(hasNode(*invokeMeta_, sfCreatedNode, ltEMITTED_TXN));
    }

    void
    testHook(FeatureBitset const& features)
    {
        testcase("Hook");
        // The emitting account always already owns a Hook entry, so an
        // emitted SetHook can only modify it.
        if (auto const meta = emitSetHook(features))
            BEAST_EXPECT(hasNode(*meta, sfModifiedNode, ltHOOK));
    }

    void
    testImportVLSequence(FeatureBitset const& features)
    {
        testcase("ImportVLSequence");
        // created by Import, which needs a validly signed xpop; not
        // emittable
        pass();
    }

    void
    testNegativeUNL(FeatureBitset const& features)
    {
        testcase("NegativeUNL");
        // pseudo-transaction only
        pass();
    }

    void
    testNFTokenPage(FeatureBitset const& features)
    {
        testcase("NFTokenPage");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();
        installEmitHook(env, alice);
        expectCreated(env, alice, token::mint(alice, 0), ltNFTOKEN_PAGE);
    }

    void
    testUNLReport(FeatureBitset const& features)
    {
        testcase("UNLReport");
        // pseudo-transaction only
        pass();
    }

    void
    testSignerList(FeatureBitset const& features)
    {
        testcase("SignerList");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();
        installEmitHook(env, alice);
        expectCreated(env, alice, signers(alice, 1, {{bob, 1}}), ltSIGNER_LIST);
    }

    void
    testTicket(FeatureBitset const& features)
    {
        testcase("Ticket");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();
        installEmitHook(env, alice);
        // keyed by AccountRoot.Sequence, which CreateTicket advances itself
        expectCreatedTwice(env, alice, ticket::create(alice, 1), ltTICKET);
    }

    void
    testURIToken(FeatureBitset const& features)
    {
        testcase("URIToken");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();
        installEmitHook(env, alice);
        expectCreated(env, alice, uritoken::mint(alice, "uri"), ltURI_TOKEN);
    }

    void
    testAccountRoot(FeatureBitset const& features)
    {
        testcase("AccountRoot");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.close();
        installEmitHook(env, alice);
        expectCreated(
            env,
            alice,
            pay(alice, bob, STAmount{env.current()->fees().accountReserve(10)}),
            ltACCOUNT_ROOT);
    }

    void
    testDirectoryNode(FeatureBitset const& features)
    {
        testcase("DirectoryNode");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        Account const gw{"gw"};
        auto const USD = gw["USD"];
        env.fund(XRP(10000), alice, gw);
        env.close();
        installEmitHook(env, alice);
        // a new offer book creates its directory
        expectCreated(env, alice, offer(alice, USD(100), XRP(100)), ltDIR_NODE);
    }

    void
    testAmendments(FeatureBitset const& features)
    {
        testcase("Amendments");
        // pseudo-transaction only
        pass();
    }

    void
    testLedgerHashes(FeatureBitset const& features)
    {
        testcase("LedgerHashes");
        // ledger-internal entry
        pass();
    }

    void
    testBridge(FeatureBitset const& features)
    {
        testcase("Bridge");
        // XChainBridge is not supported on Xahau
        BEAST_EXPECT(!features[featureXChainBridge]);
    }

    void
    testOffer(FeatureBitset const& features)
    {
        testcase("Offer");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        Account const gw{"gw"};
        auto const USD = gw["USD"];
        env.fund(XRP(10000), alice, gw);
        env.close();
        installEmitHook(env, alice);
        expectCreatedTwice(
            env, alice, offer(alice, USD(100), XRP(100)), ltOFFER);
    }

    void
    testDepositPreauth(FeatureBitset const& features)
    {
        testcase("DepositPreauth");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();
        installEmitHook(env, alice);
        expectCreated(env, alice, deposit::auth(alice, bob), ltDEPOSIT_PREAUTH);
    }

    void
    testXChainOwnedClaimID(FeatureBitset const& features)
    {
        testcase("XChainOwnedClaimID");
        // XChainBridge is not supported on Xahau
        BEAST_EXPECT(!features[featureXChainBridge]);
    }

    void
    testRippleState(FeatureBitset const& features)
    {
        testcase("RippleState");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        Account const gw{"gw"};
        auto const USD = gw["USD"];
        env.fund(XRP(10000), alice, gw);
        env.close();
        installEmitHook(env, alice);
        expectCreated(env, alice, trust(alice, USD(1000)), ltRIPPLE_STATE);
    }

    void
    testFeeSettings(FeatureBitset const& features)
    {
        testcase("FeeSettings");
        // pseudo-transaction only
        pass();
    }

    void
    testXChainOwnedCreateAccountClaimID(FeatureBitset const& features)
    {
        testcase("XChainOwnedCreateAccountClaimID");
        // XChainBridge is not supported on Xahau
        BEAST_EXPECT(!features[featureXChainBridge]);
    }

    void
    testEscrow(FeatureBitset const& features)
    {
        testcase("Escrow");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();
        installEmitHook(env, alice);

        Json::Value jv;
        jv[jss::TransactionType] = jss::EscrowCreate;
        jv[jss::Account] = alice.human();
        jv[jss::Destination] = bob.human();
        jv[jss::Amount] = XRP(100).value().getJson(JsonOptions::none);
        jv[sfFinishAfter.jsonName] = env.now().time_since_epoch().count() + 100;
        expectCreatedTwice(env, alice, jv, ltESCROW);
    }

    void
    testHookState(FeatureBitset const& features)
    {
        testcase("HookState");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();
        installEmitHook(env, alice);
        // bob's hook runs as a strong TSH of the emitted Invoke and writes
        // state
        env(hook(bob, {{hso(makestate_wasm)}}, 0), fee(XRP(1)));
        env.close();

        auto jv = invoke::invoke(alice);
        jv[jss::Destination] = bob.human();
        expectCreated(env, alice, jv, ltHOOK_STATE);
    }

    void
    testPayChannel(FeatureBitset const& features)
    {
        testcase("PayChannel");
        using namespace jtx;
        using namespace std::chrono_literals;
        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();
        installEmitHook(env, alice);
        expectCreatedTwice(
            env,
            alice,
            paychan::create(alice, bob, XRP(100), 100s, alice.pk()),
            ltPAYCHAN);
    }

    void
    testAMM(FeatureBitset const& features)
    {
        testcase("AMM");
        using namespace jtx;
        Env env{*this, features};
        Account const alice{"alice"};
        Account const gw{"gw"};
        auto const USD = gw["USD"];
        env.fund(XRP(10000), alice, gw);
        env.close();
        env(trust(alice, USD(1000)));
        env(pay(gw, alice, USD(500)));
        env.close();
        installEmitHook(env, alice);

        Json::Value jv;
        jv[jss::TransactionType] = jss::AMMCreate;
        jv[jss::Account] = alice.human();
        jv[jss::Amount] = XRP(100).value().getJson(JsonOptions::none);
        jv[jss::Amount2] = USD(100).value().getJson(JsonOptions::none);
        jv[jss::TradingFee] = 0;
        expectCreated(env, alice, jv, ltAMM);
    }

    void
    testOracle(FeatureBitset const& features)
    {
        testcase("Oracle");
        using namespace jtx;
        using namespace std::chrono;
        Env env{*this, features};
        // OracleSet needs closeTime > maxLastUpdateTimeDelta
        env.close(seconds(10'000));
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();
        installEmitHook(env, alice);

        Json::Value jv;
        jv[jss::TransactionType] = jss::OracleSet;
        jv[jss::Account] = alice.human();
        jv[jss::OracleDocumentID] = 1;
        jv[jss::Provider] = strHex(std::string("provider"));
        jv[jss::AssetClass] = strHex(std::string("currency"));
        jv[jss::LastUpdateTime] = static_cast<Json::UInt>(
            duration_cast<seconds>(env.now().time_since_epoch()).count() +
            epoch_offset.count());
        Json::Value price;
        price[jss::BaseAsset] = "XRP";
        price[jss::QuoteAsset] = "USD";
        price[jss::AssetPrice] = 740;
        price[jss::Scale] = 1;
        jv[jss::PriceDataSeries][0u][jss::PriceData] = price;
        expectCreated(env, alice, jv, ltORACLE);
    }

    void
    testMPTokenIssuance(FeatureBitset const& features)
    {
        testcase("MPTokenIssuance");
        // MPTokensV1 is not supported on Xahau
        BEAST_EXPECT(!features[featureMPTokensV1]);
    }

    void
    testMPToken(FeatureBitset const& features)
    {
        testcase("MPToken");
        // MPTokensV1 is not supported on Xahau
        BEAST_EXPECT(!features[featureMPTokensV1]);
    }

    void
    testCredential(FeatureBitset const& features)
    {
        testcase("Credential");
        // Credentials is not supported on Xahau
        BEAST_EXPECT(!features[featureCredentials]);
    }

    void
    testPermissionedDomain(FeatureBitset const& features)
    {
        testcase("PermissionedDomain");
        // PermissionedDomains is not supported on Xahau
        BEAST_EXPECT(!features[featurePermissionedDomains]);
    }

    void
    testDID(FeatureBitset const& features)
    {
        testcase("DID");
        // DID is not supported on Xahau
        BEAST_EXPECT(!features[featureDID]);
    }

    void
    testCreateLedgerEntry(FeatureBitset const& features)
    {
#pragma push_macro("LEDGER_ENTRY")
#pragma push_macro("EXPAND")
#undef LEDGER_ENTRY
#undef EXPAND

#define LEDGER_ENTRY(tag, value, name, ...) test##name(features);

#include <xrpl/protocol/detail/ledger_entries.macro>

#undef LEDGER_ENTRY
#undef EXPAND
#pragma pop_macro("EXPAND")
#pragma pop_macro("LEDGER_ENTRY")
    }

private:
    // This hook is used in HookEmitTxn_test.cpp
    TestHook emit_tx_wasm = wasm[
        R"[test.hook](
          #include <stdint.h>
          extern int32_t _g(uint32_t id, uint32_t maxiter);
          extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
          extern int64_t rollback(uint32_t read_ptr, uint32_t read_len,
                                  int64_t error_code);
          extern int64_t emit(uint32_t write_ptr, uint32_t write_len, uint32_t read_ptr,
                              uint32_t read_len);
          extern int64_t hook_account(uint32_t write_ptr, uint32_t write_len);
          extern int64_t etxn_reserve(uint32_t);
          extern int64_t etxn_fee_base(uint32_t read_ptr, uint32_t read_len);
          extern int64_t etxn_details(uint32_t write_ptr, uint32_t write_len);
          extern int64_t ledger_seq(void);
          extern int64_t otxn_field(uint32_t write_ptr, uint32_t write_len,
                                    uint32_t field_id);
          extern int64_t trace(uint32_t write_ptr, uint32_t write_len, uint32_t read_ptr,
                               uint32_t read_len, int32_t as_hex);
          #define sfBlob ((7U << 16U) + 26U)
          
          #define SBUF(x) (uint32_t) x, sizeof(x)
          
          uint8_t blob[1000];
          
          #define FLIP_ENDIAN_32(value)                                                  \
            (uint32_t)(((value & 0xFFU) << 24) | ((value & 0xFF00U) << 8) |              \
                       ((value & 0xFF0000U) >> 8) | ((value & 0xFF000000U) >> 24))
          
          #define SET_UINT32(ptr, value) *((uint32_t *)(ptr)) = FLIP_ENDIAN_32(value);
          
          #define SET_NATIVE_AMOUNT(ptr, amount)                                         \
            do {                                                                         \
              uint8_t *b = (ptr);                                                        \
              *b++ = 0b01000000 + ((amount >> 56) & 0b00111111);                         \
              *b++ = (amount >> 48) & 0xFFU;                                             \
              *b++ = (amount >> 40) & 0xFFU;                                             \
              *b++ = (amount >> 32) & 0xFFU;                                             \
              *b++ = (amount >> 24) & 0xFFU;                                             \
              *b++ = (amount >> 16) & 0xFFU;                                             \
              *b++ = (amount >> 8) & 0xFFU;                                              \
              *b++ = (amount >> 0) & 0xFFU;                                              \
            } while (0)
          
          #define PREPARE_TXN(ptr, len)                                                  \
            do {                                                                         \
              uint8_t *nextPtr = (uint8_t *)(ptr);                                       \
              nextPtr += len;                                                            \
              etxn_reserve(1);                                                           \
              uint32_t fls = (uint32_t)ledger_seq() + 1;                                 \
              /** firstledgersequence = ledger_seq + 1 */                                \
              *nextPtr++ = 0x20U;                                                        \
              *nextPtr++ = 0x1AU;                                                        \
              txn_len += 2;                                                              \
              SET_UINT32((uint32_t)nextPtr, fls);                                        \
              nextPtr += 4;                                                              \
              txn_len += 4;                                                              \
              /** lastledgersequence = ledger_seq + 5 */                                 \
              *nextPtr++ = 0x20U;                                                        \
              *nextPtr++ = 0x1BU;                                                        \
              txn_len += 2;                                                              \
              SET_UINT32((uint32_t)nextPtr, fls + 4);                                    \
              nextPtr += 4;                                                              \
              txn_len += 4;                                                              \
              /** account = hook_account */                                              \
              *nextPtr++ = 0x81U;                                                        \
              *nextPtr++ = 0x14U;                                                        \
              txn_len += 2;                                                              \
              hook_account((uint32_t)nextPtr, 20);                                       \
              nextPtr += 20;                                                             \
              txn_len += 20;                                                             \
              /** emit details */                                                        \
              etxn_details((uint32_t)nextPtr, 116U);                                     \
              nextPtr += 116;                                                            \
              txn_len += 116;                                                            \
              /** fee */                                                                 \
              *nextPtr++ = 0x68U;                                                        \
              *nextPtr++ = 0x40U;                                                        \
              txn_len += 1;                                                              \
              SET_NATIVE_AMOUNT((uint32_t)--nextPtr, 0);                                 \
              txn_len += 8;                                                              \
              int64_t fee = etxn_fee_base((uint32_t)ptr, txn_len);                       \
              SET_NATIVE_AMOUNT((uint32_t)nextPtr, fee);                                 \
            } while (0)
          
          int64_t hook(uint32_t reserved) {
            _g(1, 1);
          
            int64_t blob_len = otxn_field(SBUF(blob), sfBlob);
            uint64_t prefix_len = (blob[0] < 193) ? 1 : (blob[0] < 241) ? 2 : 3;
          
          #define txn_ptr (blob + prefix_len)
            uint32_t txn_len = blob_len - prefix_len;
          
            PREPARE_TXN(txn_ptr, txn_len);
            uint8_t emithash[32];
            int64_t emit_result = emit(SBUF(emithash), txn_ptr, txn_len);
            if (emit_result < 0)
              return rollback(SBUF("Emit failed."), __LINE__);
            return accept(SBUF("Emit succeeded."), __LINE__);
          }
        )[test.hook]"];
    HASH_WASM(emit_tx);

    TestHook makestate_wasm = wasm[
        R"[test.hook](
            #include <stdint.h>
            extern int32_t _g           (uint32_t id, uint32_t maxiter);
            extern int64_t accept       (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t state_set    (uint32_t read_ptr, uint32_t read_len, uint32_t kread_ptr, uint32_t kread_len);
            #define SBUF(x) x, sizeof(x)
            int64_t hook(uint32_t reserved )
            {
                _g(1,1);
                uint8_t test_key[] = "key";
                uint8_t test_value[] = "value";
                return accept(0,0, state_set(SBUF(test_value), SBUF(test_key)));
            }
        )[test.hook]"];

public:
    void
    run() override
    {
        using namespace test::jtx;
        testCreateLedgerEntry(supported_amendments());
    }
};

BEAST_DEFINE_TESTSUITE_PRIO(HookEmitTxn, app, ripple, 2);
}  // namespace test
}  // namespace ripple
