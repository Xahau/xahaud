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

    void
    expectFuel(std::uint64_t actual, std::uint64_t expected)
    {
        BEAST_EXPECT_EQ(actual, expected);
    }

public:
    void
    run() override
    {
        //@@start jshooks-hook-fixtures
        auto const& hookBytecode = jshooks_test_wasm.at(R"[test.tshook](
export function main(_reserved: number): never {
  void _reserved;
  const txType = rollback.onFail(otxn.type(), "otxn.type failed");
  if (txType !== TransactionType.Payment) {
    rollback("expected Payment", txType);
  }
  accept(`payment:${txType}`, 42);
}
)[test.tshook]");
        auto const hookCode = packageCurrentQuickJS(hookBytecode);

        auto const surfaceProbeCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function main(_reserved: number): never {
  void _reserved;
  const account = hook.account();
  if (account.toHex().length !== 40) {
    rollback("unexpected Hook account", -1);
  }
  if (ledger.sequence <= 0 || ledger.lastHash.isZero()) {
    rollback("invalid ledger context", -2);
  }
  const expectedNow = (946_684_800 + ledger.lastTime) * 1000;
  if (Date.now() !== expectedNow) {
    rollback("Date clock disagrees with ledger", -3);
  }
  const missing = rollback.onFail(
    state.get("surface-missing"),
    "missing state was not ordinary absence",
  );
  if (missing !== undefined) {
    rollback("missing state was not ordinary absence", -4);
  }

  try {
    accept(`surface:${account.toHex().length}`, 99);
  } catch {
    rollback("terminal was catchable", -99);
  }
  rollback("terminal returned", -98);
}
)[test.tshook]"));

        auto const stObjectArrayCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function main(_reserved: number): never {
  void _reserved;
  const ordered = util.decodeObject(
    Uint8Array.from([
      0x24, 0x00, 0x00, 0x00, 0x07, 0x22, 0x00, 0x00, 0x00, 0x09,
    ]),
  );
  const flags = ordered.get(Field.Flags);
  const flagsAgain = ordered.get(Field.Flags);
  if (flags === undefined || flags !== flagsAgain || flags.toNumber() !== 9) {
    rollback("repeated Flags identity", -1);
  }
  const canonical = Array.from(ordered.toBytes());
  if (canonical.join(",") !== "34,0,0,0,9,36,0,0,0,7") {
    rollback("canonical bytes", -2);
  }
  const json = JSON.stringify(ordered.toJSON());
  if (json !== '{"Flags":9,"Sequence":7}') {
    rollback("canonical json", -3);
  }

  const amountRoot = util.decodeObject(
    Uint8Array.from([0x61, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2a]),
  );
  const amount = amountRoot.get(Field.Amount);
  if (amount === undefined || amount.kind !== "native" || amount.drops !== 42n) {
    rollback("native amount", -4);
  }
  const issue = amount.issue;
  const currency = issue.currency;
  if (currency === undefined || currency !== issue.currency) {
    rollback("amount.issue.currency identity", -5);
  }
  if (!currency.isNative || currency.toString() !== "XAH") {
    rollback("native currency", -6);
  }

  const root = util.decodeObject(
    Uint8Array.from([
      0xf9, 0xea, 0x22, 0x00, 0x00, 0x00, 0x01, 0xe1, 0xea, 0x22, 0x00, 0x00,
      0x00, 0x02, 0xe1, 0xf1,
    ]),
  );
  const memos = root.get(Field.Memos);
  if (memos === undefined || memos.length !== 2) {
    rollback("STArray length", -7);
  }
  const first = memos.at(0);
  if (first === undefined || first !== memos[0] || first !== memos.at(0)) {
    rollback("STArray element identity", -8);
  }
  const firstFlags = first.get(Field.Flags);
  const secondFlags = memos.at(1)?.get(Field.Flags);
  if (
    firstFlags === undefined ||
    secondFlags === undefined ||
    firstFlags.toNumber() !== 1 ||
    secondFlags.toNumber() !== 2
  ) {
    rollback("STArray element flags", -9);
  }
  if (memos.at(99) !== undefined) {
    rollback("STArray out of range", -10);
  }
  accept("stobject-starray", 77);
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
export function main(_reserved: number): never {
  void _reserved;
  const seeded = rollback.onFail(state.get("bridge"), "state read failed");
  if (seeded === undefined) rollback("state missing", -1);

  const seededHex = seeded.toHex();
  trace("js-state-read", seededHex);
  if (seededHex !== "66726F6D2D63") {
    rollback(`unexpected state:${seededHex}`, -2);
  }

  rollback.onFail(state.set("bridge", "from-js"), "state write failed");
  trace("js-state-write", "from-js");
  accept("state bridged", 84);
}
)[test.tshook]"));

        auto const stateRollbackCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function main(_reserved: number): never {
  void _reserved;
  rollback.onFail(state.set("bridge", "must-not-stick"), "state write failed");
  trace("js-state-rollback", "must-not-stick");
  rollback("state rollback", -84);
}
)[test.tshook]"));

        auto const memoryGrowthCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function main(_reserved: number): never {
  void _reserved;
  const blocks: Uint8Array[] = [];
  for (let i = 0; i < 6; ++i) blocks.push(new Uint8Array(1024 * 1024));
  blocks[5][0] = 42;

  const seeded = rollback.onFail(state.get("bridge"), "growth state read failed");
  if (seeded?.toHex() !== "66726F6D2D6A73") {
    rollback("growth state read failed", -1);
  }
  rollback.onFail(state.set("growth", "must-roll-back"), "growth state write failed");
  trace("growth-host-call", blocks[5].subarray(0, 1));
  throw new Error("memory-growth-diagnostic");
}
)[test.tshook]"));

        auto const hostWorkExhaustionCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function main(_reserved: number): never {
  void _reserved;
  rollback.onFail(state.set("meter", "must-not-stick"), "state write failed");

  const chunk = "x".repeat(1000);
  for (let i = 0; i < 1100; ++i) trace("meter", chunk);
  accept("host-work budget escaped", -1);
}
)[test.tshook]"));

        auto const amendmentBeforeChargeCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function main(_reserved: number): never {
  void _reserved;
  const transaction = STBlob.from(new Uint8Array(0xffff));
  for (let i = 0; i < 8; ++i) {
    const unavailable = emit.prepare(transaction);
    if (unavailable.ok) {
      rollback("prepare was not amendment-unavailable", -1);
    } else if (unavailable.error.code !== HookReturnCode.NOT_IMPLEMENTED) {
      rollback("prepare was not amendment-unavailable", -1);
    }
  }
  accept("unavailable calls were not charged", 808);
}
)[test.tshook]"));

        auto const callbackCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
export function main(_reserved: number): never {
  void _reserved;
  rollback.onFail(state.set("cbak", "pending"), "callback seed failed");
  rollback.onFail(emit.reserve(1), "emit.reserve failed");
  const prepared = rollback.onFail(
    emit.prepare(STBlob.fromHex("1200032280000000")),
    "emit.prepare failed",
  );
  rollback.onFail(emit.tx(prepared), "emit.tx failed");
  accept("callback emitted", 101);
}

export function callback(info: CallbackInfo): never {
  if (info.failureBitSet) rollback("emitted transaction failed", info.rawFlags);
  rollback.onFail(state.set("cbak", "called"), "callback state failed");
  accept("callback called", 202);
}
)[test.tshook]"));

        auto const missingMainCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
// @jshookz-allow-malformed
export function callback(info: CallbackInfo): never {
  void info;
  accept("callback only", 1);
}
)[test.tshook]"));

        auto const nonCallableEntriesCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
// @jshookz-allow-malformed
export const main = 1;
export const callback = 2;
)[test.tshook]"));

        auto const hostInitializingCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
// @jshookz-allow-malformed
export function main(_reserved: number): never {
  accept("entry", _reserved);
}
void ledger.sequence;
)[test.tshook]"));

        // The C-Hook prepare contract predates fixed-buffer host reads: the
        // declared length is bounds-checked, then the full prepared result is
        // copied. QuickJS deliberately selects the safer fixed-buffer contract.
        auto const& legacyPrepareCode = jshooks_test_wasm.at(R"[test.hook](
#include <stdint.h>
extern int32_t _g(uint32_t id, uint32_t maxiter);
extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t code);
extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t code);
extern int64_t etxn_reserve(uint32_t count);
extern int64_t prepare(
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len);

#define ASSERT(x) if (!(x)) return rollback(0, 0, __LINE__)

int64_t hook(uint32_t reserved)
{
    _g(1, 1);
    ASSERT(etxn_reserve(1) == 1);

    uint8_t input[8] = {0x12, 0x00, 0x03, 0x22, 0x80, 0x00, 0x00, 0x00};
    uint8_t exact[1024];
    int64_t n = prepare(
        (uint32_t)exact,
        sizeof(exact),
        (uint32_t)input,
        sizeof(input));
    ASSERT(n > 1 && n < sizeof(exact));

    uint8_t short_buffer[1024];
    short_buffer[n - 1] = exact[n - 1] ^ 0xFFU;
    ASSERT(prepare(
        (uint32_t)short_buffer,
        (uint32_t)(n - 1),
        (uint32_t)input,
        sizeof(input)) == n);
    ASSERT(short_buffer[n - 1] == exact[n - 1]);
    return accept(0, 0, reserved);
}
)[test.hook]");
        //@@end jshooks-hook-fixtures

        using namespace jtx;
        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        auto const carol = Account{"carol"};
        auto const features = supported_amendments();

        testcase("Preserve legacy C-Hook prepare copy semantics");
        {
            Env legacyEnv{*this, features};
            legacyEnv.fund(XRP(10000), alice, bob);
            legacyEnv.close();

            auto prepareHook = hso(legacyPrepareCode);
            prepareHook[jss::Flags] = hsfOVERRIDE;
            legacyEnv(
                jtx::hook(alice, {{prepareHook}}, 0),
                fee(XRP(10)),
                ter(tesSUCCESS));
            legacyEnv.close();
            legacyEnv(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
            legacyEnv.close();
        }

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
        BEAST_EXPECTS(!providerError, providerError.value_or(""));
        if (providerError)
            return;
        auto const currentRuntime = hook::findQuickJSRuntime(*currentArtifact);
        BEAST_EXPECT(!!currentRuntime);

        auto const successfulValidation =
            hook::validateQuickJSBytecodeForTests(currentRuntime, hookBytecode);
        BEAST_EXPECT(!successfulValidation.error);
        BEAST_EXPECT(!successfulValidation.hasCallback);
        expectFuel(successfulValidation.invocationFuelConsumed, 54534);

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
            expectFuel(result.invocationFuelConsumed, 54534);
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
            expectFuel(failedValidation.invocationFuelConsumed, 14029);
            identityEnv(
                jtx::hook(
                    alice,
                    {{hsoVersioned(
                        packageCurrentQuickJS(malformedBytecode), 1)}},
                    0),
                fee(XRP(10)),
                ter(temMALFORMED));
            identityEnv(
                jtx::hook(alice, {{hsoVersioned(missingMainCode, 1)}}, 0),
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
        auto conflictingObjectLimits = hook::currentQuickJSRuntimeProfile();
        ++conflictingObjectLimits.serializedObjectMaxBytes;
        BEAST_EXPECT(
            !!hook::registerQuickJSRuntime(conflictingObjectLimits, provider));
        conflictingObjectLimits = hook::currentQuickJSRuntimeProfile();
        ++conflictingObjectLimits.serializedObjectMaxFields;
        BEAST_EXPECT(
            !!hook::registerQuickJSRuntime(conflictingObjectLimits, provider));
        conflictingObjectLimits = hook::currentQuickJSRuntimeProfile();
        ++conflictingObjectLimits.serializedObjectMaxScopes;
        BEAST_EXPECT(
            !!hook::registerQuickJSRuntime(conflictingObjectLimits, provider));
        conflictingObjectLimits = hook::currentQuickJSRuntimeProfile();
        conflictingObjectLimits.serializedObjectMaxDepth = 0;
        BEAST_EXPECT(
            !!hook::registerQuickJSRuntime(conflictingObjectLimits, provider));
        auto conflictingMemory = hook::currentQuickJSRuntimeProfile();
        ++conflictingMemory.providerMemoryMinimumPages;
        BEAST_EXPECT(
            !!hook::registerQuickJSRuntime(conflictingMemory, provider));
        conflictingMemory = hook::currentQuickJSRuntimeProfile();
        conflictingMemory.providerMemoryMaximumPages =
            conflictingMemory.providerMemoryMinimumPages - 1;
        BEAST_EXPECT(
            !!hook::registerQuickJSRuntime(conflictingMemory, provider));
        //@@end jshooks-provider-fixture

        //@@start jshooks-provider-launch
        testcase("Launch a background provider registration");
        {
            auto launchedProfile = hook::currentQuickJSRuntimeProfile();
            launchedProfile.runtimeProfile[1] ^= 0xffU;
            auto launchedArtifact = *currentArtifact;
            launchedArtifact.runtimeProfile = launchedProfile.runtimeProfile;

            // Never launched: resolution and await answer without waiting.
            BEAST_EXPECT(!hook::findQuickJSRuntime(launchedArtifact));
            BEAST_EXPECT(
                !!hook::awaitQuickJSRuntimeRegistration(launchedProfile));

            BEAST_EXPECT(!hook::launchQuickJSRuntimeRegistration(
                launchedProfile, provider));

            // Concurrent resolution blocks on the compile, then agrees.
            std::array<std::future<hook::QuickJSRuntimeHandle>, 4>
                concurrentFinds;
            for (auto& find : concurrentFinds)
            {
                find = std::async(std::launch::async, [&] {
                    return hook::findQuickJSRuntime(launchedArtifact);
                });
            }
            auto const launchedRuntime =
                hook::findQuickJSRuntime(launchedArtifact);
            BEAST_EXPECT(!!launchedRuntime);
            for (auto& find : concurrentFinds)
                BEAST_EXPECT(find.get() == launchedRuntime);
            BEAST_EXPECT(
                !hook::awaitQuickJSRuntimeRegistration(launchedProfile));
            BEAST_EXPECT(launchedRuntime != currentRuntime);
            BEAST_EXPECT(
                hook::findQuickJSRuntime(*currentArtifact) == currentRuntime);

            // A same-profile relaunch is a no-op; a mismatched one is
            // refused and leaves the registered runtime alone.
            BEAST_EXPECT(!hook::launchQuickJSRuntimeRegistration(
                launchedProfile, provider));
            BEAST_EXPECT(
                hook::findQuickJSRuntime(launchedArtifact) == launchedRuntime);
            auto mismatchedProfile = launchedProfile;
            ++mismatchedProfile.invocationFuel;
            BEAST_EXPECT(!!hook::launchQuickJSRuntimeRegistration(
                mismatchedProfile, provider));
            mismatchedProfile = launchedProfile;
            ++mismatchedProfile.serializedObjectMaxFields;
            BEAST_EXPECT(!!hook::launchQuickJSRuntimeRegistration(
                mismatchedProfile, provider));
            BEAST_EXPECT(
                !!hook::awaitQuickJSRuntimeRegistration(mismatchedProfile));
            BEAST_EXPECT(
                hook::findQuickJSRuntime(launchedArtifact) == launchedRuntime);
        }

        testcase("Retain a failed background registration");
        {
            auto failedProfile = hook::currentQuickJSRuntimeProfile();
            failedProfile.runtimeProfile[2] ^= 0xffU;
            auto failedArtifact = *currentArtifact;
            failedArtifact.runtimeProfile = failedProfile.runtimeProfile;
            auto corruptedLaunchProvider = provider;
            corruptedLaunchProvider[0] ^= 0xff;

            BEAST_EXPECT(!hook::launchQuickJSRuntimeRegistration(
                failedProfile, std::move(corruptedLaunchProvider)));
            auto const failure =
                hook::awaitQuickJSRuntimeRegistration(failedProfile);
            BEAST_EXPECTS(
                failure &&
                    *failure ==
                        "provider SHA-256 does not match its runtime profile",
                failure ? *failure : "no retained error");
            BEAST_EXPECT(!hook::findQuickJSRuntime(failedArtifact));

            // Failure is terminal: relaunching with good bytes is refused
            // with the retained error and still resolves nothing.
            BEAST_EXPECT(
                hook::launchQuickJSRuntimeRegistration(
                    failedProfile, provider) == failure);
            BEAST_EXPECT(!hook::findQuickJSRuntime(failedArtifact));
            BEAST_EXPECT(
                hook::awaitQuickJSRuntimeRegistration(failedProfile) ==
                failure);
        }
        //@@end jshooks-provider-launch

        testcase("Measure per-session provider cost");
        {
            // Issue 0001 step 1: what one cold session costs on this tree,
            // before any Wizer/AOT change to the provider bytes. Numbers are
            // logged, not asserted; only the shape is checked.
            auto const cost = hook::measureQuickJSSessionCostForTests(
                currentRuntime, hookBytecode, 50);
            BEAST_EXPECTS(!cost.error, cost.error.value_or(""));
            BEAST_EXPECT(cost.iterations == 50);
            BEAST_EXPECT(cost.createNanosMin > 0);
            BEAST_EXPECT(cost.initializeNanosMin > 0);
            BEAST_EXPECT(cost.validateNanosMin > 0);
            BEAST_EXPECT(cost.initializationFuelConsumed > 0);
            BEAST_EXPECT(cost.invocationFuelConsumed > 0);
            auto const perIteration = [&](std::uint64_t total) {
                return cost.iterations ? total / cost.iterations : 0;
            };
            log << "provider session cost over " << cost.iterations
                << " cold sessions (mean/min microseconds): create "
                << perIteration(cost.createNanosTotal) / 1000 << "/"
                << cost.createNanosMin / 1000 << ", initialize "
                << perIteration(cost.initializeNanosTotal) / 1000 << "/"
                << cost.initializeNanosMin / 1000 << ", validate "
                << perIteration(cost.validateNanosTotal) / 1000 << "/"
                << cost.validateNanosMin / 1000 << "; fuel: initialization "
                << cost.initializationFuelConsumed << " of "
                << hook::currentQuickJSRuntimeProfile().initializationFuel
                << ", invocation " << cost.invocationFuelConsumed << " of "
                << hook::currentQuickJSRuntimeProfile().invocationFuel
                << std::endl;
        }

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
        expectFuel(execution.getFieldU64(sfHookInstructionCount), 64518);

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
        expectFuel(
            surfaceExecution.getFieldU64(sfHookInstructionCount), 101078);

        testcase("Execute accepted STObject and STArray on Wasmtime");
        auto stObjectHook = hsoVersioned(stObjectArrayCode, 1);
        stObjectHook[jss::Flags] = hsfOVERRIDE;
        env(jtx::hook(alice, {{stObjectHook}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        env.close();
        env(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
        env.close();
        auto const stObjectMeta = env.meta();
        BEAST_EXPECT(!!stObjectMeta);
        if (!stObjectMeta)
            return;
        auto const stObjectExecutions =
            stObjectMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(stObjectExecutions.size() == 1);
        if (stObjectExecutions.size() != 1)
            return;
        BEAST_EXPECT(
            stObjectExecutions[0].getFieldU8(sfHookResult) ==
            static_cast<uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(stObjectExecutions[0].getFieldU64(sfHookReturnCode) == 77);
        auto const stObjectMessage =
            stObjectExecutions[0].getFieldVL(sfHookReturnString);
        BEAST_EXPECT(
            std::string(stObjectMessage.begin(), stObjectMessage.end()) ==
            "stobject-starray");

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
        expectFuel(
            rollbackExecutions[0].getFieldU64(sfHookInstructionCount), 80256);

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
        expectFuel(
            memoryGrowthExecution.getFieldU64(sfHookInstructionCount), 7251198);
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
        expectFuel(
            hostWorkExecutions[0].getFieldU64(sfHookInstructionCount),
            30502768);

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
        expectFuel(
            callbackExecution.getFieldU64(sfHookInstructionCount), 136305);
        BEAST_EXPECT_EQ(
            callbackExecution.getFieldU8(sfHookResult),
            static_cast<std::uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT_EQ(
            callbackExecution.getFieldU64(sfHookReturnCode),
            std::uint64_t{202});
        auto const callbackMessage =
            callbackExecution.getFieldVL(sfHookReturnString);
        BEAST_EXPECT_EQ(
            std::string(callbackMessage.begin(), callbackMessage.end()),
            std::string("callback called"));

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
        BEAST_EXPECT_EQ(
            std::string(callbackData.begin(), callbackData.end()),
            std::string("called"));
    }
};

BEAST_DEFINE_TESTSUITE(JSHooks, app, ripple);

}  // namespace test
}  // namespace ripple
