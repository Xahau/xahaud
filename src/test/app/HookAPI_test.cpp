//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 XRPL-Labs

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
#include <ripple/app/hook/HookAPI.h>
#include <ripple/basics/StringUtilities.h>
#include <ripple/json/json_writer.h>
#include <ripple/protocol/STAccount.h>
#include "ripple/protocol/Indexes.h"
#include "test/jtx/genesis.h"
#include "test/jtx/hook.h"
#include <limits>
#include <test/app/Import_json.h>
#include <test/jtx.h>
#include <tuple>
#include <vector>

namespace ripple {

namespace test {

class HookAPI_test : public beast::unit_test::suite
{
private:
    ApplyContext
    createApplyContext(jtx::Env& env, OpenView& ov, STTx const& tx)
    {
        ApplyContext applyCtx{
            env.app(),
            ov,
            tx,
            tesSUCCESS,
            env.current()->fees().base,
            tapNONE,
            env.journal};
        return applyCtx;
    }

public:
    void
    test_accept(FeatureBitset features)
    {
        testcase("Test accept() hookapi");

        // TODO
        BEAST_EXPECT(true);
    }

    void
    test_rollback(FeatureBitset features)
    {
        testcase("Test rollback() hookapi");

        // TODO
        BEAST_EXPECT(true);
    }

    void
    testGuards(FeatureBitset features)
    {
        testcase("Test guards");

        // TODO
        BEAST_EXPECT(true);
    }

    void
    test_emit(FeatureBitset features)
    {
        testcase("Test emit");
        using namespace jtx;

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};

        using namespace hook_api;
        Env env{*this, features};

        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        STTx const emitInvokeTx = STTx(ttINVOKE, [&](STObject& obj) {
            obj[sfAccount] = alice.id();
            obj[sfSequence] = 0;
            obj[sfSigningPubKey] = PublicKey();
            obj[sfFirstLedgerSequence] = env.closed()->seq() + 1;
            obj[sfLastLedgerSequence] = env.closed()->seq() + 5;
            obj[sfFee] = env.closed()->fees().base;

            auto& emitDetails = obj.peekFieldObject(sfEmitDetails);
            emitDetails[sfEmitGeneration] = 1;
            emitDetails[sfEmitBurden] = 1;
            emitDetails[sfEmitParentTxnID] = invokeTx.getTransactionID();
            emitDetails[sfEmitNonce] = uint256();
            emitDetails[sfEmitHookHash] = uint256();
        });

        STTx const emitSetHookTx = STTx(ttHOOK_SET, [&](STObject& obj) {
            obj[sfAccount] = alice.id();
            obj[sfSequence] = 0;
            obj[sfSigningPubKey] = PublicKey();
            obj[sfFirstLedgerSequence] = env.closed()->seq() + 1;
            obj[sfLastLedgerSequence] = env.closed()->seq() + 5;
            obj[sfFee] = env.closed()->fees().base;
            STObject hookobj(sfHook);
            auto& hooks = obj.peekFieldArray(sfHooks);
            hooks.emplace_back(std::move(hookobj));

            auto& emitDetails = obj.peekFieldObject(sfEmitDetails);
            emitDetails[sfEmitGeneration] = 1;
            emitDetails[sfEmitBurden] = 1;
            emitDetails[sfEmitParentTxnID] = invokeTx.getTransactionID();
            emitDetails[sfEmitNonce] = uint256();
            emitDetails[sfEmitHookHash] = uint256();
        });

        {
            // PREREQUISITE_NOT_MET
            auto hookCtx = makeStubHookContext(
                applyCtx, alice.id(), alice.id(), {.expected_etxn_count = -1});
            hook::HookAPI api(hookCtx);

            auto const result = api.emit(emitInvokeTx.getSerializer().slice());
            BEAST_EXPECT(result.error() == PREREQUISITE_NOT_MET);
        }
        {
            // TOO_MANY_EMITTED_TXN
            std::string reason;
            auto tx = std::make_shared<ripple::Transaction>(
                std::make_shared<ripple::STTx const>(invokeTx),
                reason,
                env.app());
            std::queue<std::shared_ptr<ripple::Transaction>> emittedTxn;
            emittedTxn.push(tx);
            auto hookCtx = makeStubHookContext(
                applyCtx,
                alice.id(),
                alice.id(),
                {
                    .expected_etxn_count = 1,
                    .result = {.emittedTxn = emittedTxn},
                });
            hook::HookAPI api(hookCtx);

            auto const result = api.emit(emitInvokeTx.getSerializer().slice());
            BEAST_EXPECT(result.error() == TOO_MANY_EMITTED_TXN);
        }
        // EMISSION_FAILURE
        {
            // Invalid txn
            auto hookCtx = makeStubHookContext(
                applyCtx,
                alice.id(),
                alice.id(),
                {
                    .expected_etxn_count = 1,
                    .nonce_used = {{uint256(0), true}},
                });
            hook::HookAPI api(hookCtx);
            auto tx = emitInvokeTx;
            Serializer s = tx.getSerializer();
            s.add8(0);  // invalid value
            auto const result = api.emit(s.slice());
            BEAST_EXPECT(result.error() == EMISSION_FAILURE);
        }
        {
            // Pseudo txn
            auto hookCtx = makeStubHookContext(
                applyCtx,
                alice.id(),
                alice.id(),
                {
                    .expected_etxn_count = 1,
                    .nonce_used = {{uint256(0), true}},
                });
            hook::HookAPI api(hookCtx);
            auto tx = emitInvokeTx;
            tx.setFieldU16(sfTransactionType, ttFEE);
            auto const result = api.emit(tx.getSerializer().slice());
            BEAST_EXPECT(result.error() == EMISSION_FAILURE);
        }
        {
            // HookCanEmit (non-SetHook)
            auto hookCtx = makeStubHookContext(
                applyCtx,
                alice.id(),
                alice.id(),
                {.expected_etxn_count = 1,
                 .nonce_used = {{uint256(0), true}},
                 .result = {
                     .hookCanEmit = UINT256_BIT[ttINVOKE],
                 }});
            hook::HookAPI api(hookCtx);
            auto tx = emitInvokeTx;
            auto const result = api.emit(tx.getSerializer().slice());
            BEAST_EXPECT(result.error() == EMISSION_FAILURE);
        }
        {
            // HookCanEmit (SetHook) Error
            auto hookCtx = makeStubHookContext(
                applyCtx,
                alice.id(),
                alice.id(),
                {
                    .expected_etxn_count = 1,
                    .nonce_used = {{uint256(0), true}},
                    .result = {.hookCanEmit = uint256()},
                });
            hook::HookAPI api(hookCtx);
            auto const result = api.emit(emitSetHookTx.getSerializer().slice());
            BEAST_EXPECT(result.error() == EMISSION_FAILURE);
        }
        {
            // HookCanEmit (SetHook) Success
            auto hookCtx = makeStubHookContext(
                applyCtx,
                alice.id(),
                alice.id(),
                {
                    .expected_etxn_count = 1,
                    .nonce_used = {{uint256(0), true}},
                    .result = {.hookCanEmit = UINT256_BIT[ttHOOK_SET]},
                });
            hook::HookAPI api(hookCtx);
            auto tx = emitSetHookTx;
            auto const result = api.emit(tx.getSerializer().slice());
            BEAST_EXPECT(result.has_value());
        }

        auto hookCtx = makeStubHookContext(
            applyCtx,
            alice.id(),
            alice.id(),
            {
                .expected_etxn_count = 1,
                .nonce_used = {{uint256(0), true}},
                .result = {.hookCanEmit = uint256()},
            });
        hook::HookAPI api(hookCtx);
        {
            // Invalid sfAccount
            auto tx = emitInvokeTx;
            {
                // Missing sfAccount
                tx.makeFieldAbsent(sfAccount);
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
            {
                // Invalid sfAccount (!= HookAccount)
                tx.setAccountID(sfAccount, bob.id());
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
        }
        {
            // Invalid sfSequence
            auto tx = emitInvokeTx;
            {
                // Missing sfSequence
                tx.makeFieldAbsent(sfSequence);
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
            {
                // Invalid sfSequence (non-zero)
                tx.setFieldU32(sfSequence, 1);
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
        }
        {
            // Invalid sfSigningPubKey
            auto tx = emitInvokeTx;
            {
                // Missing sfSigningPubKey
                tx.makeFieldAbsent(sfSigningPubKey);
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
            {
                // Invalid sfSigningPubKey (wrong size)
                for (int i = 1; i < 33; ++i)
                {
                    tx.setFieldVL(sfSigningPubKey, std::vector<uint8_t>(i, 0));
                    auto const result = api.emit(tx.getSerializer().slice());
                    BEAST_EXPECT(result.error() == EMISSION_FAILURE);
                }
            }
            {
                // Invalid sfSigningPubKey (non-zero)
                for (int i = 0; i < 33; ++i)
                {
                    auto vec = std::vector<uint8_t>(33, 0);
                    vec[i] = 1;
                    tx.setFieldVL(sfSigningPubKey, vec);
                    auto const result = api.emit(tx.getSerializer().slice());
                    BEAST_EXPECT(result.error() == EMISSION_FAILURE);
                }
            }
        }
        {
            // Invalid sfSigners
            auto tx = emitInvokeTx;
            tx.setFieldArray(sfSigners, STArray(sfSigners, 1));
            auto const result = api.emit(tx.getSerializer().slice());
            BEAST_EXPECT(result.error() == EMISSION_FAILURE);
        }
        {
            // Invalid sfTicketSequence
            auto tx = emitInvokeTx;
            tx.setFieldU32(sfTicketSequence, 1);
            auto const result = api.emit(tx.getSerializer().slice());
            BEAST_EXPECT(result.error() == EMISSION_FAILURE);
        }
        {
            // Invalid sfAccountTxnID
            auto tx = emitInvokeTx;
            tx.setFieldH256(sfAccountTxnID, uint256(1));
            auto const result = api.emit(tx.getSerializer().slice());
            BEAST_EXPECT(result.error() == EMISSION_FAILURE);
        }
        {
            ;  // Invalid sfEmitDetails
            {
                // Missing sfEmitDetails
                auto tx = emitInvokeTx;
                tx.makeFieldAbsent(sfEmitDetails);
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
            {
                std::vector<std::reference_wrapper<SField const>>
                    detail_fields = {
                        sfEmitGeneration,
                        sfEmitBurden,
                        sfEmitParentTxnID,
                        sfEmitNonce,
                        sfEmitHookHash,
                    };
                // Missing fields in sfEmitDetails
                for (auto const& rf : detail_fields)
                {
                    SField const& field = rf.get();
                    auto tx = emitInvokeTx;
                    auto& details = tx.peekFieldObject(sfEmitDetails);
                    details.makeFieldAbsent(field);
                    auto const result = api.emit(tx.getSerializer().slice());
                    BEAST_EXPECT(result.error() == EMISSION_FAILURE);
                }
            }
            // TODO: test callback
            {
                ;  // Invalid sfEmitGeneration
                {
                    // Over Max sfEmitGeneration
                    auto tx = emitInvokeTx;
                    auto& details = tx.peekFieldObject(sfEmitDetails);
                    details.setFieldU32(sfEmitGeneration, 11);
                    auto const result = api.emit(tx.getSerializer().slice());
                    BEAST_EXPECT(result.error() == EMISSION_FAILURE);
                }
                {
                    // Invalid sfEmitGeneration
                    auto tx = emitInvokeTx;
                    auto& details = tx.peekFieldObject(sfEmitDetails);
                    details.setFieldU32(
                        sfEmitGeneration, hookCtx.generation + 2);
                    auto const result = api.emit(tx.getSerializer().slice());
                    BEAST_EXPECT(result.error() == EMISSION_FAILURE);
                }
            }
            {
                // Invalid sfEmitBurden
                auto tx = emitInvokeTx;
                auto& details = tx.peekFieldObject(sfEmitDetails);
                BEAST_EXPECT(hookCtx.burden == 0);
                details.setFieldU64(sfEmitBurden, 2);
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
            {
                // Invalid sfEmitParentTxnID
                auto tx = emitInvokeTx;
                auto& details = tx.peekFieldObject(sfEmitDetails);
                BEAST_EXPECT(applyCtx.tx.getTransactionID() != uint256(1));
                details.setFieldH256(sfEmitParentTxnID, uint256(1));
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
            {
                // Invalid sfEmitNonce
                auto tx = emitInvokeTx;
                auto& details = tx.peekFieldObject(sfEmitDetails);
                BEAST_EXPECT(
                    hookCtx.nonce_used.find(uint256(1)) ==
                    hookCtx.nonce_used.end());
                details.setFieldH256(sfEmitNonce, uint256(1));
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
            // TODO: test Callback
            {
                // Invalid sfEmitHookHash
                auto tx = emitInvokeTx;
                auto& details = tx.peekFieldObject(sfEmitDetails);
                BEAST_EXPECT(hookCtx.result.hookHash != uint256(1));
                details.setFieldH256(sfEmitHookHash, uint256(1));
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
        }
        {
            // Invalid sfTxnSignature
            auto tx = emitInvokeTx;
            tx.setFieldVL(sfTxnSignature, std::vector<uint8_t>(1, 0));
            auto const result = api.emit(tx.getSerializer().slice());
            BEAST_EXPECT(result.error() == EMISSION_FAILURE);
        }
        {
            ;  // Invalid sfLastLedgerSequence
            {
                // Missing sfLastLedgerSequence
                auto tx = emitInvokeTx;
                tx.makeFieldAbsent(sfLastLedgerSequence);
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
            {
                // Invalid sfLastLedgerSequence
                // (smaller than next ledger seq)
                auto tx = emitInvokeTx;
                auto const currentSeq = applyCtx.view().info().seq;
                tx.setFieldU32(sfLastLedgerSequence, currentSeq);
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
            {
                // Invalid sfLastLedgerSequence
                // (greater than current ledger seq + 5)
                auto tx = emitInvokeTx;
                auto const currentSeq = applyCtx.view().info().seq;
                tx.setFieldU32(sfLastLedgerSequence, currentSeq + 6);
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
        }
        {
            ;  // Invalid sfFirstLedgerSequence
            {
                // missing sfFirstLedgerSequence
                auto tx = emitInvokeTx;
                tx.makeFieldAbsent(sfFirstLedgerSequence);
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
            {
                // Invalid sfFirstLedgerSequence
                auto tx = emitInvokeTx;
                auto const lastLedgerSeq = tx.getFieldU32(sfLastLedgerSequence);
                tx.setFieldU32(sfFirstLedgerSequence, lastLedgerSeq + 1);
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
        }
        {
            ;  // Invalid sfFee
            {
                // Missing sfFee
                auto tx = emitInvokeTx;
                tx.makeFieldAbsent(sfFee);
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
            {
                // Invalid sfFee
                auto tx = emitInvokeTx;
                tx.setFieldAmount(sfFee, drops(1));
                auto const result = api.emit(tx.getSerializer().slice());
                BEAST_EXPECT(result.error() == EMISSION_FAILURE);
            }
        }
        {
            // Preflight failure
            auto tx = emitInvokeTx;
            tx.setFieldVL(sfBlob, std::vector<uint8_t>(128 * 1024 + 1, 1));
            auto const result = api.emit(tx.getSerializer().slice());
            BEAST_EXPECT(result.error() == EMISSION_FAILURE);
        }
        {
            // Success
            auto tx = emitInvokeTx;
            Serializer s;
            tx.add(s);
            auto const result = api.emit(s.slice());
            BEAST_EXPECT(result.has_value());
        }
    }

    void
    test_etxn_details(FeatureBitset features)
    {
        testcase("Test etxn_details");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        {
            // PREREQUISITE_NOT_MET
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            std::array<uint8_t, 256> buffer{};
            auto const result = api.etxn_details(buffer.data());
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == PREREQUISITE_NOT_MET);
        }

        {
            // FEE_TOO_LARGE (via etxn_burden overflow)
            StubHookContext stubCtx{
                .expected_etxn_count = 2,
                .burden = std::numeric_limits<uint64_t>::max(),
            };
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            std::array<uint8_t, 256> buffer{};
            auto const result = api.etxn_details(buffer.data());
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == FEE_TOO_LARGE);
        }

        {
            // SUCCESS path length check and nonce increment (no-callback)
            StubHookContext stubCtx{
                .expected_etxn_count = 2,
                .result = {.hookHash = uint256{3}},
            };
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            std::array<uint8_t, 256> buffer{};
            auto const result = api.etxn_details(buffer.data());
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == 116);
            BEAST_EXPECT(hookCtx.emit_nonce_counter == 1);
        }

        {
            // SUCCESS path length check and nonce increment (callback)
            StubHookContext stubCtx{
                .expected_etxn_count = 2,
                .result = {.hookHash = uint256{3}, .hasCallback = true},
            };
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            std::array<uint8_t, 256> buffer{};
            auto const result = api.etxn_details(buffer.data());
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == 138);
            BEAST_EXPECT(hookCtx.emit_nonce_counter == 1);
        }
    }

    void
    test_etxn_fee_base(FeatureBitset features)
    {
        testcase("Test etxn_fee_base");
        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        hook::HookContext hookCtx = makeStubHookContext(
            applyCtx,
            alice.id(),
            alice.id(),
            {
                .expected_etxn_count = -1,
                .nonce_used = {{uint256(0), true}},
                .result = {.hookCanEmit = uint256()},
            });
        hook::HookAPI api(hookCtx);

        // PREREQUISITE_NOT_MET
        {
            auto const result =
                api.etxn_fee_base(invokeTx.getSerializer().slice());
            BEAST_EXPECT(result.error() == PREREQUISITE_NOT_MET);
        }

        hookCtx.expected_etxn_count = 1;

        // INVALID_TXN
        {
            auto tx = invokeTx;
            Serializer s = tx.getSerializer();
            s.add8(0);  // invalid value
            auto const result = api.etxn_fee_base(s.slice());
            BEAST_EXPECT(result.error() == INVALID_TXN);
        }
        {
            // SUCCESS
            auto const result =
                api.etxn_fee_base(invokeTx.getSerializer().slice());
            BEAST_EXPECT(result.has_value());
            auto const baseFee = env.closed()->fees().base;
            BEAST_EXPECT(result.value() == baseFee);
        }
        {
            // Fee value
            auto tx = invokeTx;
            // add 100 bytes of blob
            tx.setFieldVL(sfBlob, std::vector<uint8_t>(100, 1));
            // add 100 bytes of memo
            tx.setFieldArray(sfMemos, STArray(sfMemos, 1));
            auto& memos = tx.peekFieldArray(sfMemos);
            STObject memo = STObject(sfMemo);
            memo.setFieldVL(sfMemoData, std::vector<uint8_t>(100, 1));
            memos.emplace_back(memo);
            auto const result = api.etxn_fee_base(tx.getSerializer().slice());
            BEAST_EXPECT(result.has_value());
            auto const baseFee = env.closed()->fees().base;
            auto const blobSize = 100;
            auto const memoSize = 100;
            if (env.closed()->rules().enabled(fixHookAPI20251128))
                BEAST_EXPECT(result.value() == baseFee + blobSize + memoSize);
            else
                BEAST_EXPECT(result.value() == baseFee + memoSize);
        }
    }

    void
    test_etxn_burden(FeatureBitset features)
    {
        testcase("Test etxn_burden");
        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        {
            // PREREQUISITE_NOT_MET
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            auto const result = api.etxn_burden();
            BEAST_EXPECT(result.error() == PREREQUISITE_NOT_MET);
        }

        {
            // FEE_TOO_LARGE (overflow)
            StubHookContext stubCtx{
                .expected_etxn_count = 2,
                .burden = std::numeric_limits<uint64_t>::max(),
            };
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            auto const result = api.etxn_burden();
            BEAST_EXPECT(result.error() == FEE_TOO_LARGE);
        }

        {
            // SUCCESS
            StubHookContext stubCtx{.expected_etxn_count = 3, .burden = 5};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            auto const result = api.etxn_burden();
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == 15);
        }
    }

    void
    test_etxn_generation(FeatureBitset features)
    {
        testcase("Test etxn_generation");
        using namespace jtx;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx baseTx = STTx(ttINVOKE, [&](STObject& obj) {});

        {
            // Cached generation value
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, baseTx);
            StubHookContext stubCtx{.generation = 4};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.etxn_generation() == 5);
        }

        {
            // No emit details -> otxn_generation() == 0
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, baseTx);
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.etxn_generation() == 1);
        }

        {
            // Emit details supply generation
            STTx emitTx = STTx(ttINVOKE, [&](STObject& obj) {
                obj.peekFieldObject(sfEmitDetails)
                    .setFieldU32(sfEmitGeneration, 2);
            });
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, emitTx);
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.etxn_generation() == 3);
        }
    }

    void
    test_etxn_nonce(FeatureBitset features)
    {
        testcase("Test etxn_nonce");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        hook::HookContext hookCtx = makeStubHookContext(
            applyCtx,
            alice.id(),
            alice.id(),
            {
                .expected_etxn_count = -1,
                .nonce_used = {{uint256(0), true}},
                .result = {.hookCanEmit = uint256()},
            });

        // TOO_MANY_NONCES
        {
            hookCtx.emit_nonce_counter = hook_api::max_nonce + 1;
            hook::HookAPI api(hookCtx);
            auto const result = api.etxn_nonce();
            BEAST_EXPECT(result.error() == TOO_MANY_NONCES);
        }

        // SUCCESS
        {
            hookCtx.emit_nonce_counter = hook_api::max_nonce;
            hook::HookAPI api(hookCtx);
            auto const result = api.etxn_nonce();
            BEAST_EXPECT(result.has_value());
        }

        {
            // Flags and cache tracking
            StubHookContext stubCtx{
                .nonce_used = {},
                .result =
                    {.isCallback = true,
                     .isStrong = true,
                     .hookChainPosition = 2},
            };
            auto hookCtxNonce =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtxNonce);
            auto const expectedFlags =
                static_cast<uint32_t>(0b11U | (2U << 2U));
            auto const expected = ripple::sha512Half(
                ripple::HashPrefix::emitTxnNonce,
                hookCtxNonce.applyCtx.tx.getTransactionID(),
                hookCtxNonce.emit_nonce_counter,
                hookCtxNonce.result.account,
                hookCtxNonce.result.hookHash,
                expectedFlags);
            auto const result = api.etxn_nonce();
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == expected);
            BEAST_EXPECT(hookCtxNonce.emit_nonce_counter == 1);
            BEAST_EXPECT(hookCtxNonce.nonce_used.count(expected) == 1);
        }
    }

    void
    test_etxn_reserve(FeatureBitset features)
    {
        testcase("Test etxn_reserve");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        {
            // ALREADY_SET
            StubHookContext stubCtx{.expected_etxn_count = 1};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            auto const result = api.etxn_reserve(2);
            BEAST_EXPECT(result.error() == ALREADY_SET);
        }

        {
            // TOO_SMALL
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            auto const result = api.etxn_reserve(0);
            BEAST_EXPECT(result.error() == TOO_SMALL);
        }

        {
            // TOO_BIG
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            auto const result = api.etxn_reserve(hook_api::max_emit + 1);
            BEAST_EXPECT(result.error() == TOO_BIG);
        }

        {
            // SUCCESS
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            auto const result = api.etxn_reserve(3);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(hookCtx.expected_etxn_count == 3);
        }
    }

    void
    test_fee_base(FeatureBitset features)
    {
        testcase("Test fee_base");

        using namespace jtx;
        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        auto const expected = env.closed()->fees().base.drops();
        BEAST_EXPECT(api.fee_base() == expected);
    }

    void
    ASSERT_FLOAT_EQUAL(hook::HookAPI& api, uint64_t x, uint64_t y)
    {
        auto float_exponent = [](uint64_t f) -> int32_t {
            return ((int32_t)(((f) >> 54U) & 0xFFU)) - 97;
        };
        int64_t px = (x);
        int64_t py = (y);
        int64_t mx = api.float_mantissa(px).value();
        int64_t my = api.float_mantissa(py).value();
        int32_t diffexp = float_exponent(px) - float_exponent(py);
        if (diffexp == 1)
            mx *= 10LL;
        if (diffexp == -1)
            my *= 10LL;
        int64_t diffman = mx - my;
        if (diffman < 0)
            diffman *= -1LL;
        if (diffexp < 0)
            diffexp *= -1;
        if (diffexp > 1 || diffman > 5000000 || mx < 0 || my < 0)
            BEAST_EXPECT(false);
        else
            BEAST_EXPECT(true);
    }

    void
    test_float_compare(FeatureBitset features)
    {
        testcase("Test float_compare");

        using namespace jtx;
        using namespace hook_api;
        using namespace compare_mode;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        auto const testSuccess =
            [&](uint64_t left, uint64_t right, uint32_t mode) {
                auto const result = api.float_compare(left, right, mode);
                BEAST_EXPECT(result.has_value());
                BEAST_EXPECT(result.value() == 1);
            };

        auto const testError = [&](uint64_t left,
                                   uint64_t right,
                                   uint32_t mode,
                                   hook::HookReturnCode error) {
            auto const result = api.float_compare(left, right, mode);
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == error);
        };

        auto const one = api.float_one();
        auto const two = api.float_set(0, 2).value();
        auto const three = 6091866696204910592ULL;  // encoded 3.0 from SetHook

        testSuccess(one, one, EQUAL);
        testSuccess(one, two, LESS);
        testSuccess(two, one, GREATER);
        testError(one, two, 0, INVALID_ARGUMENT);

        // Invalid flags
        testError(one, two, 0b1000U, INVALID_ARGUMENT);
        testError(one, two, ~0b111U, INVALID_ARGUMENT);
        testError(one, two, 0b111U, INVALID_ARGUMENT);

        // Relative ordering samples
        uint64_t largeNegative = 1622844335003378560ULL;  // -154846915
        uint64_t smallNegative = 1352229899321148800ULL;  // -1.15001111e-7
        uint64_t smallPositive =
            5713898440837102138ULL;  // 3.33411333131321e-21
        uint64_t largePositive = 7749425685711506120ULL;  // 3.234326634253e+92

        testSuccess(largeNegative, smallNegative, LESS);
        testSuccess(largeNegative, largePositive, LESS);
        testSuccess(smallNegative, smallPositive, LESS);
        testSuccess(smallPositive, largePositive, LESS);
        testSuccess(smallNegative, 0, LESS);
        testSuccess(largeNegative, 0, LESS);
        testSuccess(smallPositive, 0, GREATER);
        testSuccess(largePositive, 0, GREATER);

        // Not-equal flag check
        testSuccess(two, three, GREATER | LESS);
    }

    void
    test_float_divide(FeatureBitset features)
    {
        testcase("Test float_divide");

        using namespace jtx;
        using namespace hook_api;
        using namespace compare_mode;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        auto const one = api.float_one();

        // divide by 0
        BEAST_EXPECT(api.float_divide(one, 0).error() == DIVISION_BY_ZERO);
        BEAST_EXPECT(api.float_divide(0, one).value() == 0);

        // check 1
        BEAST_EXPECT(api.float_divide(one, one).value() == one);
        BEAST_EXPECT(
            api.float_divide(one, api.float_negate(one)).value() ==
            api.float_negate(one));
        BEAST_EXPECT(
            api.float_divide(api.float_negate(one), one).value() ==
            api.float_negate(one));
        BEAST_EXPECT(
            api.float_divide(api.float_negate(one), api.float_negate(one))
                .value() == one);

        // 1 / 10 = 0.1
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_divide(one, 6107881094714392576LL).value(),
            6071852297695428608LL);

        // 123456789 / 1623 = 76067.0295749
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_divide(6234216452170766464LL, 6144532891733356544LL)
                .value(),
            6168530993200328528LL);

        // -1.245678451111 / 1.3546984132111e+42 = -9.195245517106014e-43
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_divide(1478426356228633688LL, 6846826132016365020LL)
                .value(),
            711756787386903390LL);

        // 9.134546514878452e-81 / 1
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_divide(4638834963451748340LL, one).value(),
            4638834963451748340LL);

        // 9.134546514878452e-81 / 1.41649684651e+75 = (underflow 0)
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_divide(4638834963451748340LL, 7441363081262569392LL)
                .value(),
            0);

        // 1.3546984132111e+42 / 9.134546514878452e-81  = XFL_OVERFLOW
        BEAST_EXPECT(
            api.float_divide(6846826132016365020LL, 4638834963451748340LL)
                .error() == XFL_OVERFLOW);

        // clang-format off
        std::vector<std::tuple<uint64_t, uint64_t, uint64_t>> tests = {
            {3121244226425810900LL /* -4.753284285427668e+91 */, 2135203055881892282LL /* -9.50403176301817e+36 */, 7066645550312560102LL /* 5.001334595622374e+54 */},
            {2473507938381460320LL /* -5.535342582428512e+55 */, 6365869885731270068LL /* 6787211884129716 */     , 2187897766692155363LL /* -8.155547044835299e+39 */},
            {1716271542690607496LL /* -49036842898190.16 */, 3137794549622534856LL /* -3.28920897266964e+92 */, 4667220053951274769LL /* 1.490839995440913e-79 */},
            {1588045991926420391LL /* -2778923.092005799 */, 5933338827267685794LL /* 6.601717648113058e-9 */, 1733591650950017206LL /* -420939403974674.2 */},
            {5880783758174228306LL /* 8.089844083101523e-12 */, 1396720886139976383LL /* -0.00009612200909863615 */, 1341481714205255877LL /* -8.416224503589061e-8 */},
            {5567703563029955929LL /* 1.254423600022873e-29 */, 2184969513100691140LL /* -5.227293453371076e+39 */, 236586937995245543LL /* -2.399757371979751e-69 */},
            {7333313065548121054LL /* 1.452872188953566e+69 */, 1755926008837497886LL /* -8529353417745438 */, 2433647177826281173LL /* -1.703379046213333e+53 */},
            {1172441975040622050LL /* -1.50607192429309e-17 */, 6692015311011173216LL /* 8.673463993357152e+33 */, 560182767210134346LL /* -1.736413416192842e-51 */},
            {577964843368607493LL /* -1.504091065184005e-50 */, 6422931182144699580LL /* 9805312769113276000 */, 235721135837751035LL /* -1.533955214485243e-69 */},
            {6039815413139899240LL /* 0.0049919124634346 */, 2117655488444284242LL /* -9.970862834892113e+35 */, 779625635892827768LL /* -5.006499985102456e-39 */},
            {1353563835098586141LL /* -2.483946887437341e-7 */, 6450909070545770298LL /* 175440415122002600000 */, 992207753070525611LL /* -1.415835049016491e-27 */},
            {6382158843584616121LL /* 50617712279937850 */, 5373794957212741595LL /* 5.504201387110363e-40 */, 7088854809772330055LL /* 9.196195545910343e+55 */},
            {2056891719200540975LL /* -3.250289119594799e+32 */, 1754532627802542730LL /* -7135972382790282 */, 6381651867337939070LL /* 45547949813167340 */},
            {5730152450208688630LL /* 1.573724193417718e-20 */, 1663581695074866883LL /* -62570322025.24355 */, 921249452789827075LL /* -2.515128806245891e-31 */},
            {6234301156018475310LL /* 131927173.7708846 */, 2868710604383082256LL /* -4.4212413754468e+77 */, 219156721749007916LL /* -2.983939635224108e-70 */},
            {2691125731495874243LL /* -6.980353583058627e+67 */, 7394070851520237320LL /* 8.16746263262388e+72 */, 1377640825464715759LL /* -0.000008546538744084975 */},
            {5141867696142208039LL /* 7.764120939842599e-53 */, 5369434678231981897LL /* 1.143922406350665e-40 */, 5861466794943198400LL /* 6.7872793615536e-13 */},
            {638296190872832492LL /* -7.792243040963052e-47 */, 5161669734904371378LL /* 9.551761192523954e-52 */, 1557396184145861422LL /* -81579.12330410798 */},
            {2000727145906286285LL /* -1.128911353786061e+29 */, 2096625200460673392LL /* -6.954973360763248e+34 */, 5982403476503576795LL /* 0.000001623171355558107 */},
            {640472838055334326LL /* -9.968890223464885e-47 */, 5189754252349396763LL /* 1.607481618585371e-50 */, 1537425431139169736LL /* -6201.557833201096 */},
        };
        // clang-format on

        for (auto const& test : tests)
        {
            ASSERT_FLOAT_EQUAL(
                api,
                api.float_divide(std::get<0>(test), std::get<1>(test)).value(),
                std::get<2>(test));
        }
    }

    void
    test_float_int(FeatureBitset features)
    {
        testcase("Test float_int");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        auto const one = api.float_one();

        // clang-format off

        // check 1
        BEAST_EXPECT(api.float_int(one, 0, 0).value() == 1LL);

        // check 1.23e-20 always returns 0 (too small to display)
        BEAST_EXPECT(api.float_int(5729808726015270912LL, 0, 0).value() == 0);
        BEAST_EXPECT(api.float_int(5729808726015270912LL, 15, 0).value() == 0);
        BEAST_EXPECT(
            api.float_int(5729808726015270912LL, 16, 0).error() ==
            INVALID_ARGUMENT);

        BEAST_EXPECT(api.float_int(one, 15, 0).value() == 1000000000000000LL);
        BEAST_EXPECT(api.float_int(one, 14, 0).value() == 100000000000000LL);
        BEAST_EXPECT(api.float_int(one, 13, 0).value() == 10000000000000LL);
        BEAST_EXPECT(api.float_int(one, 12, 0).value() == 1000000000000LL);
        BEAST_EXPECT(api.float_int(one, 11, 0).value() == 100000000000LL);
        BEAST_EXPECT(api.float_int(one, 10, 0).value() == 10000000000LL);
        BEAST_EXPECT(api.float_int(one, 9, 0).value() == 1000000000LL);
        BEAST_EXPECT(api.float_int(one, 8, 0).value() == 100000000LL);
        BEAST_EXPECT(api.float_int(one, 7, 0).value() == 10000000LL);
        BEAST_EXPECT(api.float_int(one, 6, 0).value() == 1000000LL);
        BEAST_EXPECT(api.float_int(one, 5, 0).value() == 100000LL);
        BEAST_EXPECT(api.float_int(one, 4, 0).value() == 10000LL);
        BEAST_EXPECT(api.float_int(one, 3, 0).value() == 1000LL);
        BEAST_EXPECT(api.float_int(one, 2, 0).value() == 100LL);
        BEAST_EXPECT(api.float_int(one, 1, 0).value() == 10LL);
        BEAST_EXPECT(api.float_int(one, 0, 0).value() == 1LL);

        // normal upper limit on exponent
        BEAST_EXPECT(api.float_int(6360317241828374919LL, 0, 0).value() == 1234567981234567LL);

        // ask for one decimal above limit
        BEAST_EXPECT(api.float_int(6360317241828374919LL, 1, 0).error() == TOO_BIG);

        // ask for 15 decimals above limit
        BEAST_EXPECT(api.float_int(6360317241828374919LL, 15, 0).error() == TOO_BIG);

        // every combination for 1.234567981234567
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 0, 0).value() == 1LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 1, 0).value() == 12LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 2, 0).value() == 123LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 3, 0).value() == 1234LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 4, 0).value() == 12345LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 5, 0).value() == 123456LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 6, 0).value() == 1234567LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 7, 0).value() == 12345679LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 8, 0).value() == 123456798LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 9, 0).value() == 1234567981LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 10, 0).value() == 12345679812LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 11, 0).value() == 123456798123LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 12, 0).value() == 1234567981234LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 13, 0).value() == 12345679812345LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 14, 0).value() == 123456798123456LL);
        BEAST_EXPECT(api.float_int(6090101264186145159LL, 15, 0).value() == 1234567981234567LL);

        // same with absolute parameter
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 0, 1) .value() ==1LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 1, 1) .value() ==12LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 2, 1) .value() ==123LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 3, 1) .value() ==1234LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 4, 1) .value() ==12345LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 5, 1) .value() ==123456LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 6, 1) .value() ==1234567LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 7, 1) .value() ==12345679LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 8, 1) .value() ==123456798LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 9, 1) .value() ==1234567981LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 10, 1).value() == 12345679812LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 11, 1).value() == 123456798123LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 12, 1).value() == 1234567981234LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 13, 1).value() == 12345679812345LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 14, 1).value() == 123456798123456LL);
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 15, 1).value() == 1234567981234567LL);

        // neg xfl sans absolute parameter
        BEAST_EXPECT(api.float_int(1478415245758757255LL, 15, 0).error() == CANT_RETURN_NEGATIVE);

        // 1.234567981234567e-16
        BEAST_EXPECT(api.float_int(5819885286543915399LL, 15, 0).value() == 1LL);
        for (uint32_t i = 1; i < 15; ++i)
            BEAST_EXPECT(api.float_int(5819885286543915399LL, i, 0).value() == 0);

        // clang-format on
    }

    void
    test_float_invert(FeatureBitset features)
    {
        testcase("Test float_invert");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        // divide by 0
        BEAST_EXPECT(api.float_invert(0).error() == DIVISION_BY_ZERO);

        // check 1
        BEAST_EXPECT(
            api.float_invert(api.float_one()).value() == api.float_one());

        // 1 / 10 = 0.1
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_invert(6107881094714392576LL).value(),
            6071852297695428608LL);

        // 1 / 123 = 0.008130081300813009
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_invert(6126125493223874560LL).value(),
            6042953581977277649LL);

        // 1 / 1234567899999999 = 8.100000008100007e-16
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_invert(6360317241747140351LL).value(),
            5808736320061298855LL);

        // 1/ 1*10^-81 = 10**81
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_invert(4630700416936869888LL).value(),
            7540018576963469311LL);
    }

    void
    test_float_log(FeatureBitset features)
    {
        testcase("Test float_log");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        // check 0 is not allowed
        BEAST_EXPECT(api.float_log(0).error() == INVALID_ARGUMENT);

        // log10( 846513684968451 ) = 14.92763398342338
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_log(6349533412187342878LL).value(),
            6108373858112734914LL);

        // log10 ( -1000 ) = invalid (complex not supported)
        BEAST_EXPECT(
            api.float_log(1532223873305968640LL).error() ==
            COMPLEX_NOT_SUPPORTED);

        // log10 (1000) == 3
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_log(6143909891733356544LL).value(),
            6091866696204910592LL);

        // log10 (0.112381) == -0.949307107740766
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_log(6071976107695428608LL).value(),
            1468659350345448364LL);

        // log10 (0.00000000000000001123) = -16.94962024373854221
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_log(5783744921543716864LL).value(),
            1496890038311378526LL);

        // log10(100000000000000000000000000000000000000000000000000000000000000)
        // = 62
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_log(7206759403792793600LL).value(),
            6113081094714392576LL);
    }

    void
    test_float_mantissa(FeatureBitset features)
    {
        testcase("Test float_mantissa");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        // test canonical zero
        BEAST_EXPECT(api.float_mantissa(0).value() == 0);

        // test one, negative one
        {
            BEAST_EXPECT(
                api.float_mantissa(api.float_one()).value() ==
                1000000000000000LL);
            BEAST_EXPECT(
                api.float_mantissa(api.float_negate(api.float_one())).value() ==
                1000000000000000LL);
        }

        // test random numbers
        {
            // clang-format off
            std::vector<std::tuple<uint64_t, uint64_t>> tests = {
                {4763370308433150973LL /* 7.569101929907197e-74 */, 7569101929907197LL},
                {668909658849475214LL /* -2.376913998641806e-45 */, 2376913998641806LL},
                {962271544155031248LL /* -7.508423152486096e-29 */, 7508423152486096LL},
                {7335644976228470276LL /* 3.784782869302788e+69 */, 3784782869302788LL},
                {2837780149340315954LL /* -9.519583351644467e+75 */, 9519583351644466LL},
                {2614004940018599738LL /* -1.917156143712058e+63 */, 1917156143712058LL},
                {4812250541755005603LL /* 2.406139723315875e-71 */, 2406139723315875LL},
                {5140304866732560580LL /* 6.20129153019514e-53 */, 6201291530195140LL},
                {1124677839589482624LL /* -7.785132001599617e-20 */, 7785132001599616LL},
                {5269336076015865585LL /* 9.131711247126257e-46 */, 9131711247126257LL},
                {2296179634826760368LL /* -8.3510241225484e+45 */, 8351024122548400LL},
                {1104028240398536470LL /* -5.149931320135446e-21 */, 5149931320135446LL},
                {2691222059222981864LL /* -7.076681310166248e+67 */, 7076681310166248LL},
                {6113256168823855946LL /* 63.7507410946337 */, 6375074109463370LL},
                {311682216630003626LL /* -5.437441968809898e-65 */, 5437441968809898LL},
                {794955605753965262LL /* -2.322071336757966e-38 */, 2322071336757966LL},
                {204540636400815950LL /* -6.382252796514126e-71 */, 6382252796514126LL},
                {5497195278343034975LL /* 2.803732951029855e-33 */, 2803732951029855LL},
                {1450265914369875626LL /* -0.09114033611316906 */, 9114033611316906LL},
                {7481064015089962668LL /* 5.088633654939308e+77 */, 5088633654939308LL},
            };
            // clang-format on

            for (auto const& test : tests)
            {
                ASSERT_FLOAT_EQUAL(
                    api,
                    api.float_mantissa(std::get<0>(test)).value(),
                    std::get<1>(test));
            }
        }
    }

    void
    test_float_mulratio(FeatureBitset features)
    {
        testcase("Test float_mulratio");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        auto const one = api.float_one();
        auto const neg_one = api.float_negate(one);

        // multiply by 0
        BEAST_EXPECT(api.float_mulratio(one, 0, 0, 1).value() == 0);
        BEAST_EXPECT(api.float_mulratio(0, 0, 1, 1).value() == 0);

        // check 1
        BEAST_EXPECT(api.float_mulratio(one, 0, 1, 1).value() == one);
        BEAST_EXPECT(api.float_mulratio(neg_one, 0, 1, 1).value() == neg_one);

        // check overflow
        // 1e+95 * 1e+95
        BEAST_EXPECT(
            api.float_mulratio(7801234554605699072LL, 0, 0xFFFFFFFFUL, 1)
                .error() == XFL_OVERFLOW);
        // 1e+95 * 10
        BEAST_EXPECT(
            api.float_mulratio(7801234554605699072LL, 0, 10, 1).error() ==
            XFL_OVERFLOW);
        // -1e+95 * 10
        BEAST_EXPECT(
            api.float_mulratio(3189548536178311168LL, 0, 10, 1).error() ==
            XFL_OVERFLOW);

        // identity
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_mulratio(3189548536178311168LL, 0, 1, 1).value(),
            3189548536178311168LL);

        // random mulratios

        // clang-format off
        std::vector<std::tuple<uint64_t, uint32_t, uint32_t, uint64_t>> tests = {
            {2296131684119423544LL, 2210828011U, 2814367554U, 2294351094683836182LL},
            {565488225163275031LL, 2373474507U, 4203973264U, 562422045628095449LL},
            {2292703263479286183LL, 3170020147U, 773892643U, 2307839765178024100LL},
            {758435948837102675LL, 3802740780U, 1954123588U, 760168290112163547LL},
            {3063742137774439410LL, 2888815591U, 4122448592U, 3053503824756415637LL},
            {974014561126802184LL, 689168634U, 3222648522U, 957408554638995792LL},
            {2978333847445611553LL, 1718558513U, 2767410870U, 2976075722223325259LL},
            {6577058837932757648LL, 1423256719U, 1338068927U, 6577173649752398013LL},
            {2668681541248816636LL, 345215754U, 4259223936U, 2650183845127530219LL},
            {651803640367065917LL, 327563234U, 1191613855U, 639534906402789368LL},
            {3154958130393015979LL, 1304112625U, 3024066701U, 3153571282364880740LL},
            {1713286099776800976LL, 1902151138U, 2927030061U, 1712614441093927706LL},
            {2333142120591277120LL, 914099656U, 108514965U, 2349692988167140475LL},
            {995968561418010814LL, 1334462574U, 846156977U, 998955931389416094LL},
            {6276035843030312442LL, 2660687613U, 236740983U, 6294920527635363073LL},
            {7333118474702086419LL, 46947714U, 2479204760U, 7298214153648998535LL},
            {2873297486994296492LL, 880591893U, 436034100U, 2884122995598532757LL},
            {1935815261812737573LL, 3123665800U, 3786746543U, 1934366328810191207LL},
            {7249556282125616118LL, 2378803159U, 2248850590U, 7250005170160875417LL},
            {311005347529659996LL, 992915590U, 2433548552U, 308187142737041830LL},
        };
        // clang-format on

        for (auto const& test : tests)
        {
            ASSERT_FLOAT_EQUAL(
                api,
                api.float_mulratio(
                       std::get<0>(test),
                       0U,
                       std::get<1>(test),
                       std::get<2>(test))
                    .value(),
                std::get<3>(test));
        }
    }

    void
    test_float_multiply(FeatureBitset features)
    {
        testcase("Test float_multiply");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        auto const one = api.float_one();
        auto const neg_one = api.float_negate(one);

        // multiply by 0
        BEAST_EXPECT(api.float_multiply(one, 0).value() == 0);
        BEAST_EXPECT(api.float_multiply(0, one).value() == 0);

        // check 1
        BEAST_EXPECT(api.float_multiply(one, one).value() == one);
        BEAST_EXPECT(api.float_multiply(one, neg_one).value() == neg_one);
        BEAST_EXPECT(api.float_multiply(neg_one, one).value() == neg_one);
        BEAST_EXPECT(api.float_multiply(neg_one, neg_one).value() == one);

        // check overflow
        // 1e+95 * 1e+95
        BEAST_EXPECT(
            api.float_multiply(7801234554605699072LL, 7801234554605699072LL)
                .error() == XFL_OVERFLOW);
        // 1e+95 * 10
        BEAST_EXPECT(
            api.float_multiply(7801234554605699072LL, 6107881094714392576LL)
                .error() == XFL_OVERFLOW);
        BEAST_EXPECT(
            api.float_multiply(6107881094714392576LL, 7801234554605699072LL)
                .error() == XFL_OVERFLOW);
        // -1e+95 * 10
        BEAST_EXPECT(
            api.float_multiply(3189548536178311168LL, 6107881094714392576LL)
                .error() == XFL_OVERFLOW);

        // identity
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_multiply(3189548536178311168LL, one).value(),
            3189548536178311168LL);
        ASSERT_FLOAT_EQUAL(
            api,
            api.float_multiply(one, 3189548536178311168LL).value(),
            3189548536178311168LL);

        // random multiplications

        // clang-format off
        std::vector<std::tuple<uint64_t, uint64_t, uint64_t>> tests = {
            {7791757438262485039LL /* 9.537282166267951e+94 */, 4759088999670263908LL /* 3.287793167020132e-74 */, 6470304726017852129LL /* 3.135661113819873e+21 */},
            {7534790022873909775LL /* 4.771445910440463e+80 */, 1017891960669847079LL /* -9.085644138855975e-26 */, 2472307761756037979LL /* -4.335165957006171e+55 */},
            {2813999069907898454LL /* -3.75290242870895e+74 */, 4962524721184225460LL /* 8.56513107667986e-63 */, 1696567870013294731LL /* -3214410121988.235 */},
            {2151742066453140308LL /* -8.028643824784212e+37 */, 437647738130579252LL /* -5.302173903011636e-58 */, 5732835652591705549LL /* 4.256926576434637e-20 */},
            {5445302332922546340LL /* 4.953983058987172e-36 */, 7770966530708354172LL /* 6.760773121619068e+93 */, 7137051085305881332LL /* 3.349275551015668e+58 */},
            {2542989542826132533LL /* -2.959352989172789e+59 */, 6308418769944702613LL /* 3379291626008.213 */, 2775217422137696934LL /* -1.000051677471398e+72 */},
            {5017652318929433511LL /* 9.649533293441959e-60 */, 6601401767766764916LL /* 8.131913296358772e+28 */, 5538267259220228820LL /* 7.846916809259732e-31 */},
            {892430323307269235LL /* -9.724796342652019e-33 */, 1444078017997143500LL /* -0.0292613723858478 */, 5479222755754111850LL /* 2.845608871588714e-34 */},
            {7030632722283214253LL /* 5.017303585240493e+52 */, 297400838197636668LL /* -9.170462045924924e-66 */, 1247594596364389994LL /* -4.601099210133098e-13 */},
            {1321751204165279730LL /* -6.700112973094898e-9 */, 2451801790748530375LL /* -1.843593458980551e+54 */, 6918764256086244704LL /* 1.235228445162848e+46 */},
            {2055496484261758590LL /* -1.855054180812414e+32 */, 2079877890137711361LL /* -8.222061547283201e+33 */, 7279342234795540005LL /* 1.525236964818469e+66 */},
            {2439875962311968674LL /* -7.932163531900834e+53 */, 4707485682591872793LL /* 5.727671617074969e-77 */, 1067392794851803610LL /* -4.543282792366554e-23 */},
            {6348574818322812800LL /* 750654298515443.2 */, 6474046245013515838LL /* 6.877180109483582e+21 */, 6742547427357110773LL /* 5.162384810848757e+36 */},
            {1156137305783593424LL /* -3.215801176746448e-18 */, 351790564990861307LL /* -9.516993310703611e-63 */, 4650775291275116747LL /* 3.060475828764875e-80 */},
            {5786888485280994123LL /* 4.266563737277259e-17 */, 6252137323085080394LL /* 1141040294.831946 */, 5949619829273756852LL /* 4.868321144702132e-8 */},
            {2078182880999439640LL /* -6.52705240901148e+33 */, 1662438186251269392LL /* -51135233789.26864 */, 6884837854131013998LL /* 3.33762350889611e+44 */},
            {1823781083140711248LL /* -43268336830308640000 */, 1120252241608199010LL /* -3.359534020316002e-20 */, 6090320310700749729LL /* 1.453614495839137 */},
            {6617782604883935174LL /* 6.498351904047046e+29 */, 6185835042802056262LL /* 689635.404973575 */, 6723852137583788319LL /* 4.481493547008287e+35 */},
            {333952667495151166LL /* -9.693494324475454e-64 */, 1556040883317758614LL /* -68026.1150230799 */, 5032611291744396930LL /* 6.594107598923394e-59 */},
            {2326968399632616779LL /* -3.110991909440843e+47 */, 707513695207834635LL /* -4.952153338037259e-43 */, 6180479299649214949LL /* 154061.0896894437 */},
            {1271003508324696477LL /* -9.995612660957597e-12 */, 5321949753651889765LL /* 7.702193354704484e-43 */, 512101972406838314LL /* -7.698814141342762e-54 */},
            {1928646740923345323LL /* -1.106100408773035e+25 */, 4639329980209973352LL /* 9.629563273103463e-81 */, 487453886143282122LL /* -1.065126387268554e-55 */},
            {6023906813956669432LL /* 0.0007097711789686777 */, 944348444470060009LL /* -7.599721976996842e-30 */, 888099590592064434LL /* -5.394063627447218e-33 */},
            {6580290597764062787LL /* 5.035141803138627e+27 */, 6164319297265300034LL /* 33950.07022461506 */, 6667036882686408593LL /* 1.709434178074513e+32 */},
            {2523439530503240484LL /* -1.423739175762724e+58 */, 5864448766677980801LL /* 9.769251096336e-13 */, 2307233895764065602LL /* -1.39088655037165e+46 */},
            {6760707453987140465LL /* 5.308012931396465e+37 */, 5951641080643457645LL /* 6.889572514402925e-8 */, 6632955645489194550LL /* 3.656993999824438e+30 */},
            {6494270716308443375LL /* 9.087252894929135e+22 */, 564752637895553836LL /* -6.306284101612332e-51 */, 978508199357889360LL /* -5.730679845862224e-28 */},
            {6759145618427534062LL /* 3.746177371790062e+37 */, 4721897842483633304LL /* 2.125432999353496e-76 */, 5394267403342547165LL /* 7.962249007433949e-39 */},
            {1232673571201806425LL /* -7.694472557031513e-14 */, 6884256144221925318LL /* 2.75591359980743e+44 */, 2037747561727791012LL /* -2.12053015632682e+31 */},
            {1427694775835421031LL /* -0.004557293586344295 */, 4883952867277976402LL /* 2.050871208358738e-67 */, 225519204318055258LL /* -9.34642220427145e-70 */},
            {5843509949864662087LL /* 6.84483279249927e-14 */, 5264483986612843822LL /* 4.279621844104494e-46 */, 5028946513739275800LL /* 2.929329593802264e-59 */},
            {6038444022009738988LL /* 0.003620521333274348 */, 7447499078040748850LL /* 7.552493624689458e+75 */, 7406652183825856093LL /* 2.734396428760669e+73 */},
            {939565473697468970LL /* -2.816751204405802e-30 */, 1100284903077087966LL /* -1.406593998686942e-21 */, 5174094397561240825LL /* 3.962025339911417e-51 */},
            {5694071830210473617LL /* 1.521901214166673e-22 */, 5536709154363579683LL /* 6.288811952610595e-31 */, 5143674525748709391LL /* 9.570950546343951e-53 */},
            {600729862341871819LL /* -6.254711528966347e-49 */, 6330630279715378440LL /* 75764028872020.56 */, 851415551394320910LL /* -4.738821448667662e-35 */},
            {1876763139233864902LL /* -3.265694247738566e+22 */, 4849561230315278754LL /* 3.688031264625058e-69 */, 649722744589988028LL /* -1.204398248636604e-46 */},
            {3011947542126279863LL /* -3.542991042788535e+85 */, 1557732559110376235LL /* -84942.87294925611 */,7713172080438368541LL /* 3.009518380079389e+90 */},
            {5391579936313268788LL /* 5.274781978155572e-39 */, 1018647290024655822LL /* -9.840973493664718e-26 */, 329450072133864644LL /* -5.190898963188932e-64 */},
            {2815029221608845312LL /* -4.783054129655808e+74 */, 4943518985822088837LL /* 7.57379422402522e-64 */,1678961648155863225LL /* -362258677403.8713 */ },
            {1377509900308195934LL /* -0.00000841561358756515 */, 7702104197062186199LL /* 9.95603351337903e+89 */, 2998768765665354000LL /* -8.378613091344656e+84 */},
        };
        // clang-format on

        for (auto const& test : tests)
        {
            ASSERT_FLOAT_EQUAL(
                api,
                api.float_multiply(std::get<0>(test), std::get<1>(test))
                    .value(),
                std::get<2>(test));
        }
    }

    void
    test_float_negate(FeatureBitset features)
    {
        testcase("Test float_negate");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        auto const one = api.float_one();

        // test canonical zero
        BEAST_EXPECT(api.float_negate(0) == 0);

        // test double negation
        {
            BEAST_EXPECT(api.float_negate(one) != one);
            BEAST_EXPECT(api.float_negate(api.float_negate(one)) == one);
        }

        // test random numbers
        {
            // +/- 3.463476342523e+22
            BEAST_EXPECT(
                api.float_negate(6488646939756037240LL) ==
                1876960921328649336LL);

            BEAST_EXPECT(api.float_negate(one) == 1478180677777522688LL);

            BEAST_EXPECT(
                api.float_negate(1838620299498162368LL) ==
                6450306317925550272LL);
        }
    }

    void
    test_float_one(FeatureBitset features)
    {
        testcase("Test float_one");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        auto const one = api.float_one();
        BEAST_EXPECT(one == 6089866696204910592ULL);
    }

    void
    test_float_root(FeatureBitset features)
    {
        testcase("Test float_root");

        using namespace jtx;
        using namespace hook_api;
        using namespace compare_mode;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        auto const one = api.float_one();

        // sqrt 1 is 1
        ASSERT_FLOAT_EQUAL(api, api.float_root(one, 2).value(), one);

        // sqrt 9 is 3
        auto const nine = 6097866696204910592LL;
        auto const three = 6091866696204910592LL;
        ASSERT_FLOAT_EQUAL(api, api.float_root(nine, 2).value(), three);

        // cube root of 1000 is 10
        auto const thousand = 6143909891733356544LL;
        auto const ten = 6107881094714392576LL;
        ASSERT_FLOAT_EQUAL(api, api.float_root(thousand, 3).value(), ten);

        // sqrt of negative is "complex not supported error"
        auto const negative_one = 1478180677777522688LL;
        BEAST_EXPECT(
            api.float_root(negative_one, 2).error() == COMPLEX_NOT_SUPPORTED);

        // tenth root of 0 is 0
        ASSERT_FLOAT_EQUAL(api, api.float_root(0, 10).value(), 0);
    }

    void
    test_float_set(FeatureBitset features)
    {
        testcase("Test float_set");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        // zero mantissa should return canonical zero
        BEAST_EXPECT(api.float_set(-5, 0).value() == 0);
        BEAST_EXPECT(api.float_set(50, 0).value() == 0);
        BEAST_EXPECT(api.float_set(-50, 0).value() == 0);
        BEAST_EXPECT(api.float_set(0, 0).value() == 0);

        // an exponent lower than -96 should produce an invalid float error
        BEAST_EXPECT(api.float_set(-97, 1).error() == INVALID_FLOAT);

        // an exponent larger than +96 should produce an invalid float error
        BEAST_EXPECT(api.float_set(+97, 1).error() == INVALID_FLOAT);

        // clang-format off
        std::vector<std::tuple<int32_t, int64_t, uint64_t>> tests = {
            {-5, 6541432897943971LL, 6275552114197674403LL},
            {-83, 7906202688397446LL, 4871793800248533126LL},
            {76, 4760131426754533LL, 7732937091994525669LL},
            {37, -8019384286534438LL, 2421948784557120294LL},
            {50, 5145342538007840LL, 7264947941859247392LL},
            {-70, 4387341302202416LL, 5102462119485603888LL},
            {-26, -1754544005819476LL, 1280776838179040340LL},
            {36, 8261761545780560LL, 7015862781734272336LL},
            {35, 7975622850695472LL, 6997562244529705264LL},
            {17, -4478222822793996LL, 2058119652903740172LL},
            {-53, 5506604247857835LL, 5409826157092453035LL},
            {-60, 5120164869507050LL, 5283338928147728362LL},
            {41, 5176113875683063LL, 7102849126611584759LL},
            {-54, -3477931844992923LL, 778097067752718235LL},
            {21, 6345031894305479LL, 6743730074440567495LL},
            {-23, 5091583691147091LL, 5949843091820201811LL},
            {-33, 7509684078851678LL, 5772117207113086558LL},
            {-72, -1847771838890268LL, 452207734575939868LL},
            {71, -9138413713437220LL, 3035557363306410532LL},
            {28, 4933894067102586LL, 6868419726179738490LL},
        };
        // clang-format on

        for (auto const& test : tests)
        {
            ASSERT_FLOAT_EQUAL(
                api,
                api.float_set(std::get<0>(test), std::get<1>(test)).value(),
                std::get<2>(test));
        }
    }

    void
    test_float_sign(FeatureBitset features)
    {
        testcase("Test float_sign");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        // test canonical zero
        BEAST_EXPECT(api.float_sign(0) == 0);

        // test one
        auto const one = api.float_one();
        BEAST_EXPECT(api.float_sign(one) == 0);
        BEAST_EXPECT(api.float_sign(api.float_negate(one)) == 1);

        // test random numbers

        // clang-format off
        std::vector<std::tuple<int64_t, int64_t>> tests = {
            {7248434512952957686LL /* 6.646312141200119e+64 */, 0LL},
            {889927818394811978LL /* -7.222291430194763e-33 */, 1LL},
            {5945816149233111421LL /* 1.064641104056701e-8 */, 0LL},
            {6239200145838704863LL /* 621826155.7938399 */, 0LL},
            {6992780785042190360LL /* 3.194163363180568e+50 */, 0LL},
            {6883099933108789087LL /* 1.599702486671199e+44 */, 0LL},
            {890203738162163464LL /* -7.498211197546248e-33 */, 1LL},
            {4884803073052080964LL /* 2.9010769824633e-67 */, 0LL},
            {2688292350356944394LL /* -4.146972444128778e+67 */, 1LL},
            {4830109852288093280LL /* 2.251051746921568e-70 */, 0LL},
            {294175951907940320LL /* -5.945575756228576e-66 */, 1LL},
            {7612037404955382316LL /* 9.961233953985069e+84 */, 0LL},
            {7520840929603658997LL /* 8.83675114967167e+79 */, 0LL},
            {4798982086157926282LL /* 7.152082635718538e-72 */, 0LL},
            {689790136568817905LL /* -5.242993208502513e-44 */, 1LL},
            {5521738045011558042LL /* 9.332101110070938e-32 */, 0LL},
            {728760820583452906LL /* -8.184880204173546e-42 */, 1LL},
            {2272937984362856794LL /* -3.12377216812681e+44 */, 1LL},
            {1445723661896317830LL /* -0.0457178113775911 */, 1LL},
            {5035721527359772724LL /* 9.704343214299189e-59 */, 0LL},
        };
        // clang-format on

        for (auto const& test : tests)
        {
            BEAST_EXPECT(
                api.float_sign(std::get<0>(test)) == std::get<1>(test));
        }
    }

    void
    test_float_sto(FeatureBitset features)
    {
        testcase("Test float_sto");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        uint64_t const xfl = 6198187654261802496ULL;  // 1234567.0 as in SetHook
        // XRP serialization
        auto const ser = api.float_sto(std::nullopt, std::nullopt, xfl, 0, 8);
        BEAST_EXPECT(ser.has_value());
        auto const parsed = api.float_sto_set(ser.value());
        BEAST_EXPECT(parsed.has_value());
        BEAST_EXPECT(
            api.float_compare(parsed.value(), xfl, compare_mode::EQUAL)
                .value() == 1);
    }

    void
    test_float_sto_set(FeatureBitset features)
    {
        testcase("Test float_sto_set");

        using namespace jtx;
        using namespace hook_api;
        using namespace hook;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        // Not enough bytes
        BEAST_EXPECT(
            api.float_sto_set(Bytes{1, 2, 3}).error() == NOT_AN_OBJECT);

        uint64_t const xfl = 6198187654261802496ULL;  // 1234567.0 as in SetHook
        auto const ser = api.float_sto(std::nullopt, std::nullopt, xfl, 0, 8);
        BEAST_EXPECT(ser.has_value());
        auto const parsed = api.float_sto_set(ser.value());
        BEAST_EXPECT(parsed.has_value());
        BEAST_EXPECT(
            api.float_compare(parsed.value(), xfl, compare_mode::EQUAL)
                .value() == 1);
    }

    void
    test_float_sum(FeatureBitset features)
    {
        testcase("Test float_sum");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        auto const one = api.float_one();
        auto const neg_one = api.float_negate(api.float_one());

        // 1 + 1 = 2
        ASSERT_FLOAT_EQUAL(
            api, 6090866696204910592LL, api.float_sum(one, one).value());

        // 1 - 1 = 0
        ASSERT_FLOAT_EQUAL(api, 0, api.float_sum(one, neg_one).value());
        // 45678 + 0.345678 = 45678.345678
        ASSERT_FLOAT_EQUAL(
            api,
            6165492124810638528LL,
            api.float_sum(6165492090242838528LL, 6074309077695428608LL)
                .value());
        // -151864512641 + 100000000000000000 = 99999848135487359
        ASSERT_FLOAT_EQUAL(
            api,
            6387097057170171072LL,
            api.float_sum(1676857706508234512LL, 6396111470866104320LL)
                .value());

        // auto generated random sums
        // clang-format off
        std::vector<std::tuple<uint64_t, uint64_t, uint64_t>> tests = {
            {7607324992379065667 /* 5.248821377668419e+84 */, 95785354843184473 /* -5.713362295774553e-77 */, 7607324992379065667 /* 5.248821377668419e+84 */},
            {1011203427860697296 /* -2.397111329706192e-26 */, 7715811566197737722 /* 5.64900413944857e+90 */, 7715811566197737722 /* 5.64900413944857e+90 */},
            {6507979072644559603 /* 4.781210721563379e+23 */, 422214339164556094 /* -7.883173446470462e-59 */, 6507979072644559603 /* 4.781210721563379e+23 */},
            {129493221419941559 /* -3.392431853567671e-75 */, 6742079437952459317 /* 4.694395406197301e+36 */, 6742079437952459317 /* 4.694395406197301e+36 */},
            {5172806703808250354 /* 2.674331586920946e-51 */, 3070396690523275533 /* -7.948943911338253e+88 */, 3070396690523275533 /* -7.948943911338253e+88 */},
            {2440992231195047997 /* -9.048432414980156e+53 */, 4937813945440933271 /* 1.868753842869655e-64 */, 2440992231195047996 /* -9.048432414980156e+53 */},
            {7351918685453062372 /* 2.0440935844129e+70 */, 6489541496844182832 /* 4.358033430668592e+22 */, 7351918685453062372 /* 2.0440935844129e+70 */},
            {4960621423606196948 /* 6.661833498651348e-63 */, 6036716382996689576 /* 0.001892882320224936 */, 6036716382996689576 /* 0.001892882320224936 */},
            {1342689232407435206 /* -9.62374270576839e-8 */, 5629833007898276923 /* 9.340672939897915e-26 */, 1342689232407435206 /* -9.62374270576839e-8 */},
            {7557687707019793516 /* 9.65473154684222e+81 */, 528084028396448719 /* -5.666471621471183e-53 */, 7557687707019793516 /* 9.65473154684222e+81 */},
            {130151633377050812 /* -4.050843810676924e-75 */, 2525286695563827336 /* -3.270904236349576e+58 */, 2525286695563827336 /* -3.270904236349576e+58 */},
            {5051914485221832639 /* 7.88290256687712e-58 */, 7518727241611221951 /* 6.723063157234623e+79 */, 7518727241611221951 /* 6.723063157234623e+79 */},
            {3014788764095798870 /* -6.384213012307542e+85 */, 7425019819707800346 /* 3.087633801222938e+74 */, 3014788764095767995 /* -6.384213012276667e+85 */},
            {4918950856932792129 /* 1.020063844210497e-65 */, 7173510242188034581 /* 3.779635414204949e+60 */, 7173510242188034581 /* 3.779635414204949e+60 */},
            {20028000442705357 /* -2.013601933223373e-81 */, 95248745393457140 /* -5.17675284604722e-77 */, 95248946753650462 /* -5.176954206240542e-77 */},
            {5516870225060928024 /* 4.46428115944092e-32 */, 7357202055584617194 /* 7.327463715967722e+70 */, 7357202055584617194 /* 7.327463715967722e+70 */},
            {2326103538819088036 /* -2.2461310959121e+47 */, 1749360946246242122 /* -1964290826489674 */, 2326103538819088036 /* -2.2461310959121e+47 */},
            {1738010758208819410 /* -862850129854894.6 */, 2224610859005732191 /* -8.83984233944816e+41 */, 2224610859005732192 /* -8.83984233944816e+41 */},
            {4869534730307487904 /* 5.647132747352224e-68 */, 2166841923565712115 /* -5.114102427874035e+38 */, 2166841923565712115 /* -5.114102427874035e+38 */},
            {1054339559322014937 /* -9.504445772059864e-24 */, 1389511416678371338 /* -0.0000240273144825857 */, 1389511416678371338 /* -0.0000240273144825857 */},
        };
        // clang-format on

        for (auto const& test : tests)
        {
            ASSERT_FLOAT_EQUAL(
                api,
                api.float_sum(std::get<0>(test), std::get<1>(test)).value(),
                std::get<2>(test));
        }
    }

    void
    test_hook_account(FeatureBitset features)
    {
        testcase("Test hook_account");

        using namespace jtx;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        StubHookContext stubCtx{};
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
        hook::HookAPI api(hookCtx);

        BEAST_EXPECT(api.hook_account() == alice.id());
    }

    void
    test_hook_again(FeatureBitset features)
    {
        testcase("Test hook_again");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        {
            // Already requested
            StubHookContext stubCtx{};
            stubCtx.result.executeAgainAsWeak = true;
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            BEAST_EXPECT(hookCtx.result.executeAgainAsWeak);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.hook_again().error() == ALREADY_SET);
        }
        {
            // Strong hook requests weak re-exec
            StubHookContext stubCtx{};
            stubCtx.result.isStrong = true;
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(!hookCtx.result.executeAgainAsWeak);
            auto const result = api.hook_again();
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == 1);
            BEAST_EXPECT(hookCtx.result.executeAgainAsWeak);
        }

        {
            // Not strong -> prerequisite not met
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            BEAST_EXPECT(!hookCtx.result.executeAgainAsWeak);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.hook_again().error() == PREREQUISITE_NOT_MET);
        }
    }

    void
    test_hook_hash(FeatureBitset features)
    {
        testcase("Test hook_hash");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        env.fund(XRP(10000), alice);
        env.close();

        env(hook(
                alice,
                {{
                    hso(genesis::AcceptHook),
                    hso(genesis::MintTestHook),
                }},
                0),
            fee(XRP(100)));
        env.close();

        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        uint256 const expectedHash{2};
        StubHookContext stubCtx{
            .result = {.hookHash = expectedHash},
        };
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
        hook::HookAPI api(hookCtx);

        {
            // current hook hash
            auto const result = api.hook_hash(-1);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == expectedHash);
        }

        {
            // Does not exist
            auto const result = api.hook_hash(2);
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == DOESNT_EXIST);
        }

        {
            // Success index = 0
            auto const wasm = genesis::AcceptHook;
            auto const hash =
                ripple::sha512Half_s(ripple::Slice(wasm.data(), wasm.size()));
            auto const result = api.hook_hash(0);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == hash);
        }

        {
            // Success index = 1
            auto const wasm = genesis::MintTestHook;
            auto const hash =
                ripple::sha512Half_s(ripple::Slice(wasm.data(), wasm.size()));
            auto const result = api.hook_hash(1);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == hash);
        }
    }

    void
    test_hook_param(FeatureBitset features)
    {
        testcase("Test hook_param");

        using namespace jtx;
        using namespace hook_api;
        using namespace hook;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        {
            // TOO_SMALL / TOO_BIG
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.hook_param({}).error() == TOO_SMALL);
            BEAST_EXPECT(api.hook_param(Bytes(33, 1)).error() == TOO_BIG);
        }

        // {
        //     // Hook params map
        //     Bytes const name{'k'};
        //     Bytes const value{1, 2, 3};
        //     StubHookContext stubCtx{
        //         .result = {
        //             .hookParams = {{{name, value}}},
        //         }};
        //     auto hookCtx =
        //         makeStubHookContext(applyCtx, alice.id(), alice.id(),
        //         stubCtx);
        //     hook::HookAPI api(hookCtx);
        //     auto const result = api.hook_param(name);
        //     BEAST_EXPECT(result.has_value());
        //     BEAST_EXPECT(result.value() == value);
        // }

        {
            // Override deletion wins
            Bytes const name{'d'};
            StubHookContext stubCtx{};
            stubCtx.result.hookParamOverrides = {
                {stubCtx.result.hookHash, {{name, Bytes{}}}}};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.hook_param(name).error() == DOESNT_EXIST);
        }

        {
            // Override takes precedence
            Bytes const name{'o'};
            Bytes const overrideValue{9};
            StubHookContext stubCtx{};
            stubCtx.result.hookParams =
                std::map<Bytes, Bytes>{{name, Bytes{1}}};  // base value
            stubCtx.result.hookParamOverrides = {
                {stubCtx.result.hookHash, {{name, overrideValue}}}};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            auto const result = api.hook_param(name);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == overrideValue);
        }
    }

    void
    test_hook_param_set(FeatureBitset features)
    {
        testcase("Test hook_param_set");

        using namespace jtx;
        using namespace hook_api;
        using namespace hook;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hash = uint256{7};

        {
            // TOO_SMALL
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(
                api.hook_param_set(hash, {}, Bytes{1}).error() == TOO_SMALL);
        }

        {
            // TOO_BIG key/value and TOO_MANY_PARAMS
            StubHookContext stubCtx{};
            stubCtx.result.overrideCount = hook_api::max_params;
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(
                api.hook_param_set(
                       hash,
                       Bytes(hook::maxHookParameterKeySize() + 1, 1),
                       Bytes{})
                    .error() == TOO_BIG);
            BEAST_EXPECT(
                api.hook_param_set(
                       hash,
                       Bytes{1},
                       Bytes(hook::maxHookParameterValueSize() + 1, 1))
                    .error() == TOO_BIG);
            BEAST_EXPECT(
                api.hook_param_set(hash, Bytes{1}, Bytes{1}).error() ==
                TOO_MANY_PARAMS);
        }

        {
            // SUCCESS and override stored
            Bytes const name{'x'};
            Bytes const value{5, 6};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            auto const result = api.hook_param_set(hash, name, value);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == value.size());
            BEAST_EXPECT(hookCtx.result.overrideCount == 1);
            BEAST_EXPECT(
                hookCtx.result.hookParamOverrides[hash].at(name) == value);
        }
    }

    void
    test_hook_pos(FeatureBitset features)
    {
        testcase("Test hook_pos");

        using namespace jtx;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        StubHookContext stubCtx{};
        stubCtx.result.hookChainPosition = 3;
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
        hook::HookAPI api(hookCtx);

        BEAST_EXPECT(api.hook_pos() == 3);
    }

    void
    test_hook_skip(FeatureBitset features)
    {
        testcase("Test hook_skip");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        env.fund(XRP(10000), alice);
        env.close();
        env(hook(alice, {{hso(genesis::AcceptHook)}}, 0), fee(XRP(1)));
        env.close();

        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx = makeStubHookContext(
            applyCtx,
            alice.id(),
            alice.id(),
            {
                .result = {.hookSkips = {uint256{123}, uint256{456}}},
            });
        hook::HookAPI api(hookCtx);

        {
            // INVALID_ARGUMENT
            auto const result = api.hook_skip(uint256{0}, 2);
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == INVALID_ARGUMENT);
        }

        {
            // DOESNT_EXIST
            auto const result = api.hook_skip(uint256{1}, 0);
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == DOESNT_EXIST);
        }

        {
            // with Delete flag (1)
            auto result = api.hook_skip(uint256{0}, 1);
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == DOESNT_EXIST);
            BEAST_EXPECT(hookCtx.result.hookSkips.size() == 2);

            result = api.hook_skip(uint256{123}, 1);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == 1);
            BEAST_EXPECT(hookCtx.result.hookSkips.size() == 1);
        }

        {
            // already skipped
            auto result = api.hook_skip(uint256{456}, 0);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == 1);
            BEAST_EXPECT(hookCtx.result.hookSkips.size() == 1);
        }

        {
            // doesn't found
            auto result = api.hook_skip(uint256{123}, 0);
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == DOESNT_EXIST);
            BEAST_EXPECT(hookCtx.result.hookSkips.size() == 1);
        }

        {
            // success
            auto const wasm = genesis::AcceptHook;
            auto const hash =
                ripple::sha512Half_s(ripple::Slice(wasm.data(), wasm.size()));
            auto result = api.hook_skip(hash, 0);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == 1);
            BEAST_EXPECT(hookCtx.result.hookSkips.size() == 2);
            BEAST_EXPECT(
                hookCtx.result.hookSkips.find(hash) !=
                hookCtx.result.hookSkips.end());
        }
    }

    void
    test_ledger_keylet(FeatureBitset features)
    {
        testcase("Test ledger_keylet");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        env.fund(XRP(10000), alice);
        env.close();
        env(cron::set(alice), cron::startTime(1000), fee(XRP(1)));
        env.close();
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        {
            // does not match
            auto const result = api.ledger_keylet(
                keylet::account(alice), keylet::ownerDir(alice));
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == DOES_NOT_MATCH);
        }
        {
            // does not exist
            auto const result =
                api.ledger_keylet(keylet::cron(100), keylet::cron(101));
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == DOESNT_EXIST);
        }

        // Success case
        {
            auto const lo = keylet::cron(1000);
            auto const hi = keylet::cron(1001);
            auto const result = api.ledger_keylet(lo, hi);
            BEAST_EXPECT(result.has_value());
            auto const expected = keylet::cron(1000, alice);
            BEAST_EXPECT(result.value().type == expected.type);
            BEAST_EXPECT(result.value().key == expected.key);
        }
    }

    void
    test_ledger_last_hash(FeatureBitset features)
    {
        testcase("Test ledger_last_hash");

        using namespace jtx;
        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        BEAST_EXPECT(api.ledger_last_hash() == ov.info().parentHash);
    }

    void
    test_ledger_last_time(FeatureBitset features)
    {
        testcase("Test ledger_last_time");

        using namespace jtx;
        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        auto const expected =
            ov.info().parentCloseTime.time_since_epoch().count();
        BEAST_EXPECT(api.ledger_last_time() == expected);
    }

    void
    test_ledger_nonce(FeatureBitset features)
    {
        testcase("Test ledger_nonce");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        {
            // TOO_MANY_NONCES
            StubHookContext stubCtx{
                .ledger_nonce_counter =
                    static_cast<uint16_t>(hook_api::max_nonce + 1)};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            auto const result = api.ledger_nonce();
            BEAST_EXPECT(result.error() == TOO_MANY_NONCES);
        }

        {
            // SUCCESS
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            auto const info = hookCtx.applyCtx.view().info();
            auto const expected = ripple::sha512Half(
                ripple::HashPrefix::hookNonce,
                info.seq,
                info.parentCloseTime.time_since_epoch().count(),
                info.parentHash,
                hookCtx.applyCtx.tx.getTransactionID(),
                hookCtx.ledger_nonce_counter,
                hookCtx.result.account);
            auto const result = api.ledger_nonce();
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == expected);
            BEAST_EXPECT(hookCtx.ledger_nonce_counter == 1);
        }
    }

    void
    test_ledger_seq(FeatureBitset features)
    {
        testcase("Test ledger_seq");

        using namespace jtx;
        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        BEAST_EXPECT(api.ledger_seq() == ov.info().seq);
    }

    void
    test_meta_slot(FeatureBitset features)
    {
        testcase("Test meta_slot");

        using namespace jtx;
        using namespace hook_api;
        auto const alice = Account{"alice"};
        Env env{*this, features};
        env.fund(XRP(10000), alice);

        env(noop(alice));
        env.close();

        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx = makeStubHookContext(
            applyCtx,
            alice.id(),
            alice.id(),
            {.result = {.provisionalMeta = env.meta()}});
        hook::HookAPI api(hookCtx);

        {
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            // prerequisite not met
            auto const result = api.meta_slot(0);
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == PREREQUISITE_NOT_MET);
        }

        {
            // invalid argument
            auto const result = api.meta_slot(hook_api::max_slots + 1);
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == INVALID_ARGUMENT);
        }

        {
            auto hookCtx = makeStubHookContext(
                applyCtx,
                alice.id(),
                alice.id(),
                {.result = {.provisionalMeta = env.meta()}});
            for (uint32_t i = 1; i <= hook_api::max_slots; ++i)
                hookCtx.slot[i] = hook::SlotEntry{};
            // no free slots
            hook::HookAPI api(hookCtx);
            auto const result = api.meta_slot(0);
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == NO_FREE_SLOTS);
        }

        {
            hook::HookAPI api(hookCtx);
            auto const result = api.meta_slot(0);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == 1);

            BEAST_EXPECT(*hookCtx.slot[1].entry == *env.meta());
        }
    }

    void
    test_xpop_slot(FeatureBitset features)
    {
        testcase("Test xpop_slot");

        using namespace jtx;
        using namespace hook_api;
        auto const alice = Account{"alice"};

        Env env{*this, features};

        STTx invokeTx = STTx(ttIMPORT, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        {
            STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            // invalid transaction type
            auto const result = api.xpop_slot(0, 0);
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == PREREQUISITE_NOT_MET);
        }

        {
            // invalid slot number
            auto const result1 = api.xpop_slot(hook_api::max_slots + 1, 0);
            BEAST_EXPECT(!result1.has_value());
            BEAST_EXPECT(result1.error() == INVALID_ARGUMENT);

            auto const result2 = api.xpop_slot(0, hook_api::max_slots + 1);
            BEAST_EXPECT(!result2.has_value());
            BEAST_EXPECT(result2.error() == INVALID_ARGUMENT);
        }

        {
            // no free slots
            for (uint32_t i = 1; i <= hook_api::max_slots - 1; ++i)
                hookCtx.slot[i] = hook::SlotEntry{};
            hook::HookAPI api(hookCtx);
            auto const result = api.xpop_slot(0, 0);
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == NO_FREE_SLOTS);
        }

        {
            // same slot number for both
            auto const result = api.xpop_slot(1, 1);
            BEAST_EXPECT(!result.has_value());
            BEAST_EXPECT(result.error() == INVALID_ARGUMENT);
        }

        // TODO: test INVALID_TXN

        {
            // Success
            auto const xpopJson = import::loadXpop(ImportTCAccountSet::w_seed);
            std::string xpopStr = Json::FastWriter().write(xpopJson);
            STTx invokeTx = STTx(ttIMPORT, [&](STObject& obj) {
                obj.setFieldVL(sfBlob, *strUnHex(strHex(xpopStr)));
            });
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            auto const result = api.xpop_slot(0, 0);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value().first == 1);
            BEAST_EXPECT(result.value().second == 2);
            Serializer stx, smeta;
            hookCtx.slot[1].entry->add(stx);
            hookCtx.slot[2].entry->add(smeta);
            stx.getData();
            smeta.getData();

            std::string blob = strHex(stx.getData());
            std::string meta = strHex(smeta.getData());
            BEAST_EXPECT(xpopJson["transaction"]["blob"] == blob);
            BEAST_EXPECT(xpopJson["transaction"]["meta"] == meta);
        }
    }

    void
    test_otxn_burden(FeatureBitset features)
    {
        testcase("Test otxn_burden");
        using namespace jtx;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx baseTx = STTx(ttINVOKE, [&](STObject& obj) {});

        {
            // Cached burden
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, baseTx);
            StubHookContext stubCtx{.burden = 7};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.otxn_burden() == 7);
        }

        {
            // No sfEmitDetails
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, baseTx);
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.otxn_burden() == 1);
        }

        {
            // sfEmitDetails without sfEmitBurden
            STTx tx = STTx(ttINVOKE, [&](STObject& obj) {
                obj.peekFieldObject(sfEmitDetails);
            });
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, tx);
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.otxn_burden() == 1);
        }

        {
            // sfEmitBurden with high bit set is masked
            STTx tx = STTx(ttINVOKE, [&](STObject& obj) {
                auto& details = obj.peekFieldObject(sfEmitDetails);
                details.setFieldU64(
                    sfEmitBurden, (static_cast<uint64_t>(1) << 63) | 25);
            });
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, tx);
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            auto const burden = api.otxn_burden();
            BEAST_EXPECT(burden == 25);
            BEAST_EXPECT(api.otxn_burden() == burden);
        }
    }

    void
    test_otxn_generation(FeatureBitset features)
    {
        testcase("Test otxn_generation");
        using namespace jtx;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx baseTx = STTx(ttINVOKE, [&](STObject& obj) {});

        {
            // Cached generation
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, baseTx);
            StubHookContext stubCtx{.generation = 9};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.otxn_generation() == 9);
        }

        {
            // No sfEmitDetails
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, baseTx);
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.otxn_generation() == 0);
        }

        {
            // sfEmitDetails without sfEmitGeneration
            STTx tx = STTx(ttINVOKE, [&](STObject& obj) {
                obj.peekFieldObject(sfEmitDetails);
            });
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, tx);
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.otxn_generation() == 0);
        }

        {
            // sfEmitGeneration present
            STTx tx = STTx(ttINVOKE, [&](STObject& obj) {
                obj.peekFieldObject(sfEmitDetails)
                    .setFieldU32(sfEmitGeneration, 4);
            });
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, tx);
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.otxn_generation() == 4);
            BEAST_EXPECT(api.otxn_generation() == 4);
        }
    }

    void
    test_otxn_field(FeatureBitset features)
    {
        testcase("Test otxn_field");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx tx =
            STTx(ttINVOKE, [&](STObject& obj) { obj[sfAccount] = alice.id(); });
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, tx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        // TODO: INVALID_FIELD now returns DOESNT_EXIST
        // BEAST_EXPECT(api.otxn_field(0).error() == INVALID_FIELD);
        BEAST_EXPECT(
            api.otxn_field(sfDestination.getCode()).error() == DOESNT_EXIST);
        auto const result = api.otxn_field(sfAccount.getCode());
        BEAST_EXPECT(result.has_value());
        auto const* acct = dynamic_cast<STAccount const*>(result.value());
        BEAST_EXPECT(acct != nullptr);
        BEAST_EXPECT(acct->value() == alice.id());
    }

    void
    test_otxn_id(FeatureBitset features)
    {
        testcase("Test otxn_id");

        using namespace jtx;

        auto const alice = Account{"alice"};
        Env env{*this, features};

        {
            STTx tx = STTx(ttINVOKE, [&](STObject& obj) {});
            auto const txID = tx.getTransactionID();
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, tx);
            // Originating transaction ID
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            auto const result = api.otxn_id(0);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() == txID);
        }

        {
            STTx tx = STTx(ttEMIT_FAILURE, [&](STObject& obj) {
                obj.setFieldH256(sfTransactionHash, uint256{5});
            });
            STTx emitFailedTx = STTx(ttINVOKE, [&](STObject& obj) {});
            auto const txID = tx.getTransactionID();
            OpenView ov{*env.current()};
            ApplyContext applyCtx = createApplyContext(env, ov, tx);
            // Emit failure transaction ID
            StubHookContext stubCtx{.emitFailure = emitFailedTx};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(
                api.otxn_id(0).value() == tx.getFieldH256(sfTransactionHash));
            // flags bypass emitFailure
            BEAST_EXPECT(api.otxn_id(1).value() == txID);
        }
    }

    void
    test_otxn_slot(FeatureBitset features)
    {
        testcase("Test otxn_slot");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx tx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, tx);

        {
            // Invalid slot argument
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            auto const result = api.otxn_slot(hook_api::max_slots + 1);
            BEAST_EXPECT(result.error() == INVALID_ARGUMENT);
        }

        {
            // No free slots
            StubHookContext stubCtx{};
            for (uint32_t i = 1; i <= hook_api::max_slots; ++i)
                stubCtx.slot[i] = hook::SlotEntry{};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            for (uint32_t i = 1; i <= hook_api::max_slots; ++i)
                hookCtx.slot[i] = hook::SlotEntry{};
            BEAST_EXPECT(stubCtx.slot.size() == hook_api::max_slots);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.otxn_slot(0).error() == NO_FREE_SLOTS);
        }

        {
            // SUCCESS allocate new slot (slot = 0)
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            for (uint32_t i = 1; i <= 111; ++i)
                hookCtx.slot[i] = hook::SlotEntry{};
            BEAST_EXPECT(!hookCtx.slot.contains(112));
            hook::HookAPI api(hookCtx);
            auto const result = api.otxn_slot(0);
            BEAST_EXPECT(result.has_value());
            auto const newSlot = result.value();
            BEAST_EXPECT(newSlot == 112);
            BEAST_EXPECT(hookCtx.slot.contains(112));
            BEAST_EXPECT(hookCtx.slot[112].entry != nullptr);
            // TODO: test slot content
        }

        {
            // SUCCESS allocate new slot (slot != 0)
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            BEAST_EXPECT(!hookCtx.slot.contains(200));
            hook::HookAPI api(hookCtx);
            auto const result = api.otxn_slot(200);
            BEAST_EXPECT(result.has_value());
            auto const newSlot = result.value();
            BEAST_EXPECT(newSlot == 200);
            BEAST_EXPECT(hookCtx.slot.contains(200));
            BEAST_EXPECT(hookCtx.slot[newSlot].entry != nullptr);
            // TODO: test slot content
        }
    }

    void
    test_otxn_type(FeatureBitset features)
    {
        testcase("Test otxn_type");

        using namespace jtx;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx tx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, tx);

        {
            STObject failure(sfHook);
            failure.setFieldU16(sfTransactionType, ttACCOUNT_SET);
            StubHookContext stubCtx{.emitFailure = failure};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.otxn_type() == ttACCOUNT_SET);
        }

        {
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.otxn_type() == ttINVOKE);
        }
    }

    void
    test_otxn_param(FeatureBitset features)
    {
        testcase("Test otxn_param");

        using namespace jtx;
        using namespace hook_api;
        using namespace hook;

        auto const alice = Account{"alice"};
        Env env{*this, features};

        // Build tx with hook parameters
        STTx tx = STTx(ttINVOKE, [&](STObject& obj) {
            STArray params{sfHookParameters, 2};
            {
                STObject p{sfHookParameter};
                p.setFieldVL(sfHookParameterName, Bytes{'a'});
                p.setFieldVL(sfHookParameterValue, Bytes{1, 2});
                params.emplace_back(std::move(p));
            }
            {
                STObject p{sfHookParameter};
                p.setFieldVL(sfHookParameterName, Bytes{'b'});
                // missing value to test DOESNT_EXIST
                params.emplace_back(std::move(p));
            }
            {
                STObject p{sfHookParameter};
                p.setFieldVL(sfHookParameterName, Bytes{'c'});
                // empty value to test DOESNT_EXIST
                p.setFieldVL(sfHookParameterValue, Bytes{});
                params.emplace_back(std::move(p));
            }
            obj.setFieldArray(sfHookParameters, params);
        });

        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, tx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        BEAST_EXPECT(api.otxn_param({}).error() == TOO_SMALL);
        BEAST_EXPECT(api.otxn_param(Bytes(33, 1)).error() == TOO_BIG);
        auto const result = api.otxn_param(Bytes{'a'});
        BEAST_EXPECT(result.has_value());
        BEAST_EXPECT(result.value() == Bytes({1, 2}));
        BEAST_EXPECT(api.otxn_param(Bytes{'b'}).error() == DOESNT_EXIST);
        BEAST_EXPECT(api.otxn_param(Bytes{'c'}).error() == DOESNT_EXIST);
        BEAST_EXPECT(api.otxn_param(Bytes{'d'}).error() == DOESNT_EXIST);
    }

    void
    test_slot(FeatureBitset features)
    {
        testcase("Test slot");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        // Missing slot
        {
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.slot(1).error() == DOESNT_EXIST);
        }

        // Present slot pointing to an STAmount field
        {
            printf("test\n");
            STObject obj(sfGeneric);
            obj.setFieldAmount(sfAmount, drops(1));
            auto storage = std::make_shared<STObject const>(obj);
            StubHookContext stubCtx{};
            stubCtx.slot[1] = hook::SlotEntry{.storage = storage, .entry = 0};
            stubCtx.slot[1].entry = &(*stubCtx.slot[1].storage);
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            auto const result = api.slot(1);
            BEAST_EXPECT(result.has_value());

            Serializer s;
            (*result)->add(s);

            STObject resultObj{s.slice(), sfGeneric};
            printf(
                "resultObj: %s\n",
                resultObj.getJson(JsonOptions::none).toStyledString().c_str());
        }
    }

    void
    test_slot_clear(FeatureBitset features)
    {
        testcase("Test slot_clear");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        // Missing slot
        {
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.slot_clear(1).error() == DOESNT_EXIST);
        }

        // Clear existing slot and push to free queue
        {
            STObject obj(sfGeneric);
            auto storage = std::make_shared<STObject const>(obj);
            StubHookContext stubCtx{};
            stubCtx.slot[2] = {.storage = storage, .entry = &(*storage)};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.slot_clear(2).has_value());
            BEAST_EXPECT(hookCtx.slot.find(2) == hookCtx.slot.end());
            BEAST_EXPECT(!hookCtx.slot_free.empty());
        }
    }

    void
    test_slot_count(FeatureBitset features)
    {
        testcase("Test slot_count");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        // Nonexistent
        {
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.slot_count(1).error() == DOESNT_EXIST);
        }

        // Not an array
        {
            STObject obj(sfGeneric);
            auto storage = std::make_shared<STObject const>(obj);
            StubHookContext stubCtx{};
            stubCtx.slot[1] = {.storage = storage, .entry = &(*storage)};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.slot_count(1).error() == NOT_AN_ARRAY);
        }

        // Array count
        {
            STArray arr{sfGeneric};
            arr.emplace_back(STObject(sfGeneric));
            arr.emplace_back(STObject(sfGeneric));
            auto storage = std::make_shared<STArray const>(arr);
            StubHookContext stubCtx{};
            stubCtx.slot[3] = {
                .storage =
                    std::reinterpret_pointer_cast<const STObject>(storage),
                .entry = &(*storage)};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.slot_count(3).value() == 2);
        }
    }

    void
    test_slot_float(FeatureBitset features)
    {
        testcase("Test slot_float");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        // Missing slot
        {
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.slot_float(1).error() == DOESNT_EXIST);
        }

        // Not an amount
        {
            STObject obj(sfGeneric);
            auto storage = std::make_shared<STObject const>(obj);
            StubHookContext stubCtx{};
            stubCtx.slot[1] = {.storage = storage, .entry = &(*storage)};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.slot_float(1).error() == NOT_AN_AMOUNT);
        }

        // Native amount
        {
            STObject obj(sfGeneric);
            obj.setFieldAmount(sfAmount, drops(10));
            auto storage = std::make_shared<STObject const>(obj);
            auto* amtPtr = &storage->getFieldAmount(sfAmount);
            StubHookContext stubCtx{};
            stubCtx.slot[2] = {.storage = storage, .entry = amtPtr};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            auto const result = api.slot_float(2);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() != 0);
        }
    }

    void
    test_slot_set(FeatureBitset features)
    {
        testcase("Test slot_set");

        using namespace jtx;
        using namespace hook;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        // Invalid argument (wrong size)
        BEAST_EXPECT(
            api.slot_set(Bytes{1, 2, 3}, 0).error() == INVALID_ARGUMENT);
        // Invalid argument (slot_no beyond max)
        BEAST_EXPECT(
            api.slot_set(Bytes(32, 0), hook_api::max_slots + 1).error() ==
            INVALID_ARGUMENT);
    }

    void
    test_slot_size(FeatureBitset features)
    {
        testcase("Test slot_size");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        // Missing slot
        {
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.slot_size(1).error() == DOESNT_EXIST);
        }

        // Size of STObject
        {
            STObject obj(sfGeneric);
            obj.setFieldAmount(sfAmount, drops(1));
            auto storage = std::make_shared<STObject const>(obj);
            StubHookContext stubCtx{};
            stubCtx.slot[1] = {.storage = storage, .entry = &(*storage)};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.slot_size(1).value() > 0);
        }
    }

    void
    test_slot_subarray(FeatureBitset features)
    {
        testcase("Test slot_subarray");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        // Missing parent
        {
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.slot_subarray(1, 0, 0).error() == DOESNT_EXIST);
        }

        // Valid subarray extraction
        {
            STArray arr{sfGeneric};
            arr.emplace_back(STObject(sfGeneric));
            arr.emplace_back(STObject(sfGeneric));
            auto storage = std::make_shared<STArray const>(arr);
            StubHookContext stubCtx{};
            stubCtx.slot[1] = {
                .storage =
                    std::reinterpret_pointer_cast<const STObject>(storage),
                .entry = &(*storage)};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            auto const result = api.slot_subarray(1, 1, 0);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() != 0);
            BEAST_EXPECT(hookCtx.slot[result.value()].entry != nullptr);
        }
    }

    void
    test_slot_subfield(FeatureBitset features)
    {
        testcase("Test slot_subfield");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        // Missing parent
        {
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(
                api.slot_subfield(1, sfAccount.getCode(), 0).error() ==
                DOESNT_EXIST);
        }

        // No free slots
        {
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            for (int i = 0; i < hook_api::max_slots; i++)
                hookCtx.slot[i] = hook::SlotEntry{};
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(
                api.slot_subfield(1, sfAccount.getCode(), 0).error() ==
                NO_FREE_SLOTS);
            BEAST_EXPECT(
                api.slot_subfield(
                       1, sfAccount.getCode(), hook_api::max_slots + 1)
                    .error() == INVALID_ARGUMENT);
        }

        // Invalid field
        {
            STObject obj(sfGeneric);
            auto storage = std::make_shared<STObject const>(obj);
            StubHookContext stubCtx{};
            stubCtx.slot[1] = {.storage = storage, .entry = &(*storage)};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(
                api.slot_subfield(1, ((99U << 16U) + 99U), 0).error() ==
                INVALID_FIELD);
            BEAST_EXPECT(
                api.slot_subfield(1, sfAccount.getCode(), 0).error() ==
                DOESNT_EXIST);
        }

        // Valid field
        {
            STObject obj(sfGeneric);
            obj.setFieldAmount(sfAmount, drops(1));
            auto storage = std::make_shared<STObject const>(obj);
            StubHookContext stubCtx{};
            stubCtx.slot[1] = {.storage = storage, .entry = &(*storage)};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            auto const result = api.slot_subfield(1, sfAmount.getCode(), 0);
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(result.value() != 0);
            BEAST_EXPECT(hookCtx.slot[result.value()].entry != nullptr);
            // test the amount bytes
        }
    }

    void
    test_slot_type(FeatureBitset features)
    {
        testcase("Test slot_type");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);

        // Missing slot
        {
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
            hook::HookAPI api(hookCtx);
            BEAST_EXPECT(api.slot_type(1, 0).error() == DOESNT_EXIST);
        }

        // Generic object
        {
            STObject obj(sfGeneric);
            obj.setFieldAmount(sfAmount, drops(5));
            auto storage = std::make_shared<STObject const>(obj);
            StubHookContext stubCtx{};
            stubCtx.slot[1] = {.storage = storage, .entry = &(*storage)};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            auto const result = api.slot_type(1, 0);
            BEAST_EXPECT(result.has_value());
        }

        // Amount with flag 1
        {
            STObject obj(sfGeneric);
            obj.setFieldAmount(sfAmount, drops(7));
            auto storage = std::make_shared<STObject const>(obj);
            auto* amtPtr = &storage->getFieldAmount(sfAmount);
            StubHookContext stubCtx{};
            stubCtx.slot[2] = {.storage = storage, .entry = amtPtr};
            auto hookCtx =
                makeStubHookContext(applyCtx, alice.id(), alice.id(), stubCtx);
            hook::HookAPI api(hookCtx);
            auto const result = api.slot_type(2, 1);
            BEAST_EXPECT(result.has_value());
        }
    }

    void
    test_state(FeatureBitset features)
    {
        testcase("Test state");

        BEAST_EXPECT(true);
    }

    void
    test_state_foreign(FeatureBitset features)
    {
        testcase("Test state_foreign");

        BEAST_EXPECT(true);
    }

    void
    test_state_foreign_set_max(FeatureBitset features)
    {
        testcase("Test state_foreign_set max");

        BEAST_EXPECT(true);
    }

    void
    test_state_foreign_set(FeatureBitset features)
    {
        testcase("Test state_foreign_set");

        BEAST_EXPECT(true);
    }

    void
    test_state_set(FeatureBitset features)
    {
        testcase("Test state_set");

        BEAST_EXPECT(true);
    }

    void
    test_sto_emplace(FeatureBitset features)
    {
        testcase("Test sto_emplace");

        using namespace jtx;
        using namespace hook;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        {
            // Invalid argument (wrong source_object size)
            auto const small = Bytes(1, 0);
            auto const large = Bytes(1024 * 16 + 1, 0);

            auto const small_result =
                api.sto_emplace(small, std::nullopt, sfAccount.getCode());
            BEAST_EXPECT(!small_result.has_value());
            BEAST_EXPECT(small_result.error() == TOO_SMALL);

            auto const large_result =
                api.sto_emplace(large, std::nullopt, sfAccount.getCode());
            BEAST_EXPECT(!large_result.has_value());
            BEAST_EXPECT(large_result.error() == TOO_BIG);
        }

        {
            // Invalid argument (wrong field_object size)
            auto const source = Bytes(16, 0);
            auto const small = Bytes(1, 0);
            auto const large = Bytes(4096 + 1, 0);

            auto const small_result =
                api.sto_emplace(source, small, sfAccount.getCode());
            BEAST_EXPECT(!small_result.has_value());
            BEAST_EXPECT(small_result.error() == TOO_SMALL);

            auto const large_result =
                api.sto_emplace(source, large, sfAccount.getCode());
            BEAST_EXPECT(!large_result.has_value());
            BEAST_EXPECT(large_result.error() == TOO_BIG);
        }

        {
            // TODO: test parse error
        }

        {
            // Success
            auto const source_object =
                Bytes({0x81U, 0x14U, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                       0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0});
            // Account: "<ZERO_ACCOUNT>"
            auto const field_object =
                Bytes({0x24U, 0x00U, 0x00U, 0x00U, 0x00U});
            // Sequence: 0

            auto const result = api.sto_emplace(
                source_object, field_object, sfSequence.getCode());
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(
                result.value().size() ==
                source_object.size() + field_object.size());
            BEAST_EXPECT(
                result.value() ==
                Bytes({0x24U, 0x00U, 0x00U, 0x00U, 0x00U, 0x81U, 0x14U, 0, 0,
                       0,     0,     0,     0,     0,     0,     0,     0, 0,
                       0,     0,     0,     0,     0,     0,     0,     0, 0}));
        }

        auto const _source_object =
            Bytes({0x81U, 0x14U, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                   0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0});
        {
            // use UINT16
            auto source_object = _source_object;
            auto field_object = Bytes({0x10U, 0x10U, 0x00U, 0x01U});
            // Version: 1

            auto const result = api.sto_emplace(
                source_object, field_object, sfVersion.getCode());
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(
                result.value().size() ==
                source_object.size() + field_object.size());
            field_object.insert(
                field_object.end(), source_object.begin(), source_object.end());
            BEAST_EXPECT(result.value() == field_object);
        }

        {
            // use UINT32
            auto source_object = _source_object;
            auto field_object = Bytes({0x24U, 0x00U, 0x00U, 0x00U, 0x01U});
            // Sequence: 1

            auto const result = api.sto_emplace(
                source_object, field_object, sfSequence.getCode());
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(
                result.value().size() ==
                source_object.size() + field_object.size());
            field_object.insert(
                field_object.end(), source_object.begin(), source_object.end());
            BEAST_EXPECT(result.value() == field_object);
        }

        {
            // use UINT64
            auto source_object = _source_object;
            auto field_object = Bytes(
                {0x36U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x01U});
            // ExchangeRate: 1

            auto const result = api.sto_emplace(
                source_object, field_object, sfExchangeRate.getCode());
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(
                result.value().size() ==
                source_object.size() + field_object.size());
            field_object.insert(
                field_object.end(), source_object.begin(), source_object.end());
            BEAST_EXPECT(result.value() == field_object);
        }

        {
            // use UINT128
            auto source_object = _source_object;
            auto field_object = Bytes(
                {0x41U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U});
            // EmailHash: 1

            auto const result = api.sto_emplace(
                source_object, field_object, sfEmailHash.getCode());
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(
                result.value().size() ==
                source_object.size() + field_object.size());
            field_object.insert(
                field_object.end(), source_object.begin(), source_object.end());
            BEAST_EXPECT(result.value() == field_object);
        }

        {
            // use UINT256
            auto source_object = _source_object;
            auto field_object = Bytes(
                {0x5EU, 0x00U, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                 0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
            // ObjectID:
            // "0000000000000000000000000000000000000000000000000000000000000000"

            auto const result = api.sto_emplace(
                source_object, field_object, sfObjectID.getCode());
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(
                result.value().size() ==
                source_object.size() + field_object.size());
            field_object.insert(
                field_object.end(), source_object.begin(), source_object.end());
            BEAST_EXPECT(result.value() == field_object);
        }

        {
            // use AMOUNT
            auto source_object = _source_object;

            auto nativeamount = Bytes(
                {0x61U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,
                 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,
                 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,
                 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,
                 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,
                 0x99U, 0x99U, 0x99U, 0x99U, 0x99U});
            auto iouamount = Bytes(
                {0x61U,
                 0x99U,
                 0x99U,
                 0x99U,
                 0x99U,
                 0x99U,
                 0x99U,
                 0x99U,
                 0x99U});

            for (auto field_object : {nativeamount, iouamount})
            {
                auto const result = api.sto_emplace(
                    source_object, field_object, sfAmount.getCode());
                BEAST_EXPECT(result.has_value());
                BEAST_EXPECT(
                    result.value().size() ==
                    source_object.size() + field_object.size());
                field_object.insert(
                    field_object.end(),
                    source_object.begin(),
                    source_object.end());
                BEAST_EXPECT(result.value() == field_object);
            }
        }

        {
            // OBJECT
            auto source_object = _source_object;
            auto field_object = Bytes(
                {0xE0U,
                 0x5BU,
                 0x61U,
                 0x40U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x00U,
                 0x64U,
                 0xE1U});
            // {"AmountEntry": {"Amount": "100"}}

            auto const result = api.sto_emplace(
                source_object, field_object, sfAmountEntry.getCode());
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(
                result.value().size() ==
                source_object.size() + field_object.size());
            source_object.insert(
                source_object.end(), field_object.begin(), field_object.end());
            BEAST_EXPECT(result.value() == source_object);
        }

        {
            // ARRAY
            auto source_object = _source_object;
            auto field_object = Bytes(
                {0xF9U,
                 0xEAU,
                 0x7DU,
                 0x04U,
                 0xDEU,
                 0xADU,
                 0xBEU,
                 0xEFU,
                 0xE1U,
                 0xF1U});
            // {"Memos": [{"Memo":{ "MemoData": "DEADBEEF" }}]}

            auto const result =
                api.sto_emplace(source_object, field_object, sfMemos.getCode());
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(
                result.value().size() ==
                source_object.size() + field_object.size());
            source_object.insert(
                source_object.end(), field_object.begin(), field_object.end());
            BEAST_EXPECT(result.value() == source_object);
        }

        {
            // UINT8
            auto source_object = _source_object;
            auto field_object = Bytes({0x00U, 0x10U, 0x10U, 0x01U});
            // {"TickSize": 1}

            auto const result = api.sto_emplace(
                source_object, field_object, sfTickSize.getCode());
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(
                result.value().size() ==
                source_object.size() + field_object.size());
            source_object.insert(
                source_object.end(), field_object.begin(), field_object.end());
            BEAST_EXPECT(result.value() == source_object);
        }

        {
            // UINT160
            auto source_object = _source_object;
            auto field_object =
                Bytes({0x01U, 0x11U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                       0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x55U, 0x53U,
                       0x44U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U});
            // {"TakerPaysCurrency": "0000000000000000000000005553440000000000"}

            auto const result = api.sto_emplace(
                source_object, field_object, sfTakerPaysCurrency.getCode());
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(
                result.value().size() ==
                source_object.size() + field_object.size());
            source_object.insert(
                source_object.end(), field_object.begin(), field_object.end());
            BEAST_EXPECT(result.value() == source_object);
        }

        {
            // PATHSET
            auto source_object = _source_object;
            auto field_object = Bytes(
                {0x01U, 0x12U, 0x30U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x55U, 0x53U, 0x44U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x05U, 0x4FU, 0x6FU, 0x78U,
                 0x4AU, 0x58U, 0xF9U, 0xEFU, 0xB0U, 0xA9U, 0xEBU, 0x90U, 0xB8U,
                 0x34U, 0x64U, 0xF9U, 0xD1U, 0x66U, 0x46U, 0x19U, 0x00U});
            // {"Paths": [[{ "currency": "USD", "issuer":
            // "rVnYNK9yuxBz4uP8zC8LEFokM2nqH3poc" }]]}

            auto const result =
                api.sto_emplace(source_object, field_object, sfPaths.getCode());
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(
                result.value().size() ==
                source_object.size() + field_object.size());
            source_object.insert(
                source_object.end(), field_object.begin(), field_object.end());
            BEAST_EXPECT(result.value() == source_object);
        }

        {
            // VECTOR256
            auto source_object = _source_object;
            auto field_object = Bytes(
                {0x03U, 0x13U, 0x20U, 0x42U, 0x42U, 0x6CU, 0x4DU, 0x4FU, 0x10U,
                 0x09U, 0xEEU, 0x67U, 0x08U, 0x0AU, 0x9BU, 0x79U, 0x65U, 0xB4U,
                 0x46U, 0x56U, 0xD7U, 0x71U, 0x4DU, 0x10U, 0x4AU, 0x72U, 0xF9U,
                 0xB4U, 0x36U, 0x9FU, 0x97U, 0xABU, 0xF0U, 0x44U, 0xEEU});
            // {"Amendments":["42426C4D4F1009EE67080A9B7965B44656D7714D104A72F9B4369F97ABF044EE"]}

            auto const result =
                api.sto_emplace(source_object, field_object, sfPaths.getCode());
            BEAST_EXPECT(result.has_value());
            BEAST_EXPECT(
                result.value().size() ==
                source_object.size() + field_object.size());
            source_object.insert(
                source_object.end(), field_object.begin(), field_object.end());
            BEAST_EXPECT(result.value() == source_object);
        }

        {
            // UINT96
        }

        {
            // UINT384
        }

        {
            // UINT512
        }
    }

    void
    test_sto_erase(FeatureBitset features)
    {
        testcase("Test sto_erase");

        BEAST_EXPECT(true);
    }
    void
    test_sto_subarray(FeatureBitset features)
    {
        testcase("Test sto_subarray");

        using namespace jtx;
        using namespace hook;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        {
            // Invalid data size
            BEAST_EXPECT(api.sto_subarray(Bytes{}, 0).error() == TOO_SMALL);
            BEAST_EXPECT(api.sto_subarray(Bytes{0x00}, 0).error() == TOO_SMALL);
        }

        {
            // Invalid: wrapped but size = 0 ([])
            // { Memos: [] }
            BEAST_EXPECT(
                api.sto_subarray(Bytes{0xF9, 0xF1}, 0).error() == PARSE_ERROR);
            // { Amounts: [] }
            BEAST_EXPECT(
                api.sto_subarray(Bytes{0xF0, 0x5C, 0xF1}, 0).error() ==
                PARSE_ERROR);
        }

        {
            // doesn't found
            // { Memos: [{Memo: {MemoData: "BEEF"}}] }
            auto const memos = *strUnHex("F9EA7D02BEEFE1F1");
            BEAST_EXPECT(api.sto_subarray(memos, 2).error() == DOESNT_EXIST);
            // { Amounts: [{AmountEntry: {Amount: "100"}}] }
            auto const amounts = *strUnHex("F05CE05B614000000000000064E1F1");
            // TODO: fix this
            // BEAST_EXPECT(api.sto_subarray(amounts, 2).error() ==
            // DOESNT_EXIST);
        }

        {
            // success
            // { Memos: [{Memo: {MemoData: "BEEF"}}] }
            auto const memos = *strUnHex("F9EA7D02BEEFE1F1");
            BEAST_EXPECT(
                api.sto_subarray(memos, 0).value() == std::make_pair(1, 6));
            // { Amounts: [{AmountEntry: {Amount: "100"}}] }
            auto const amounts = *strUnHex("F05CE05B614000000000000064E1F1");
            // TODO: fix this
            // BEAST_EXPECT(
            //     api.sto_subarray(amounts, 0).value() == std::make_pair(2,
            //     12));
        }
    }

    void
    test_sto_subfield(FeatureBitset features)
    {
        testcase("Test sto_subfield");

        using namespace jtx;
        using namespace hook;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        {
            // Invalid data size
            BEAST_EXPECT(api.sto_subfield(Bytes{}, 0).error() == TOO_SMALL);
            BEAST_EXPECT(api.sto_subfield(Bytes{0x00}, 0).error() == TOO_SMALL);
        }

        {
            // Invalid data
            BEAST_EXPECT(
                api.sto_subfield(Bytes{0xFF, 0xFF, 0xFF, 0xFF}, 0).error() ==
                PARSE_ERROR);
        }

        {
            // doesn't found
            // { Memo: {MemoData: "BEEF"} }
            auto const memos = *strUnHex("EA7D02BEEFE1");
            BEAST_EXPECT(
                api.sto_subfield(memos, sfMemoData.getCode()).error() ==
                DOESNT_EXIST);
            // { AmountEntry: {Amount: "100"} }
            auto const amounts = *strUnHex("E05B614000000000000064E1");
            BEAST_EXPECT(
                api.sto_subfield(amounts, sfAmount.getCode()).error() ==
                DOESNT_EXIST);
        }

        {
            // success
            // { Memo: {MemoData: "BEEF"} }
            auto const memos = *strUnHex("EA7D02BEEFE1");
            BEAST_EXPECT(
                api.sto_subfield(memos, sfMemo.getCode()).value() ==
                std::make_pair(1, 4));
            // { AmountEntry: {Amount: "100"} }
            auto const amounts = *strUnHex("E05B614000000000000064E1");
            BEAST_EXPECT(
                api.sto_subfield(amounts, sfAmountEntry.getCode()).value() ==
                std::make_pair(2, 9));
        }
    }

    void
    test_sto_validate(FeatureBitset features)
    {
        testcase("Test sto_validate");

        using namespace jtx;
        using namespace hook;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        BEAST_EXPECT(api.sto_validate(Bytes{}).error() == TOO_SMALL);
        BEAST_EXPECT(api.sto_validate(Bytes{0x00}).error() == TOO_SMALL);

        // { Memo: {MemoData: "BEEF"} }
        auto const memos = *strUnHex("EA7D02BEEFE1");
        // { AmountEntry: {Amount: "100"} }
        auto const amounts = *strUnHex("E05B614000000000000064E1");
        // { Memo: {MemoData: "BEEF"} }
        auto const memo = *strUnHex("EA7D02BEEFE1");
        // { AmountEntry: {Amount: "100"} }
        auto const amountEntry = *strUnHex("E05B614000000000000064E1");

        BEAST_EXPECT(api.sto_validate(memos).value() == true);
        BEAST_EXPECT(api.sto_validate(amounts).value() == true);
        BEAST_EXPECT(api.sto_validate(memo).value() == true);
        BEAST_EXPECT(api.sto_validate(amountEntry).value() == true);

        // Invalid data
        BEAST_EXPECT(
            api.sto_validate(Bytes{0xFF, 0xFF, 0xFF, 0xFF}).value() == false);

        Bytes const i_memos(&memos[0], &memos[memos.size() - 1]);
        Bytes const i_amounts(&amounts[0], &amounts[amounts.size() - 1]);
        Bytes const i_memo(&memo[0], &memo[memo.size() - 1]);
        Bytes const i_amountEntry(
            &amountEntry[0], &amountEntry[amountEntry.size() - 1]);
        BEAST_EXPECT(api.sto_validate(i_memos).value() == false);
        BEAST_EXPECT(api.sto_validate(i_amounts).value() == false);
        BEAST_EXPECT(api.sto_validate(i_memo).value() == false);
        BEAST_EXPECT(api.sto_validate(i_amountEntry).value() == false);

        Bytes const i2_memos(&memos[1], &memos[memos.size()]);
        Bytes const i2_amounts(&amounts[1], &amounts[amounts.size()]);
        Bytes const i2_memo(&memo[1], &memo[memo.size()]);
        Bytes const i2_amountEntry(
            &amountEntry[1], &amountEntry[amountEntry.size()]);
        BEAST_EXPECT(api.sto_validate(i_memos).value() == false);
        BEAST_EXPECT(api.sto_validate(i_amounts).value() == false);
        BEAST_EXPECT(api.sto_validate(i_memo).value() == false);
        BEAST_EXPECT(api.sto_validate(i_amountEntry).value() == false);
    }

    void
    test_trace(FeatureBitset features)
    {
        testcase("Test trace");

        BEAST_EXPECT(true);
    }

    void
    test_trace_float(FeatureBitset features)
    {
        testcase("Test trace_float");

        BEAST_EXPECT(true);
    }

    void
    test_trace_num(FeatureBitset features)
    {
        testcase("Test trace_num");

        BEAST_EXPECT(true);
    }

    void
    test_util_accid(FeatureBitset features)
    {
        testcase("Test util_accid");

        using namespace jtx;
        using namespace hook;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        // Invalid base58 string
        BEAST_EXPECT(api.util_accid("invalid").error() == INVALID_ARGUMENT);

        // Valid r-address round-trip via util_raddr
        auto accid = api.util_accid(alice.human());
        BEAST_EXPECT(accid.has_value());
        auto aliceid = alice.id();
        BEAST_EXPECT(accid.value() == Bytes(aliceid.begin(), aliceid.end()));
    }

    void
    test_util_keylet(FeatureBitset features)
    {
        testcase("Test util_keylet");

        // TODO
        BEAST_EXPECT(true);
    }

    void
    test_util_raddr(FeatureBitset features)
    {
        testcase("Test util_raddr");

        using namespace jtx;
        using namespace hook;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        // Wrong size
        BEAST_EXPECT(api.util_raddr(Bytes(10, 0)).error() == INVALID_ARGUMENT);

        // Valid accountID
        auto aliceid = alice.id();
        auto id = Bytes(aliceid.begin(), aliceid.end());
        auto addr = api.util_raddr(id);
        BEAST_EXPECT(addr.has_value());
        BEAST_EXPECT(addr.value() == alice.human());
    }

    void
    test_util_sha512h(FeatureBitset features)
    {
        testcase("Test util_sha512h");

        using namespace jtx;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        std::string msg{"hello"};
        auto hash = api.util_sha512h(Slice(msg.data(), msg.size()));
        auto expected = ripple::sha512Half(Slice(msg.data(), msg.size()));
        BEAST_EXPECT(hash == expected);
    }

    void
    test_util_verify(FeatureBitset features)
    {
        testcase("Test util_verify");

        using namespace jtx;
        using namespace hook;
        using namespace hook_api;

        auto const alice = Account{"alice"};
        Env env{*this, features};
        STTx invokeTx = STTx(ttINVOKE, [&](STObject& obj) {});
        OpenView ov{*env.current()};
        ApplyContext applyCtx = createApplyContext(env, ov, invokeTx);
        auto hookCtx =
            makeStubHookContext(applyCtx, alice.id(), alice.id(), {});
        hook::HookAPI api(hookCtx);

        // Generate keypair and signature
        KeyType type = KeyType::secp256k1;
        auto kp = generateKeyPair(type, generateSeed("util_verify_test"));
        Serializer s;
        s.add32(42);
        auto const msg = s.slice();
        auto sig = ripple::sign(kp.first, kp.second, msg);

        // Invalid key size
        BEAST_EXPECT(
            api.util_verify(msg, sig, kp.first.slice().substr(0, 32)).error() ==
            INVALID_KEY);

        // Invalid data size
        BEAST_EXPECT(
            api.util_verify(Slice{}, sig, kp.first.slice()).error() ==
            TOO_SMALL);

        // Invalid sig size
        BEAST_EXPECT(
            api.util_verify(msg, Slice(sig.data(), 29), kp.first.slice())
                .error() == TOO_SMALL);

        // Invalid sig type
        auto const& invalidKey = kp.first.slice().substr(1, 34);
        BEAST_EXPECT(
            api.util_verify(msg, sig, invalidKey).error() == INVALID_KEY);
        return;

        // Success
        auto const ok = api.util_verify(msg, sig, kp.first.slice());
        BEAST_EXPECT(ok.has_value());
        BEAST_EXPECT(ok.value());
    }

    void
    testWithFeatures(FeatureBitset features)
    {
        using namespace test::jtx;
        test_accept(features);
        test_rollback(features);
        testGuards(features);

        test_emit(features);
        test_etxn_burden(features);
        test_etxn_generation(features);
        test_otxn_burden(features);
        test_otxn_generation(features);
        test_etxn_details(features);
        test_etxn_fee_base(features - fixHookAPI20251128);
        test_etxn_fee_base(features);
        test_etxn_nonce(features);
        test_etxn_reserve(features);
        test_fee_base(features);

        test_otxn_field(features);

        test_ledger_keylet(features);

        test_float_compare(features);
        test_float_divide(features);
        test_float_int(features);
        test_float_invert(features);
        test_float_log(features);
        test_float_mantissa(features);
        test_float_mulratio(features);
        test_float_multiply(features);
        test_float_negate(features);
        test_float_one(features);
        test_float_root(features);
        test_float_set(features);
        test_float_sign(features);
        test_float_sto(features);
        test_float_sto_set(features);
        test_float_sum(features);

        test_hook_account(features);
        test_hook_again(features);
        test_hook_hash(features);
        test_hook_param(features);
        test_hook_param_set(features);
        test_hook_pos(features);
        test_hook_skip(features);

        test_ledger_last_hash(features);
        test_ledger_last_time(features);
        test_ledger_nonce(features);
        test_ledger_seq(features);

        test_meta_slot(features);
        test_xpop_slot(features);

        test_otxn_id(features);
        test_otxn_slot(features);
        test_otxn_type(features);
        test_otxn_param(features);

        test_slot(features);
        test_slot_clear(features);
        test_slot_count(features);
        test_slot_float(features);
        test_slot_set(features);
        test_slot_size(features);
        test_slot_subarray(features);
        test_slot_subfield(features);
        test_slot_type(features);

        test_state(features);
        test_state_foreign(features);
        test_state_foreign_set(features);
        test_state_foreign_set_max(features);
        test_state_set(features);

        test_sto_emplace(features);
        test_sto_erase(features);
        test_sto_subarray(features);
        test_sto_subfield(features);
        test_sto_validate(features);

        test_trace(features);
        test_trace_float(features);
        test_trace_num(features);

        test_util_accid(features);
        test_util_keylet(features);
        test_util_raddr(features);
        test_util_sha512h(features);
        test_util_verify(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        testWithFeatures(supported_amendments());
    }
};

BEAST_DEFINE_TESTSUITE_PRIO(HookAPI, app, ripple, 2);
}  // namespace test
}  // namespace ripple
#undef M
