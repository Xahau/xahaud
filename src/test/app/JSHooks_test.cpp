//------------------------------------------------------------------------------
/*
    Transaction-level proof for TypeScript Hooks through the QuickJS provider.
*/
//==============================================================================
#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/hook/HookArtifact.h>
#include <xrpl/protocol/Keylet.h>
#include <xrpl/protocol/digest.h>
#include "JSHooks_test_hooks.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <vector>

namespace ripple {
namespace test {

class JSHooks_test : public beast::unit_test::suite
{
    static Blob
    readBinary(char const* path)
    {
        std::ifstream input{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{input}, {}};
    }

    static Blob
    packageCurrentQuickJS(Blob const& bytecode)
    {
        Blob result(hook::artifact::quickJSHeaderSize + bytecode.size(), 0);
        std::copy(
            hook::artifact::quickJSMagic.begin(),
            hook::artifact::quickJSMagic.end(),
            result.begin());
        result[4] = hook::artifact::quickJSEnvelopeVersion;
        result[5] = hook::artifact::quickJSBytecodeKind;
        result[7] = hook::artifact::quickJSHeaderSize;
        result[9] = 1;
        auto const size = static_cast<std::uint32_t>(bytecode.size());
        result[12] = static_cast<std::uint8_t>(size >> 24);
        result[13] = static_cast<std::uint8_t>(size >> 16);
        result[14] = static_cast<std::uint8_t>(size >> 8);
        result[15] = static_cast<std::uint8_t>(size);
        std::copy(
            hook::artifact::quickJSBytecodeABI.begin(),
            hook::artifact::quickJSBytecodeABI.end(),
            result.begin() + 16);
        std::copy(
            hook::artifact::quickJSRuntimeProfile.begin(),
            hook::artifact::quickJSRuntimeProfile.end(),
            result.begin() + 48);
        std::copy(bytecode.begin(), bytecode.end(), result.begin() + 80);
        return result;
    }

public:
    void
    run() override
    {
        //@@start jshooks-hook-fixtures
        auto const& hookBytecode = jshooks_test_wasm.at(R"[test.tshook](
export function hook(_reserved: number): never {
  void _reserved;
  const txType = otxn.type();
  if (!txType.ok) lifecycle.rollback("otxn.type failed", txType.code);
  if (txType.value !== TransactionType.Payment) {
    lifecycle.rollback("expected Payment", txType.value);
  }
  lifecycle.accept(`payment:${txType.value}`, 42);
}
)[test.tshook]");
        auto const hookCode = packageCurrentQuickJS(hookBytecode);

        auto const surfaceProbeCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function hook(_reserved: number): never {
  void _reserved;
  const account = lifecycle.account();
  if (!account.ok) lifecycle.rollback("lifecycle.account failed", account.code);
  if (account.value.toHex().length !== 40) {
    lifecycle.rollback("unexpected Hook account", -1);
  }
  if (ledger.sequence <= 0 || ledger.lastHash.isZero()) {
    lifecycle.rollback("invalid ledger context", -2);
  }
  const expectedNow = (946_684_800 + ledger.lastTime) * 1000;
  if (Date.now() !== expectedNow) {
    lifecycle.rollback("Date clock disagrees with ledger", -3);
  }
  const missing = state.get("surface-missing");
  if (!missing.ok || missing.value !== undefined) {
    lifecycle.rollback("missing state was not ordinary absence", -4);
  }

  try {
    lifecycle.accept(`surface:${account.value.toHex().length}`, 99);
  } catch {
    lifecycle.rollback("terminal was catchable", -99);
  }
  lifecycle.rollback("terminal returned", -98);
}
)[test.tshook]"));

        auto const& stateSeedCode = jshooks_test_wasm.at(R"[test.hook](
#include <stdint.h>
extern int32_t _g(uint32_t id, uint32_t maxiter);
extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t code);
extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t code);
extern int64_t trace(
    uint32_t tag_ptr,
    uint32_t tag_len,
    uint32_t data_ptr,
    uint32_t data_len,
    uint32_t as_hex);
extern int64_t state_set(
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t key_ptr,
    uint32_t key_len);

int64_t hook(uint32_t reserved)
{
    _g(1, 1);
    (void)reserved;
    uint8_t key[] = "bridge";
    uint8_t value[] = "from-c";
    uint8_t label[] = "c-state-seed";
    trace(label, sizeof(label) - 1, value, sizeof(value) - 1, 0);
    int64_t result = state_set(
        value, sizeof(value) - 1, key, sizeof(key) - 1);
    if (result < 0)
        return rollback(0, 0, result);
    return accept(0, 0, result);
}
)[test.hook]");

        auto const stateBridgeCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function hook(_reserved: number): never {
  void _reserved;
  const seeded = state.get("bridge");
  if (!seeded.ok) lifecycle.rollback("state read failed", seeded.code);
  if (seeded.value === undefined) lifecycle.rollback("state missing", -1);

  const seededHex = seeded.value.toHex();
  trace("js-state-read", seededHex);
  if (seededHex !== "66726F6D2D63") {
    lifecycle.rollback(`unexpected state:${seededHex}`, -2);
  }

  const write = state.set("bridge", "from-js");
  if (!write.ok) lifecycle.rollback("state write failed", write.code);
  trace("js-state-write", "from-js");
  lifecycle.accept("state bridged", 84);
}
)[test.tshook]"));

        auto const stateRollbackCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function hook(_reserved: number): never {
  void _reserved;
  const write = state.set("bridge", "must-not-stick");
  if (!write.ok) lifecycle.rollback("state write failed", write.code);
  trace("js-state-rollback", "must-not-stick");
  lifecycle.rollback("state rollback", -84);
}
)[test.tshook]"));

        auto const memoryGrowthCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function hook(_reserved: number): never {
  void _reserved;
  const blocks: Uint8Array[] = [];
  for (let i = 0; i < 6; ++i) blocks.push(new Uint8Array(1024 * 1024));
  blocks[5][0] = 42;

  const seeded = state.get("bridge");
  if (!seeded.ok || seeded.value?.toHex() !== "66726F6D2D6A73") {
    lifecycle.rollback("growth state read failed", -1);
  }
  const write = state.set("growth", "must-roll-back");
  if (!write.ok) lifecycle.rollback("growth state write failed", write.code);
  trace("growth-host-call", blocks[5].subarray(0, 1));
  throw new Error("memory-growth-diagnostic");
}
)[test.tshook]"));

        auto const hostWorkExhaustionCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function hook(_reserved: number): never {
  void _reserved;
  const write = state.set("meter", "must-not-stick");
  if (!write.ok) lifecycle.rollback("state write failed", write.code);

  const chunk = "x".repeat(1000);
  for (let i = 0; i < 1100; ++i) trace("meter", chunk);
  lifecycle.accept("host-work budget escaped", -1);
}
)[test.tshook]"));

        auto const amendmentBeforeChargeCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function hook(_reserved: number): never {
  void _reserved;
  const transaction = STBlob.from(new Uint8Array(0xffff));
  for (let i = 0; i < 8; ++i) {
    const unavailable = emit.prepare(transaction);
    if (unavailable.ok || unavailable.code !== -14) {
      lifecycle.rollback("prepare was not amendment-unavailable", -1);
    }
  }
  lifecycle.accept("unavailable calls were not charged", 808);
}
)[test.tshook]"));

        auto const callbackCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function hook(_reserved: number): never {
  void _reserved;
  const write = state.set("cbak", "pending");
  if (!write.ok) lifecycle.rollback("callback seed failed", write.code);
  const reserve = emit.reserve(1);
  if (!reserve.ok) lifecycle.rollback("emit.reserve failed", reserve.code);
  const prepared = emit.prepare(STBlob.from("1200032280000000"));
  if (!prepared.ok) lifecycle.rollback("emit.prepare failed", prepared.code);
  const sent = emit.tx(prepared.value);
  if (!sent.ok) lifecycle.rollback("emit.tx failed", sent.code);
  lifecycle.accept("callback emitted", 101);
}

export function cbak(_reserved: number): never {
  if (_reserved !== 0) lifecycle.rollback("emitted transaction failed", _reserved);
  const write = state.set("cbak", "called");
  if (!write.ok) lifecycle.rollback("callback state failed", write.code);
  lifecycle.accept("callback called", 202);
}
)[test.tshook]"));

        auto const missingHookCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function cbak(_reserved: number): never {
  void _reserved;
  lifecycle.accept("cbak only", 1);
}
)[test.tshook]"));

        auto const nonCallableEntriesCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export const hook = 1;
export const cbak = 2;
)[test.tshook]"));

        auto const hostInitializingCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function hook(_reserved: number): never {
  lifecycle.accept("entry", _reserved);
}
void ledger.sequence;
)[test.tshook]"));
        //@@end jshooks-hook-fixtures

        using namespace jtx;
        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        auto const carol = Account{"carol"};
        auto const features = supported_amendments();

        //@@start jshooks-amendment-and-entry
        testcase("Validate QuickJS deployment identity and admission");
        {
            Env env{*this, features};
            env.fund(XRP(10000), alice);
            env.close();

            // A canonical artifact is not installable before profile
            // activation, and raw qjsc is never an on-ledger artifact.
            env(jtx::hook(alice, {{hsoVersioned(hookCode, 1)}}, 0),
                fee(XRP(10)),
                ter(temMALFORMED));
            env(jtx::hook(alice, {{hsoVersioned(hookBytecode, 1)}}, 0),
                fee(XRP(10)),
                ter(temMALFORMED));
        }

        //@@start jshooks-provider-fixture
        auto const* providerPath = std::getenv("XAHAU_QJS_PROVIDER_WASM");
        auto const providerRequired =
            std::getenv("XAHAU_REQUIRE_QJS_PROVIDER_TESTS") != nullptr;
        if (!providerPath)
        {
            testcase("QuickJS execution provider fixture absent");
            BEAST_EXPECT(!providerRequired);
            return;
        }

        auto provider = readBinary(providerPath);
        BEAST_EXPECT(!provider.empty());
        if (provider.empty())
            return;

        auto corruptedProvider = provider;
        corruptedProvider[0] ^= 0xff;
        auto const corruptProviderError =
            hook::setQuickJSProviderForTests(std::move(corruptedProvider));
        BEAST_EXPECT(!!corruptProviderError);
        auto const currentArtifact = hook::artifact::parse(makeSlice(hookCode));
        BEAST_EXPECT(!!currentArtifact);
        if (!currentArtifact)
            return;
        BEAST_EXPECT(!hook::findQuickJSRuntime(*currentArtifact));

        testcase("Reject unregistered QuickJS profiles in test builds");
        {
            Env unregisteredEnv{*this, features | featureJSHooks};
            unregisteredEnv.fund(XRP(10000), alice);
            unregisteredEnv.close();
            unregisteredEnv(
                jtx::hook(alice, {{hsoVersioned(hookCode, 1)}}, 0),
                fee(XRP(10)),
                ter(temMALFORMED));
        }

        auto const providerError = hook::setQuickJSProviderForTests(provider);
        BEAST_EXPECT(!providerError);
        if (providerError)
            return;
        auto const currentRuntime = hook::findQuickJSRuntime(*currentArtifact);
        BEAST_EXPECT(!!currentRuntime);

        auto const successfulValidation =
            hook::validateQuickJSBytecodeForTests(currentRuntime, hookBytecode);
        BEAST_EXPECT(!successfulValidation.error);
        BEAST_EXPECT(!successfulValidation.hasCallback);
        BEAST_EXPECT(successfulValidation.invocationFuelConsumed == 48330);

        testcase("Validate one retained provider concurrently");
        std::array<std::future<hook::QuickJSValidationForTests>, 4>
            concurrentValidations;
        for (auto& validation : concurrentValidations)
        {
            validation = std::async(std::launch::async, [&] {
                return hook::validateQuickJSBytecodeForTests(
                    currentRuntime, hookBytecode);
            });
        }
        for (auto& validation : concurrentValidations)
        {
            auto result = validation.get();
            BEAST_EXPECT(!result.error);
            BEAST_EXPECT(!result.hasCallback);
            BEAST_EXPECT(result.invocationFuelConsumed == 48330);
        }

        testcase("Bind API, profile, hash, dedup, and hash install");
        {
            Env identityEnv{*this, features | featureJSHooks};
            identityEnv.fund(XRP(10000), alice, bob, carol);
            identityEnv.close();

            identityEnv(
                jtx::hook(alice, {{hsoVersioned(hookCode, 0)}}, 0),
                fee(XRP(10)),
                ter(temMALFORMED));

            auto unsupportedProfile = hookCode;
            unsupportedProfile[48] ^= 0xFFU;
            identityEnv(
                jtx::hook(alice, {{hsoVersioned(unsupportedProfile, 1)}}, 0),
                fee(XRP(10)),
                ter(temMALFORMED));

            identityEnv(
                jtx::hook(alice, {{hsoVersioned(hookCode, 1)}}, 0),
                fee(XRP(10)),
                ter(tesSUCCESS));
            identityEnv.close();

            auto malformedBytecode = hookBytecode;
            malformedBytecode.resize(8);
            auto const failedValidation = hook::validateQuickJSBytecodeForTests(
                currentRuntime, malformedBytecode);
            BEAST_EXPECT(!!failedValidation.error);
            BEAST_EXPECT(failedValidation.invocationFuelConsumed == 12726);
            identityEnv(
                jtx::hook(
                    alice,
                    {{hsoVersioned(
                        packageCurrentQuickJS(malformedBytecode), 1)}},
                    0),
                fee(XRP(10)),
                ter(temMALFORMED));
            identityEnv(
                jtx::hook(alice, {{hsoVersioned(missingHookCode, 1)}}, 0),
                fee(XRP(10)),
                ter(temMALFORMED));
            identityEnv(
                jtx::hook(
                    alice, {{hsoVersioned(nonCallableEntriesCode, 1)}}, 0),
                fee(XRP(10)),
                ter(temMALFORMED));
            identityEnv(
                jtx::hook(alice, {{hsoVersioned(hostInitializingCode, 1)}}, 0),
                fee(XRP(10)),
                ter(temMALFORMED));

            auto const hookHash = sha512Half_s(makeSlice(hookCode));
            auto const definition =
                identityEnv.le(keylet::hookDefinition(hookHash));
            BEAST_EXPECT(!!definition);
            if (!definition)
                return;
            BEAST_EXPECT(definition->getFieldU16(sfHookApiVersion) == 1);
            BEAST_EXPECT(definition->getFieldVL(sfCreateCode) == hookCode);
            BEAST_EXPECT(!definition->isFieldPresent(sfHookCallbackFee));

            // Submitting identical envelope bytes reuses the same definition.
            identityEnv(
                jtx::hook(bob, {{hsoVersioned(hookCode, 1)}}, 0),
                fee(XRP(10)),
                ter(tesSUCCESS));
            identityEnv.close();
            BEAST_EXPECT(
                identityEnv.le(keylet::hookDefinition(hookHash))
                    ->getFieldU64(sfReferenceCount) == 2);

            Json::Value hashInstall;
            hashInstall[jss::HookHash] = to_string(hookHash);
            identityEnv(
                jtx::hook(carol, {{hashInstall}}, 0),
                fee(XRP(10)),
                ter(tesSUCCESS));
            identityEnv.close();
            BEAST_EXPECT(
                identityEnv.le(keylet::hookDefinition(hookHash))
                    ->getFieldU64(sfReferenceCount) == 3);
        }
        //@@end jshooks-amendment-and-entry

        auto historicalProfile = hook::currentQuickJSRuntimeProfile();
        historicalProfile.runtimeProfile[0] ^= 0xffU;
        auto const historicalError =
            hook::registerQuickJSRuntime(historicalProfile, provider);
        BEAST_EXPECT(!historicalError);
        auto historicalArtifact = *currentArtifact;
        historicalArtifact.runtimeProfile = historicalProfile.runtimeProfile;
        auto const historicalRuntime =
            hook::findQuickJSRuntime(historicalArtifact);
        BEAST_EXPECT(!!historicalRuntime);
        BEAST_EXPECT(historicalRuntime != currentRuntime);
        BEAST_EXPECT(
            hook::findQuickJSRuntime(*currentArtifact) == currentRuntime);

        auto conflictingProfile = hook::currentQuickJSRuntimeProfile();
        ++conflictingProfile.invocationFuel;
        BEAST_EXPECT(
            !!hook::registerQuickJSRuntime(conflictingProfile, provider));
        //@@end jshooks-provider-fixture

        testcase("Execute an enveloped TypeScript Hook transaction");
        Env env{*this, features | featureJSHooks};
        env.fund(XRP(10000), alice, bob);
        env.close();
        env(jtx::hook(alice, {{hsoVersioned(hookCode, 1)}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        env.close();

        env(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
        env.close();

        auto const meta = env.meta();
        BEAST_EXPECT(!!meta);
        if (!meta)
            return;
        BEAST_EXPECT(meta->isFieldPresent(sfHookExecutions));
        if (!meta->isFieldPresent(sfHookExecutions))
            return;
        auto const executions = meta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(executions.size() == 1);
        if (executions.size() != 1)
            return;
        auto const& execution = executions[0];

        BEAST_EXPECT(
            execution.getFieldU8(sfHookResult) ==
            static_cast<uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(execution.getFieldU64(sfHookReturnCode) == 42);
        auto const message = execution.getFieldVL(sfHookReturnString);
        BEAST_EXPECT(
            std::string(message.begin(), message.end()) == "payment:0");
        BEAST_EXPECT(execution.getFieldU64(sfHookInstructionCount) == 55811);

        testcase("Bind ledger context and keep terminals uncatchable");
        auto surfaceProbeHook = hsoVersioned(surfaceProbeCode, 1);
        surfaceProbeHook[jss::Flags] = hsfOVERRIDE;
        env(jtx::hook(alice, {{surfaceProbeHook}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        env.close();

        env(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
        env.close();

        auto const surfaceMeta = env.meta();
        BEAST_EXPECT(!!surfaceMeta);
        if (!surfaceMeta)
            return;
        auto const surfaceExecutions =
            surfaceMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(surfaceExecutions.size() == 1);
        if (surfaceExecutions.size() != 1)
            return;
        auto const& surfaceExecution = surfaceExecutions[0];
        BEAST_EXPECT(
            surfaceExecution.getFieldU8(sfHookResult) ==
            static_cast<uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(surfaceExecution.getFieldU64(sfHookReturnCode) == 99);
        auto const surfaceMessage =
            surfaceExecution.getFieldVL(sfHookReturnString);
        BEAST_EXPECT(
            std::string(surfaceMessage.begin(), surfaceMessage.end()) ==
            "surface:40");
        BEAST_EXPECT(
            surfaceExecution.getFieldU64(sfHookInstructionCount) == 116108);

        //@@start jshooks-state-bridge
        testcase("Execute a C Hook through WasmEdge and persist state");

        auto seedHook = hso(stateSeedCode);
        seedHook[jss::Flags] = hsfOVERRIDE;
        env(jtx::hook(alice, {{seedHook}}, 0), fee(XRP(10)), ter(tesSUCCESS));
        env.close();

        env(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
        env.close();

        auto const stateKey = uint256::fromVoid(
            (std::array<uint8_t, 32>{
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 'b',   'r',   'i',   'd',   'g',   'e'})
                .data());
        auto const stateKeylet =
            keylet::hookState(alice.id(), stateKey, uint256{beast::zero});

        auto stateEntry = env.le(stateKeylet);
        BEAST_EXPECT(!!stateEntry);
        if (!stateEntry)
            return;
        auto seededData = stateEntry->getFieldVL(sfHookStateData);
        BEAST_EXPECT(
            std::string(seededData.begin(), seededData.end()) == "from-c");

        auto const cHookMeta = env.meta();
        BEAST_EXPECT(!!cHookMeta);
        if (!cHookMeta || !cHookMeta->isFieldPresent(sfHookExecutions))
            return;
        auto const cHookExecutions = cHookMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(cHookExecutions.size() == 1);
        if (cHookExecutions.size() != 1)
            return;
        auto const& cHookExecution = cHookExecutions[0];
        BEAST_EXPECT(
            cHookExecution.getFieldU8(sfHookResult) ==
            static_cast<std::uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(cHookExecution.getFieldU64(sfHookReturnCode) == 6);
        BEAST_EXPECT(cHookExecution.getFieldVL(sfHookReturnString).empty());
        BEAST_EXPECT(cHookExecution.getFieldU64(sfHookInstructionCount) > 0);

        testcase("Read WasmEdge C Hook state from TypeScript and replace it");

        auto stateBridgeHook = hsoVersioned(stateBridgeCode, 1);
        stateBridgeHook[jss::Flags] = hsfOVERRIDE;
        env(jtx::hook(alice, {{stateBridgeHook}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        env.close();

        env(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
        env.close();

        auto const bridgeMeta = env.meta();
        BEAST_EXPECT(!!bridgeMeta);
        if (!bridgeMeta)
            return;
        BEAST_EXPECT(bridgeMeta->isFieldPresent(sfHookExecutions));
        if (!bridgeMeta->isFieldPresent(sfHookExecutions))
            return;
        auto const bridgeExecutions =
            bridgeMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(bridgeExecutions.size() == 1);
        if (bridgeExecutions.size() != 1)
            return;
        auto const& bridgeExecution = bridgeExecutions[0];
        BEAST_EXPECT(
            bridgeExecution.getFieldU8(sfHookResult) ==
            static_cast<uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(bridgeExecution.getFieldU64(sfHookReturnCode) == 84);
        auto const bridgeMessage =
            bridgeExecution.getFieldVL(sfHookReturnString);
        BEAST_EXPECT(
            std::string(bridgeMessage.begin(), bridgeMessage.end()) ==
            "state bridged");

        stateEntry = env.le(stateKeylet);
        BEAST_EXPECT(!!stateEntry);
        if (!stateEntry)
            return;
        auto const bridgedData = stateEntry->getFieldVL(sfHookStateData);
        BEAST_EXPECT(
            std::string(bridgedData.begin(), bridgedData.end()) == "from-js");
        //@@end jshooks-state-bridge

        //@@start jshooks-rollback-atomicity
        testcase("Rollback a TypeScript state replacement atomically");

        auto stateRollbackHook = hsoVersioned(stateRollbackCode, 1);
        stateRollbackHook[jss::Flags] = hsfOVERRIDE;
        env(jtx::hook(alice, {{stateRollbackHook}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        env.close();

        env(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tecHOOK_REJECTED));

        auto const rollbackMeta = env.meta();
        BEAST_EXPECT(!!rollbackMeta);
        if (!rollbackMeta || !rollbackMeta->isFieldPresent(sfHookExecutions))
            return;
        auto const rollbackExecutions =
            rollbackMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(rollbackExecutions.size() == 1);
        if (rollbackExecutions.size() != 1)
            return;
        BEAST_EXPECT(
            rollbackExecutions[0].getFieldU64(sfHookInstructionCount) == 54296);

        stateEntry = env.le(stateKeylet);
        BEAST_EXPECT(!!stateEntry);
        if (!stateEntry)
            return;
        auto const afterRollbackData = stateEntry->getFieldVL(sfHookStateData);
        BEAST_EXPECT(
            std::string(afterRollbackData.begin(), afterRollbackData.end()) ==
            "from-js");
        //@@end jshooks-rollback-atomicity

        testcase("Reacquire provider memory after growth");

        auto memoryGrowthHook = hsoVersioned(memoryGrowthCode, 1);
        memoryGrowthHook[jss::Flags] = hsfOVERRIDE;
        env(jtx::hook(alice, {{memoryGrowthHook}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        env.close();

        env(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tecHOOK_REJECTED));
        auto const memoryGrowthMeta = env.meta();
        BEAST_EXPECT(!!memoryGrowthMeta);
        if (!memoryGrowthMeta)
            return;
        auto const memoryGrowthExecutions =
            memoryGrowthMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(memoryGrowthExecutions.size() == 1);
        if (memoryGrowthExecutions.size() != 1)
            return;
        auto const& memoryGrowthExecution = memoryGrowthExecutions[0];
        BEAST_EXPECT(
            memoryGrowthExecution.getFieldU64(sfHookInstructionCount) ==
            6449373);
        BEAST_EXPECT(
            memoryGrowthExecution.getFieldU8(sfHookResult) ==
            static_cast<std::uint8_t>(hook_api::ExitType::WASM_ERROR));
        auto const growthMessage =
            memoryGrowthExecution.getFieldVL(sfHookReturnString);
        BEAST_EXPECT(
            std::string(growthMessage.begin(), growthMessage.end())
                .find("memory-growth-diagnostic") != std::string::npos);

        auto const growthKey = uint256::fromVoid(
            (std::array<uint8_t, 32>{
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 'g',   'r',   'o',   'w',   't',   'h'})
                .data());
        BEAST_EXPECT(!env.le(
            keylet::hookState(alice.id(), growthKey, uint256{beast::zero})));

        //@@start jshooks-host-work-budget
        testcase("Exhaust host work before a call and roll back atomically");

        auto hostWorkHook = hsoVersioned(hostWorkExhaustionCode, 1);
        hostWorkHook[jss::Flags] = hsfOVERRIDE;
        env(jtx::hook(alice, {{hostWorkHook}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        env.close();

        env(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tecHOOK_REJECTED));

        auto const hostWorkMeta = env.meta();
        BEAST_EXPECT(!!hostWorkMeta);
        if (!hostWorkMeta)
            return;
        auto const hostWorkExecutions =
            hostWorkMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(hostWorkExecutions.size() == 1);
        if (hostWorkExecutions.size() != 1)
            return;
        BEAST_EXPECT(
            hostWorkExecutions[0].getFieldU8(sfHookResult) ==
            static_cast<std::uint8_t>(hook_api::ExitType::WASM_ERROR));
        BEAST_EXPECT(
            hostWorkExecutions[0].getFieldU64(sfHookInstructionCount) ==
            11425847);

        auto const meterKey = uint256::fromVoid(
            (std::array<uint8_t, 32>{
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 'm',   'e',   't',   'e',   'r'})
                .data());
        BEAST_EXPECT(!env.le(
            keylet::hookState(alice.id(), meterKey, uint256{beast::zero})));
        //@@end jshooks-host-work-budget

        testcase("Reject unavailable imports before host-work charging");
        Env gatedEnv{*this, (features | featureJSHooks) - featureHooksUpdate2};
        gatedEnv.fund(XRP(10000), alice, bob);
        gatedEnv.close();
        gatedEnv(
            jtx::hook(alice, {{hsoVersioned(amendmentBeforeChargeCode, 1)}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        gatedEnv.close();
        gatedEnv(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
        auto const gatedMeta = gatedEnv.meta();
        BEAST_EXPECT(!!gatedMeta);
        if (!gatedMeta)
            return;
        auto const gatedExecutions = gatedMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(gatedExecutions.size() == 1);
        if (gatedExecutions.size() != 1)
            return;
        BEAST_EXPECT(
            gatedExecutions[0].getFieldU8(sfHookResult) ==
            static_cast<std::uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(gatedExecutions[0].getFieldU64(sfHookReturnCode) == 808);

        testcase("Execute a TypeScript callback from an emitted transaction");
        Env callbackEnv{*this, features | featureJSHooks};
        callbackEnv.fund(XRP(10000), alice, bob);
        callbackEnv.close();

        auto callbackHook = hsoVersioned(callbackCode, 1);
        callbackEnv(
            jtx::hook(alice, {{callbackHook}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        callbackEnv.close();

        auto const callbackHookHash = sha512Half_s(makeSlice(callbackCode));
        auto const callbackDefinition =
            callbackEnv.le(keylet::hookDefinition(callbackHookHash));
        BEAST_EXPECT(!!callbackDefinition);
        if (!callbackDefinition)
            return;
        BEAST_EXPECT(callbackDefinition->isFieldPresent(sfHookCallbackFee));

        callbackEnv(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
        auto const parentMetadata = callbackEnv.meta();
        BEAST_EXPECT(!!parentMetadata);
        if (!parentMetadata)
            return;
        BEAST_EXPECT(parentMetadata->isFieldPresent(sfHookEmissions));
        if (!parentMetadata->isFieldPresent(sfHookEmissions))
            return;
        auto const parentEmissions =
            parentMetadata->getFieldArray(sfHookEmissions);
        BEAST_EXPECT(parentEmissions.size() == 1);
        if (parentEmissions.size() != 1)
            return;
        auto const expectedEmittedID =
            parentEmissions[0].getFieldH256(sfEmittedTxnID);
        // meta() closed the parent Payment ledger; the next close applies the
        // emitted transaction and its callback.
        callbackEnv.close();

        auto const [emittedTransaction, emittedMetadata] =
            callbackEnv.closed()->txRead(expectedEmittedID);
        BEAST_EXPECT(!!emittedTransaction);
        BEAST_EXPECT(!!emittedMetadata);
        if (!emittedTransaction || !emittedMetadata)
            return;
        BEAST_EXPECT(emittedTransaction->getTxnType() == ttACCOUNT_SET);
        BEAST_EXPECT(emittedTransaction->isFieldPresent(sfEmitDetails));
        BEAST_EXPECT(
            isTesSuccess(emittedMetadata->getFieldU8(sfTransactionResult)));
        BEAST_EXPECT(emittedMetadata->isFieldPresent(sfHookExecutions));
        if (!emittedMetadata->isFieldPresent(sfHookExecutions))
            return;
        auto const callbackExecutions =
            emittedMetadata->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(callbackExecutions.size() == 1);
        if (callbackExecutions.size() != 1)
            return;
        auto const& callbackExecution = callbackExecutions[0];
        BEAST_EXPECT(
            callbackExecution.getFieldU64(sfHookInstructionCount) == 79075);
        BEAST_EXPECT(
            callbackExecution.getFieldU8(sfHookResult) ==
            static_cast<std::uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(callbackExecution.getFieldU64(sfHookReturnCode) == 202);
        auto const callbackMessage =
            callbackExecution.getFieldVL(sfHookReturnString);
        BEAST_EXPECT(
            std::string(callbackMessage.begin(), callbackMessage.end()) ==
            "callback called");

        auto const callbackKey = uint256::fromVoid(
            (std::array<std::uint8_t, 32>{
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 'c',   'b',   'a',   'k'})
                .data());
        auto const callbackState = callbackEnv.le(
            keylet::hookState(alice.id(), callbackKey, uint256{beast::zero}));
        BEAST_EXPECT(!!callbackState);
        if (!callbackState)
            return;
        auto const callbackData = callbackState->getFieldVL(sfHookStateData);
        BEAST_EXPECT(
            std::string(callbackData.begin(), callbackData.end()) == "called");
    }
};

BEAST_DEFINE_TESTSUITE(JSHooks, app, ripple);

}  // namespace test
}  // namespace ripple
