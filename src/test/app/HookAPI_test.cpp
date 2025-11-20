//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2016 Ripple Labs Inc.

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
#include <ripple/protocol/STAccount.h>
#include <test/jtx.h>
#include <vector>

namespace ripple {

namespace test {

class HookAPI_test : public beast::unit_test::suite
{
public:
    void
    test_accept(FeatureBitset features)
    {
        testcase("Test accept() hookapi");

        BEAST_EXPECT(true);
    }

    void
    test_rollback(FeatureBitset features)
    {
        testcase("Test rollback() hookapi");

        BEAST_EXPECT(true);
    }

    void
    testGuards(FeatureBitset features)
    {
        testcase("Test guards");

        BEAST_EXPECT(true);
    }

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

    // hook::HookContext
    // createHookContext(
    //     AccountID const& hookAccount,
    //     AccountID const& otxnAccount,
    //     hook::HookContext ctx)
    // {
    //     hook::HookContext hookCtx{
    //         .applyCtx = ctx.applyCtx,
    //         .result =
    //             {
    //                 .hookSetTxnID = uint256(),
    //                 .hookHash = uint256(),
    //                 .hookCanEmit = uint256(),
    //                 .accountKeylet = keylet::account(otxnAccount),
    //                 .hookKeylet = keylet::hook(hookAccount),
    //                 .account = otxnAccount,
    //                 .otxnAccount = otxnAccount,
    //                 .hookNamespace = uint256(),
    //                 .stateMap = ctx.result.stateMap,
    //                 .hookParamOverrides = {},
    //                 .hookParams = {{}},
    //                 .hookSkips = {uint256{}},
    //             },
    //         .module = nullptr};

    //     return hookCtx;
    // }

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

        BEAST_EXPECT(true);
    }

    void
    test_etxn_fee_base(FeatureBitset features)
    {
        testcase("Test etxn_fee_base");

        BEAST_EXPECT(true);
    }

    void
    test_etxn_nonce(FeatureBitset features)
    {
        testcase("Test etxn_nonce");

        BEAST_EXPECT(true);
    }

    void
    test_etxn_reserve(FeatureBitset features)
    {
        testcase("Test etxn_reserve");

        BEAST_EXPECT(true);
    }

    void
    test_fee_base(FeatureBitset features)
    {
        testcase("Test fee_base");

        BEAST_EXPECT(true);
    }

    void
    test_float_compare(FeatureBitset features)
    {
        testcase("Test float_compare");

        BEAST_EXPECT(true);
    }

    void
    test_float_divide(FeatureBitset features)
    {
        testcase("Test float_divide");

        BEAST_EXPECT(true);
    }

    void
    test_float_int(FeatureBitset features)
    {
        testcase("Test float_int");

        BEAST_EXPECT(true);
    }

    void
    test_float_invert(FeatureBitset features)
    {
        testcase("Test float_invert");

        BEAST_EXPECT(true);
    }

    void
    test_float_log(FeatureBitset features)
    {
        testcase("Test float_log");

        BEAST_EXPECT(true);
    }

    void
    test_float_mantissa(FeatureBitset features)
    {
        testcase("Test float_mantissa");

        BEAST_EXPECT(true);
    }

    void
    test_float_mulratio(FeatureBitset features)
    {
        testcase("Test float_mulratio");

        BEAST_EXPECT(true);
    }

    void
    test_float_multiply(FeatureBitset features)
    {
        testcase("Test float_multiply");

        BEAST_EXPECT(true);
    }

    void
    test_float_negate(FeatureBitset features)
    {
        testcase("Test float_negate");

        BEAST_EXPECT(true);
    }

    void
    test_float_one(FeatureBitset features)
    {
        testcase("Test float_one");

        BEAST_EXPECT(true);
    }

    void
    test_float_root(FeatureBitset features)
    {
        testcase("Test float_root");

        BEAST_EXPECT(true);
    }

    void
    test_float_set(FeatureBitset features)
    {
        testcase("Test float_set");

        BEAST_EXPECT(true);
    }

    void
    test_float_sign(FeatureBitset features)
    {
        testcase("Test float_sign");

        BEAST_EXPECT(true);
    }

    void
    test_float_sto(FeatureBitset features)
    {
        testcase("Test float_sto");

        BEAST_EXPECT(true);
    }

    void
    test_float_sto_set(FeatureBitset features)
    {
        testcase("Test float_sto_set");

        BEAST_EXPECT(true);
    }

    void
    test_float_sum(FeatureBitset features)
    {
        testcase("Test float_sum");

        BEAST_EXPECT(true);
    }

    void
    test_hook_account(FeatureBitset features)
    {
        testcase("Test hook_account");

        BEAST_EXPECT(true);
    }

    void
    test_hook_again(FeatureBitset features)
    {
        testcase("Test hook_again");

        BEAST_EXPECT(true);
    }

    void
    test_hook_hash(FeatureBitset features)
    {
        testcase("Test hook_hash");

        BEAST_EXPECT(true);
    }

    void
    test_hook_param(FeatureBitset features)
    {
        testcase("Test hook_param");

        BEAST_EXPECT(true);
    }

    void
    test_hook_param_set(FeatureBitset features)
    {
        testcase("Test hook_param_set");

        BEAST_EXPECT(true);
    }

    void
    test_hook_pos(FeatureBitset features)
    {
        testcase("Test hook_pos");

        BEAST_EXPECT(true);
    }

    void
    test_hook_skip(FeatureBitset features)
    {
        testcase("Test hook_skip");

        BEAST_EXPECT(true);
    }

    void
    test_ledger_keylet(FeatureBitset features)
    {
        testcase("Test ledger_keylet");

        BEAST_EXPECT(true);
    }

    void
    test_ledger_last_hash(FeatureBitset features)
    {
        testcase("Test ledger_last_hash");

        BEAST_EXPECT(true);
    }

    void
    test_ledger_last_time(FeatureBitset features)
    {
        testcase("Test ledger_last_time");

        BEAST_EXPECT(true);
    }

    void
    test_ledger_nonce(FeatureBitset features)
    {
        testcase("Test ledger_nonce");

        BEAST_EXPECT(true);
    }

    void
    test_ledger_seq(FeatureBitset features)
    {
        testcase("Test ledger_seq");

        BEAST_EXPECT(true);
    }

    void
    test_meta_slot(FeatureBitset features)
    {
        testcase("Test meta_slot");

        BEAST_EXPECT(true);
    }

    void
    test_xpop_slot(FeatureBitset features)
    {
        testcase("Test xpop_slot");

        BEAST_EXPECT(true);
    }

    void
    test_otxn_field(FeatureBitset features)
    {
        testcase("Test otxn_field");

        BEAST_EXPECT(true);
    }

    void
    test_otxn_id(FeatureBitset features)
    {
        testcase("Test otxn_id");

        BEAST_EXPECT(true);
    }

    void
    test_otxn_slot(FeatureBitset features)
    {
        testcase("Test otxn_slot");

        BEAST_EXPECT(true);
    }

    void
    test_otxn_type(FeatureBitset features)
    {
        testcase("Test otxn_type");

        BEAST_EXPECT(true);
    }

    void
    test_otxn_param(FeatureBitset features)
    {
        testcase("Test otxn_param");

        BEAST_EXPECT(true);
    }

    void
    test_slot(FeatureBitset features)
    {
        testcase("Test slot");

        BEAST_EXPECT(true);
    }

    void
    test_slot_clear(FeatureBitset features)
    {
        testcase("Test slot_clear");

        BEAST_EXPECT(true);
    }

    void
    test_slot_count(FeatureBitset features)
    {
        testcase("Test slot_count");

        BEAST_EXPECT(true);
    }

    void
    test_slot_float(FeatureBitset features)
    {
        testcase("Test slot_float");

        BEAST_EXPECT(true);
    }

    void
    test_slot_set(FeatureBitset features)
    {
        testcase("Test slot_set");

        BEAST_EXPECT(true);
    }

    void
    test_slot_size(FeatureBitset features)
    {
        testcase("Test slot_size");

        BEAST_EXPECT(true);
    }

    void
    test_slot_subarray(FeatureBitset features)
    {
        testcase("Test slot_subarray");

        BEAST_EXPECT(true);
    }

    void
    test_slot_subfield(FeatureBitset features)
    {
        testcase("Test slot_subfield");

        BEAST_EXPECT(true);
    }

    void
    test_slot_type(FeatureBitset features)
    {
        testcase("Test slot_type");

        BEAST_EXPECT(true);
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

        BEAST_EXPECT(true);
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

        BEAST_EXPECT(true);
    }

    void
    test_sto_subfield(FeatureBitset features)
    {
        testcase("Test sto_subfield");

        BEAST_EXPECT(true);
    }

    void
    test_sto_validate(FeatureBitset features)
    {
        testcase("Test sto_validate");

        BEAST_EXPECT(true);
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

        BEAST_EXPECT(true);
    }

    void
    test_util_keylet(FeatureBitset features)
    {
        testcase("Test util_keylet");

        BEAST_EXPECT(true);
    }

    void
    test_util_raddr(FeatureBitset features)
    {
        testcase("Test util_raddr");

        BEAST_EXPECT(true);
    }

    void
    test_util_sha512h(FeatureBitset features)
    {
        testcase("Test util_sha512h");

        BEAST_EXPECT(true);
    }

    void
    test_util_verify(FeatureBitset features)
    {
        testcase("Test util_verify");

        BEAST_EXPECT(true);
    }

    void
    testHookCanEmit(FeatureBitset features)
    {
        testcase("test HookCanEmit");

        BEAST_EXPECT(true);
    }

    void
    testWithFeatures(FeatureBitset features)
    {
        test_accept(features);
        test_rollback(features);
        testGuards(features);

        test_emit(features);
        // test_etxn_burden(features);
        // test_etxn_generation(features);
        // test_otxn_burden(features);
        // test_otxn_generation(features);
        test_etxn_details(features);
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
