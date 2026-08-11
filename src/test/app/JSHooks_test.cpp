//------------------------------------------------------------------------------
/*
    Transaction-level proof for TypeScript Hooks through the QuickJS provider.
*/
//==============================================================================
#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/Keylet.h>
#include "JSHooks_test_hooks.h"
#include <array>
#include <cstdlib>
#include <fstream>
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

public:
    void
    run() override
    {
        //@@start jshooks-provider-fixture
        auto const* providerPath = std::getenv("XAHAU_QJS_PROVIDER_WASM");
        if (!providerPath)
        {
            testcase("QuickJS Hook integration (provider fixture absent)");
            pass();
            return;
        }

        auto provider = readBinary(providerPath);
        BEAST_EXPECT(!provider.empty());
        if (provider.empty())
            return;
        hook::setQuickJSProviderForTests(std::move(provider));
        //@@end jshooks-provider-fixture

        //@@start jshooks-hook-fixtures
        auto const& hookCode = jshooks_test_wasm[R"[test.tshook](
export function hook(_reserved: number): never {
  void _reserved;
  const txType = otxn.type();
  if (!txType.ok) lifecycle.rollback("otxn.type failed", txType.code);
  if (txType.value !== TransactionType.Payment) {
    lifecycle.rollback("expected Payment", txType.value);
  }
  lifecycle.accept(`payment:${txType.value}`, 42);
}
)[test.tshook]"];

        auto const& stateSeedCode = jshooks_test_wasm[R"[test.hook](
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
)[test.hook]"];

        auto const& stateBridgeCode = jshooks_test_wasm[R"[test.tshook](
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
)[test.tshook]"];

        auto const& stateRollbackCode = jshooks_test_wasm[R"[test.tshook](
export function hook(_reserved: number): never {
  void _reserved;
  const write = state.set("bridge", "must-not-stick");
  if (!write.ok) lifecycle.rollback("state write failed", write.code);
  trace("js-state-rollback", "must-not-stick");
        lifecycle.rollback("state rollback", -84);
}
)[test.tshook]"];
        //@@end jshooks-hook-fixtures

        using namespace jtx;
        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        auto const features = supported_amendments();

        //@@start jshooks-amendment-and-entry
        testcase("Reject a TypeScript Hook before JSHooks activation");
        {
            Env env{*this, features};
            env.fund(XRP(10000), alice);
            env.close();
            env(jtx::hook(alice, {{hsoVersioned(hookCode, 1)}}, 0),
                fee(XRP(10)),
                ter(temMALFORMED));
        }

        testcase("Set and execute a TypeScript Hook transaction");
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
        BEAST_EXPECT(execution.getFieldU64(sfHookInstructionCount) > 0);
        //@@end jshooks-amendment-and-entry

        //@@start jshooks-state-bridge
        testcase("Read C Hook state from TypeScript and replace it");

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

        stateEntry = env.le(stateKeylet);
        BEAST_EXPECT(!!stateEntry);
        if (!stateEntry)
            return;
        auto const afterRollbackData = stateEntry->getFieldVL(sfHookStateData);
        BEAST_EXPECT(
            std::string(afterRollbackData.begin(), afterRollbackData.end()) ==
            "from-js");
        //@@end jshooks-rollback-atomicity
    }
};

BEAST_DEFINE_TESTSUITE(JSHooks, app, ripple);

}  // namespace test
}  // namespace ripple
