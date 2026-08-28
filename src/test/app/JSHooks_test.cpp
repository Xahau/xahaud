//------------------------------------------------------------------------------
/*
    Transaction-level proof for TypeScript Hooks through the QuickJS provider.
*/
//==============================================================================
#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/QuickJSProviderProfile.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/hook/HookArtifact.h>
#include <xrpl/protocol/Keylet.h>
#include <xrpl/protocol/digest.h>
#include "JSHooks_bypass_test_hook.h"
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
    static std::uint16_t
    xflProfileCode(hook::artifact::XFLArithmeticProfile profile)
    {
        switch (profile)
        {
            case hook::artifact::XFLArithmeticProfile::none:
                return hook::artifact::generated::xflArithmeticProfileNone;
            case hook::artifact::XFLArithmeticProfile::xahauFloatV1:
                return hook::artifact::generated::
                    xflArithmeticProfileXahauFloatV1;
            case hook::artifact::XFLArithmeticProfile::nearestEvenV1:
                return hook::artifact::generated::
                    xflArithmeticProfileNearestEvenV1;
        }
        return hook::artifact::generated::xflArithmeticProfileNone;
    }

    static Blob
    readBinary(char const* path)
    {
        std::ifstream input{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{input}, {}};
    }

    static Blob
    packageCurrentQuickJS(
        Blob const& bytecode,
        hook::artifact::XFLArithmeticProfile profile =
            hook::artifact::XFLArithmeticProfile::none)
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
        auto const profileCode = xflProfileCode(profile);
        result[10] = static_cast<std::uint8_t>(profileCode >> 8);
        result[11] = static_cast<std::uint8_t>(profileCode);
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

        auto const& otxnObjectSmokeBytecode =
            jshooks_test_wasm.at(R"[test.tshook](
export function main(): never {
  const originating = otxn.object();
  rollback.when(
    originating.TransactionType !== TransactionType.Payment,
    "originating object was not Payment",
    58,
  );
  accept("otxn.object Payment smoke", 58);
}
)[test.tshook]");
        auto const otxnObjectSmokeCode =
            packageCurrentQuickJS(otxnObjectSmokeBytecode);

        auto const& xahauProfileBytecode = jshooks_test_wasm.at(R"[test.tshook](
export const hookConfig = defineHookConfig({
  xflArithmetic: XFLProfile.xahauFloatV1,
});

export function main(_reserved: number): never {
  void _reserved;
  accept("xahau profile configured", 301);
}
)[test.tshook]");
        auto const& xflAddSubtractBytecode =
            jshooks_test_wasm.at(R"[test.tshook](
export const hookConfig = defineHookConfig({
  xflArithmetic: XFLProfile.xahauFloatV1,
});

function decimal(hex: string): XFLDecimal {
  return rollback
    .requirePresent(
      util.decodeObject(STBlob.fromHex(hex)).get(Field.Amount),
      "arithmetic amount",
    )
    .toXFL();
}

export function main(_reserved: number): never {
  void _reserved;
  const addResult = rollback.onFail(
    decimal(
      "61D8438D7EA4C680000000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
    ).add(
      decimal(
        "61D8438D7EA4C680010000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
      ),
    ),
    "xfl add failed",
  );
  const subtractResult = rollback.onFail(
    decimal(
      "61D8438D7EA4C680640000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
    ).subtract(
      decimal(
        "61D8438D7EA4C680000000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
      ),
    ),
    "xfl subtract failed",
  );
  const expectedAdd = decimal(
    "61D8471AFD498D00010000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
  );
  const expectedSubtract = decimal(
    "61D5038D7EA4C680000000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
  );
  if (!addResult.equals(expectedAdd) || !subtractResult.equals(expectedSubtract)) {
    rollback("xfl arithmetic word mismatch", 6064);
  }
  accept("xfl add/subtract", 6060);
}
)[test.tshook]");
        auto const xflAddSubtractCode = packageCurrentQuickJS(
            xflAddSubtractBytecode,
            hook::artifact::XFLArithmeticProfile::xahauFloatV1);
        auto const& xflMultiplyBytecode = jshooks_test_wasm.at(R"[test.tshook](
export const hookConfig = defineHookConfig({
  xflArithmetic: XFLProfile.xahauFloatV1,
});

function decimal(hex: string): XFLDecimal {
  return rollback
    .requirePresent(
      util.decodeObject(STBlob.fromHex(hex)).get(Field.Amount),
      "multiply amount",
    )
    .toXFL();
}

export function main(_reserved: number): never {
  void _reserved;
  const result = rollback.onFail(
    decimal(
      "61D4871AFD498D00000000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
    ).multiply(
      decimal(
        "61D48AA87BEE5380000000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
      ),
    ),
    "xfl multiply failed",
  );
  const expected = decimal(
    "61D49550F7DCA700000000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
  );
  if (!result.equals(expected)) {
    rollback("xfl multiply word mismatch", 6073);
  }
  accept("xfl multiply", 6070);
}
)[test.tshook]");
        auto const xflMultiplyCode = packageCurrentQuickJS(
            xflMultiplyBytecode,
            hook::artifact::XFLArithmeticProfile::xahauFloatV1);
        auto const& xflFixedDivideBytecode =
            jshooks_test_wasm.at(R"[test.tshook](
export const hookConfig = defineHookConfig({
  xflArithmetic: XFLProfile.xahauFloatV1,
});

function decimal(hex: string): XFLDecimal {
  return rollback
    .requirePresent(
      util.decodeObject(STBlob.fromHex(hex)).get(Field.Amount),
      "divide amount",
    )
    .toXFL();
}

export function main(_reserved: number): never {
  void _reserved;
  const result = rollback.onFail(
    decimal(
      "61D84A8AFA8D4096130000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
    ).divide(
      decimal(
        "61D848026D25F9A8760000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
      ),
    ),
    "xfl divide failed",
  );
  const expected = decimal(
    "61D484AD2B4291BC3E0000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
  );
  if (!result.equals(expected)) {
    rollback("xfl fixed-divide word mismatch", 6074);
  }
  accept("xfl fixed divide", 6071);
}
)[test.tshook]");
        auto const xflFixedDivideCode = packageCurrentQuickJS(
            xflFixedDivideBytecode,
            hook::artifact::XFLArithmeticProfile::xahauFloatV1);
        auto const& xflDivideByZeroBytecode =
            jshooks_test_wasm.at(R"[test.tshook](
export const hookConfig = defineHookConfig({
  xflArithmetic: XFLProfile.xahauFloatV1,
});

function decimal(hex: string): XFLDecimal {
  return rollback
    .requirePresent(
      util.decodeObject(STBlob.fromHex(hex)).get(Field.Amount),
      "divide-zero amount",
    )
    .toXFL();
}

export function main(_reserved: number): never {
  void _reserved;
  const outcome = decimal(
    "61D4871AFD498D00000000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
  ).divide(
    decimal(
      "6180000000000000000000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
    ),
  ).okOrHandle((error) => error);
  if (outcome instanceof XFLDecimal) {
    rollback("xfl divide by zero succeeded", 6075);
  }
  if (
    outcome.domain !== "xfl" ||
    outcome.issue !== "division-by-zero" ||
    Object.getPrototypeOf(outcome) !== null ||
    Object.isExtensible(outcome)
  ) {
    rollback("xfl divide-by-zero Result mismatch", 6076);
  }
  accept("xfl divide by zero", 6072);
}
)[test.tshook]");
        auto const xflDivideByZeroCode = packageCurrentQuickJS(
            xflDivideByZeroBytecode,
            hook::artifact::XFLArithmeticProfile::xahauFloatV1);
        Blob const nearestEvenBypassBytecode{
            jshooksNearestEvenBypassBytecode.begin(),
            jshooksNearestEvenBypassBytecode.end()};
        auto const nearestEvenBypassCode = packageCurrentQuickJS(
            nearestEvenBypassBytecode,
            hook::artifact::XFLArithmeticProfile::nearestEvenV1);
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

        auto const f0NativeMatrixCode =
            packageCurrentQuickJS(jshooks_test_wasm.at(R"[test.tshook](
type UIntType<Bits extends UIntWidth> = RuntimeType<UInt<Bits>> & {
  readonly zero: UInt<Bits>;
  readonly max: UInt<Bits>;
  from(value: bigint | number): UIntResult<UInt<Bits>>;
};

function proveWrapping<Bits extends UIntWidth>(
  Type: UIntType<Bits>,
  bits: Bits,
  siblingType: RuntimeType<unknown>,
  siblingValue: UInt,
): void {
  const one = rollback.onFail(Type.from(1), `UInt${bits}.one`, -100 - bits);
  const two = rollback.onFail(Type.from(2), `UInt${bits}.two`, -200 - bits);
  const high = rollback.onFail(
    Type.from(1n << BigInt(bits - 1)),
    `UInt${bits}.high`,
    -300 - bits,
  );
  const vectors: readonly [string, UInt<Bits>][] = [
    ["add", Type.max.wrappingAdd(one)],
    ["subtract", Type.zero.wrappingSubtract(one)],
    ["multiply", Type.max.wrappingMultiply(two)],
    ["high-bit", high.wrappingAdd(high)],
  ];
  const expected = [0n, Type.max.toBigInt(), Type.max.toBigInt() - 1n, 0n];
  for (let index = 0; index < vectors.length; ++index) {
    const [name, value] = vectors[index];
    if (
      value.toBigInt() !== expected[index] ||
      value.bits !== bits ||
      !(value instanceof UInt) ||
      !(value instanceof Type) ||
      value instanceof siblingType
    ) {
      rollback(`UInt${bits}.${name}`, -400 - bits - index);
    }
  }
  let wrongWidthRejected = false;
  try {
    Type.max.wrappingAdd(siblingValue as UInt<Bits>);
  } catch (error) {
    wrongWidthRejected = error instanceof RangeError;
  }
  let primitiveRejected = false;
  try {
    Type.max.wrappingAdd(1n as unknown as UInt<Bits>);
  } catch (error) {
    primitiveRejected = error instanceof TypeError;
  }
  if (!wrongWidthRejected || !primitiveRejected) {
    rollback(`UInt${bits}.operand admission`, -500 - bits);
  }
}

export function main(_reserved: number): never {
  void _reserved;

  proveWrapping(UInt8, 8, UInt16, UInt16.zero);
  proveWrapping(UInt16, 16, UInt8, UInt8.zero);
  proveWrapping(UInt32, 32, UInt8, UInt8.zero);
  proveWrapping(UInt64, 64, UInt32, UInt32.zero);

  const mulDivSuccess = rollback.onFail(
    UInt64.mulDivXfl(UInt64.from(21n).okOr(UInt64.zero), 2n, 4n),
    "mulDivXfl success",
    -600,
  );
  const mulDivFloor = rollback.onFail(
    UInt64.mulDivXfl(10n, 1n, 3n),
    "mulDivXfl floor",
    -601,
  );
  const mulDivNormalization = rollback.onFail(
    UInt64.mulDivXfl(5n, 2n, 1n),
    "mulDivXfl normalization",
    -602,
  );
  const mulDivZeroIssue = UInt64.mulDivXfl(
    0n,
    UInt64.max,
    0n,
  ).okOrHandle((error) => error.issue);
  if (
    mulDivSuccess.toBigInt() !== 10n ||
    !(mulDivSuccess instanceof UInt64) ||
    mulDivFloor.toBigInt() !== 3n ||
    mulDivNormalization.toBigInt() !== 9n
  ) {
    rollback("UInt64.mulDivXfl values", -603);
  }
  if (mulDivZeroIssue !== "division-by-zero") {
    rollback("UInt64.mulDivXfl zero error", -604);
  }

  const empty = util.decodeObject(new Uint8Array());
  const hash128Value = rollback.requirePresent(
    empty.withField(Field.EmailHash, new Uint8Array(16)).get(Field.EmailHash),
    "Hash128 value",
    -610,
  );
  const lowBytes = new Uint8Array(32);
  lowBytes[0] = 1;
  lowBytes[31] = 255;
  const highBytes = new Uint8Array(32);
  highBytes[0] = 2;
  const lowHash = Hash256.from(lowBytes);
  const equalHash = Hash256.from(lowBytes);
  const highHash = Hash256.from(highBytes);
  let wrongHashBrandRejected = false;
  try {
    lowHash.compare(hash128Value as unknown as Hash256);
  } catch (error) {
    wrongHashBrandRejected = error instanceof TypeError;
  }
  const hashLookalike = { byteLength: 32, compare: () => 0 };
  if (
    lowHash.byteLength !== 32 ||
    lowHash.compare(equalHash) !== 0 ||
    lowHash.compare(highHash) !== -1 ||
    highHash.compare(lowHash) !== 1 ||
    !lowHash.equals(equalHash) ||
    !wrongHashBrandRejected ||
    !(lowHash instanceof Hash) ||
    !(lowHash instanceof Hash256) ||
    hash128Value instanceof Hash256 ||
    hashLookalike instanceof Hash ||
    hashLookalike instanceof Hash256
  ) {
    rollback("Hash256 matrix", -611);
  }

  const ordered = util.decodeObject(
    Uint8Array.from([
      0x24, 0x00, 0x00, 0x00, 0x07, 0x22, 0x00, 0x00, 0x00, 0x09,
    ]),
  );
  const flagsBytes = rollback.requirePresent(
    ordered.fieldBytes(Field.Flags),
    "Flags bytes",
    -620,
  );
  const publicKeyRoot = util.decodeObject(
    Uint8Array.from([0x71, 0x03, 0xaa, 0xbb, 0xcc]),
  );
  const publicKeyBytes = rollback.requirePresent(
    publicKeyRoot.fieldBytes(Field.PublicKey),
    "PublicKey bytes",
    -621,
  );
  const nestedRoot = util.decodeObject(
    Uint8Array.from([0xea, 0x22, 0x00, 0x00, 0x00, 0x09, 0xe1]),
  );
  const memoBytes = rollback.requirePresent(
    nestedRoot.fieldBytes(Field.Memo),
    "Memo bytes",
    -622,
  );
  if (
    flagsBytes.toHex() !== "00000009" ||
    publicKeyBytes.toHex() !== "AABBCC" ||
    memoBytes.toHex() !== "2200000009E1"
  ) {
    rollback("STObject.fieldBytes boundary", -623);
  }

  const accountIDValue = AccountID.from(new Uint8Array(20));
  const hash160Value = rollback.requirePresent(
    empty
      .withField(Field.TakerPaysCurrency, new Uint8Array(20))
      .get(Field.TakerPaysCurrency),
    "Hash160 value",
    -630,
  );
  const hash192Value = rollback.requirePresent(
    empty
      .withField(Field.MPTokenIssuanceID, new Uint8Array(24))
      .get(Field.MPTokenIssuanceID),
    "Hash192 value",
    -631,
  );
  const currencyValue = rollback.requirePresent(
    empty.withField(Field.BaseAsset, new Uint8Array(20)).get(Field.BaseAsset),
    "Currency value",
    -632,
  );
  const issueValue = rollback.requirePresent(
    empty
      .withField(Field.LockingChainIssue, new Uint8Array(20))
      .get(Field.LockingChainIssue),
    "Issue value",
    -633,
  );
  const vectorValue = rollback.requirePresent(
    util
      .decodeObject(
        STBlob.fromHex(
          "011320000102030405060708090A0B0C0D0E0F" +
            "101112131415161718191A1B1C1D1E1F",
        ),
      )
      .get(Field.Indexes),
    "Vector256 value",
    -634,
  );
  const bridgeValue = rollback.requirePresent(
    util
      .decodeObject(
        STBlob.fromHex(
          "011914B5F762798A53D543A014CAF8B297CFF8F2F937E8" +
            "0000000000000000000000000000000000000000" +
            "14B5F762798A53D543A014CAF8B297CFF8F2F937E8" +
            "0000000000000000000000000000000000000000",
        ),
      )
      .get(Field.XChainBridge),
    "XChainBridge value",
    -635,
  );
  const nativeAmountValue = rollback.requirePresent(
    util.decodeObject(STBlob.fromHex("61400000000000002A")).get(Field.Amount),
    "NativeAmount value",
    -636,
  );
  const iouAmountValue = rollback.requirePresent(
    util
      .decodeObject(
        STBlob.fromHex(
          "61D4838D7EA4C680000000000000000000000000005553440000000000" +
            "B5F762798A53D543A014CAF8B297CFF8F2F937E8",
        ),
      )
      .get(Field.Amount),
    "IOUAmount value",
    -637,
  );
  const mptAmountValue = rollback.requirePresent(
    util
      .decodeObject(
        STBlob.fromHex(
          "61600000000000000001000102030405060708090A0B0C0D0E0F" +
            "1011121314151617",
        ),
      )
      .get(Field.Amount),
    "MPTAmount value",
    -638,
  );
  const pathSetValue = rollback.requirePresent(
    util
      .decodeObject(
        STBlob.fromHex(
          "011201B5F762798A53D543A014CAF8B297CFF8F2F937E800",
        ),
      )
      .get(Field.Paths),
    "PathSet value",
    -639,
  );
  const pathValue = rollback.requirePresent(pathSetValue.at(0), "Path value", -640);
  const pathHopValue = rollback.requirePresent(pathValue.at(0), "PathHop value", -641);
  const stArrayValue = rollback.requirePresent(
    util
      .decodeObject(
        Uint8Array.from([
          0xf9, 0xea, 0x22, 0x00, 0x00, 0x00, 0x01, 0xe1, 0xf1,
        ]),
      )
      .get(Field.Memos),
    "STArray value",
    -642,
  );
  const stObjectValue = util.decodeObject(
    Uint8Array.from([0x22, 0x00, 0x00, 0x00, 0x09]),
  );
  const resultValue = UInt8.from(7);
  const xflValue = rollback.requirePresent(
    iouAmountValue.asIOU(),
    "IOU narrowing",
    -643,
  ).toXFL();
  const resultBehavior = resultValue.okMapOr(
    (value) => value.toBigInt(),
    -1n,
  );

  // Result ownership is enforced statically, so the provider-owned dynamic
  // evaluation surface keeps each Result inside one create-observe-consume
  // scope while exercising the exact provider F0 nominal-membership grid.
  const nounMatrixFailures = eval(`
(() => {
  const resultValue = UInt8.from(7);
  const voidResultValue = state.set("f0-native-matrix", Uint8Array.from([1]));
  const nouns = [
    "AccountID", "Amount", "Currency", "Hash", "Hash128", "Hash160",
    "Hash192", "Hash256", "IOUAmount", "Issue", "MPTAmount",
    "NativeAmount", "Path", "PathHop", "PathSet", "Result", "STArray",
    "STBlob", "STObject", "SerializedField", "UInt", "UInt8", "UInt16",
    "UInt32", "UInt64", "Vector256", "VoidResult", "XChainBridge",
    "XFLDecimal",
  ];
  const cases = [
    ["AccountID", accountIDValue, ["AccountID"]],
    ["native amount", nativeAmountValue, ["Amount", "NativeAmount"]],
    ["IOU amount", iouAmountValue, ["Amount", "IOUAmount"]],
    ["MPT amount", mptAmountValue, ["Amount", "MPTAmount"]],
    ["Currency", currencyValue, ["Currency"]],
    ["Hash128", hash128Value, ["Hash", "Hash128"]],
    ["Hash160", hash160Value, ["Hash", "Hash160"]],
    ["Hash192", hash192Value, ["Hash", "Hash192"]],
    ["Hash256", lowHash, ["Hash", "Hash256"]],
    ["Issue", issueValue, ["Issue"]],
    ["Path", pathValue, ["Path"]],
    ["PathHop", pathHopValue, ["PathHop"]],
    ["PathSet", pathSetValue, ["PathSet"]],
    ["Result", resultValue, ["Result"]],
    ["STArray", stArrayValue, ["STArray"]],
    ["STBlob", STBlob.from(new Uint8Array()), ["STBlob"]],
    ["STObject", stObjectValue, ["STObject"]],
    ["SerializedField", Field.Flags, ["SerializedField"]],
    ["UInt8", UInt8.zero, ["UInt", "UInt8"]],
    ["UInt16", UInt16.zero, ["UInt", "UInt16"]],
    ["UInt32", UInt32.zero, ["UInt", "UInt32"]],
    ["UInt64", UInt64.zero, ["UInt", "UInt64"]],
    ["Vector256", vectorValue, ["Vector256"]],
    ["VoidResult", voidResultValue, ["VoidResult"]],
    ["XChainBridge", bridgeValue, ["XChainBridge"]],
    ["XFLDecimal", xflValue, ["XFLDecimal"]],
  ];
  const failures = [];
  const fail = (where) => failures.push(where);
  if (nouns.length !== 29 || new Set(nouns).size !== 29 || cases.length !== 26)
    fail("matrix:shape");
  for (const [label, instance, positives] of cases) {
    const expected = new Set(positives);
    for (const name of nouns) {
      if ((instance instanceof globalThis[name]) !== expected.has(name))
        fail("matrix:" + label + ":" + name);
    }
  }

  const resultNominal = resultValue instanceof Result;
  const voidResultNominal = voidResultValue instanceof VoidResult;
  if (!resultNominal || resultValue instanceof VoidResult)
    fail("matrix:Result:explicit-control");
  if (!voidResultNominal || voidResultValue instanceof Result)
    fail("matrix:VoidResult:explicit-control");
  if (UInt8.zero instanceof Result || UInt8.zero instanceof VoidResult)
    fail("matrix:non-result:explicit-control");

  const matrixResultBehavior = resultValue.okMapOr(
    (value) => value.toBigInt(),
    -1n,
  );
  rollback.onFail(voidResultValue, "state.set failed");
  if (matrixResultBehavior !== 7n) fail("matrix:Result:behavior");
  return failures;
})()
`) as unknown;
  if (
    !Array.isArray(nounMatrixFailures) ||
    nounMatrixFailures.some((failure) => typeof failure !== "string") ||
    nounMatrixFailures.length !== 0
  ) {
    const detail = Array.isArray(nounMatrixFailures)
      ? nounMatrixFailures.join(",")
      : "invalid-result";
    rollback(`29-noun exact matrix:${detail}`, -650);
  }

  if (
    resultBehavior !== 7n ||
    UInt8.from(256).okOr(UInt8.max) !== UInt8.max ||
    rollback.requirePresent(0, "present zero", -660) !== 0 ||
    rollback.requireTruthy(true, "truthy", -661) !== true ||
    accept.unlessPresent(1, "present value", -662) !== 1 ||
    accept.unlessTruthy(true, "truthy value", -663) !== true ||
    rollback.onFail(UInt8.from(3), "onFail", -664).toBigInt() !== 3n ||
    rollback.onAnyFail(
      [UInt8.from(4), UInt8.from(5)],
      "onAnyFail",
      -665,
    ).length !== 2 ||
    rollback.onAllFail(
      [UInt8.from(256), UInt8.from(6)],
      "onAllFail",
      -666,
    )[0].toBigInt() !== 6n
  ) {
    rollback("Result/control verbs", -667);
  }
  const mootResult = state.set("f0-native-moot", Uint8Array.from([2]));
  mootResult.moot();
  rollback.when(false, "rollback.when", -669);
  accept.when(false, "accept.when", -670);
  accept("f0-native-matrix", 88);
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

  const chunk = "x".repeat(2000);
  for (let i = 0; i < 1046; ++i) trace("meter", chunk);
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

        auto const& callbackBytecode = jshooks_test_wasm.at(R"[test.tshook](
export const hookConfig = defineHookConfig({
  xflArithmetic: XFLProfile.nearestEvenV1,
});

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
)[test.tshook]");
        auto const callbackCode = packageCurrentQuickJS(
            callbackBytecode,
            hook::artifact::XFLArithmeticProfile::nearestEvenV1);

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
        BEAST_EXPECT(
            successfulValidation.xflArithmeticProfile ==
            hook::artifact::XFLArithmeticProfile::none);
        expectFuel(successfulValidation.invocationFuelConsumed, 58969);

        auto const xahauValidation = hook::validateQuickJSBytecodeForTests(
            currentRuntime, xahauProfileBytecode);
        BEAST_EXPECT(!xahauValidation.error);
        BEAST_EXPECT(!xahauValidation.hasCallback);
        BEAST_EXPECT(
            xahauValidation.xflArithmeticProfile ==
            hook::artifact::XFLArithmeticProfile::xahauFloatV1);

        auto const xflAddSubtractValidation =
            hook::validateQuickJSBytecodeForTests(
                currentRuntime, xflAddSubtractBytecode);
        BEAST_EXPECT(!xflAddSubtractValidation.error);
        BEAST_EXPECT(!xflAddSubtractValidation.hasCallback);
        BEAST_EXPECT(
            xflAddSubtractValidation.xflArithmeticProfile ==
            hook::artifact::XFLArithmeticProfile::xahauFloatV1);

        for (auto const* arithmeticBytecode :
             {&xflMultiplyBytecode,
              &xflFixedDivideBytecode,
              &xflDivideByZeroBytecode})
        {
            auto const validation = hook::validateQuickJSBytecodeForTests(
                currentRuntime, *arithmeticBytecode);
            BEAST_EXPECT(!validation.error);
            BEAST_EXPECT(!validation.hasCallback);
            BEAST_EXPECT(
                validation.xflArithmeticProfile ==
                hook::artifact::XFLArithmeticProfile::xahauFloatV1);
        }

        auto const nearestEvenBypassValidation =
            hook::validateQuickJSBytecodeForTests(
                currentRuntime, nearestEvenBypassBytecode);
        BEAST_EXPECT(!nearestEvenBypassValidation.error);
        BEAST_EXPECT(!nearestEvenBypassValidation.hasCallback);
        BEAST_EXPECT(
            nearestEvenBypassValidation.xflArithmeticProfile ==
            hook::artifact::XFLArithmeticProfile::nearestEvenV1);
        BEAST_EXPECT(
            std::string_view{jshooksBypassProviderSHA256} ==
            "ef7ac257b5a25d3ac329bfb795480cd4fa4d64fb2bce6e49bb5e1bcc556fab34");
        auto const callbackValidation = hook::validateQuickJSBytecodeForTests(
            currentRuntime, callbackBytecode);
        BEAST_EXPECT(!callbackValidation.error);
        BEAST_EXPECT(callbackValidation.hasCallback);
        BEAST_EXPECT(
            callbackValidation.xflArithmeticProfile ==
            hook::artifact::XFLArithmeticProfile::nearestEvenV1);

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
            BEAST_EXPECT(
                result.xflArithmeticProfile ==
                hook::artifact::XFLArithmeticProfile::none);
            expectFuel(result.invocationFuelConsumed, 58969);
        }

        testcase("Bind XFL profile at QuickJS CREATE admission");
        {
            Env profileEnv{*this, features | featureJSHooks};
            profileEnv.fund(XRP(10000), alice, bob, carol);
            profileEnv.close();

            struct ProfileModule
            {
                Blob const& bytecode;
                hook::artifact::XFLArithmeticProfile profile;
                Account const& account;
            };
            std::array<ProfileModule, 3> const modules = {{
                {hookBytecode,
                 hook::artifact::XFLArithmeticProfile::none,
                 alice},
                {xahauProfileBytecode,
                 hook::artifact::XFLArithmeticProfile::xahauFloatV1,
                 bob},
                {callbackBytecode,
                 hook::artifact::XFLArithmeticProfile::nearestEvenV1,
                 carol},
            }};
            constexpr std::array profiles = {
                hook::artifact::XFLArithmeticProfile::none,
                hook::artifact::XFLArithmeticProfile::xahauFloatV1,
                hook::artifact::XFLArithmeticProfile::nearestEvenV1,
            };

            for (auto const& module : modules)
            {
                for (auto const headerProfile : profiles)
                {
                    if (headerProfile == module.profile)
                        continue;
                    auto const candidate =
                        packageCurrentQuickJS(module.bytecode, headerProfile);
                    auto const candidateHash =
                        sha512Half_s(makeSlice(candidate));
                    profileEnv(
                        jtx::hook(
                            module.account, {{hsoVersioned(candidate, 1)}}, 0),
                        fee(XRP(10)),
                        ter(temMALFORMED));
                    BEAST_EXPECT(
                        !profileEnv.le(keylet::hookDefinition(candidateHash)));
                }

                auto const matching =
                    packageCurrentQuickJS(module.bytecode, module.profile);
                auto const matchingHash = sha512Half_s(makeSlice(matching));
                profileEnv(
                    jtx::hook(module.account, {{hsoVersioned(matching, 1)}}, 0),
                    fee(XRP(10)),
                    ter(tesSUCCESS));
                profileEnv.close();
                BEAST_EXPECT(
                    !!profileEnv.le(keylet::hookDefinition(matchingHash)));
            }
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
            expectFuel(failedValidation.invocationFuelConsumed, 14161);
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
        expectFuel(execution.getFieldU64(sfHookInstructionCount), 69059);

        testcase("Execute packaged otxn.object Payment smoke");
        auto otxnObjectSmokeHook = hsoVersioned(otxnObjectSmokeCode, 1);
        otxnObjectSmokeHook[jss::Flags] = hsfOVERRIDE;
        env(jtx::hook(alice, {{otxnObjectSmokeHook}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        env.close();
        env(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));

        auto const smokeMeta = env.meta();
        BEAST_EXPECT(!!smokeMeta);
        if (!smokeMeta || !smokeMeta->isFieldPresent(sfHookExecutions))
            return;
        auto const smokeExecutions = smokeMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(smokeExecutions.size() == 1);
        if (smokeExecutions.size() != 1)
            return;
        auto const& smokeExecution = smokeExecutions[0];
        BEAST_EXPECT(
            smokeExecution.getFieldU8(sfHookResult) ==
            static_cast<std::uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(smokeExecution.getFieldU64(sfHookReturnCode) == 58);
        auto const smokeMessage = smokeExecution.getFieldVL(sfHookReturnString);
        BEAST_EXPECT(
            std::string(smokeMessage.begin(), smokeMessage.end()) ==
            "otxn.object Payment smoke");
        env.close();

        testcase("Execute packaged xahauFloatV1 add and subtract");
        Env xflEnv{*this, features | featureJSHooks};
        xflEnv.fund(XRP(10000), alice, bob);
        xflEnv.close();
        xflEnv(
            jtx::hook(alice, {{hsoVersioned(xflAddSubtractCode, 1)}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        xflEnv.close();
        xflEnv(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
        xflEnv.close();

        auto const xflMeta = xflEnv.meta();
        BEAST_EXPECT(!!xflMeta);
        if (!xflMeta || !xflMeta->isFieldPresent(sfHookExecutions))
            return;
        auto const xflExecutions = xflMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(xflExecutions.size() == 1);
        if (xflExecutions.size() != 1)
            return;
        auto const& xflExecution = xflExecutions[0];
        BEAST_EXPECT(
            xflExecution.getFieldU8(sfHookResult) ==
            static_cast<std::uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(xflExecution.getFieldU64(sfHookReturnCode) == 6060);
        auto const xflMessage = xflExecution.getFieldVL(sfHookReturnString);
        BEAST_EXPECT(
            std::string(xflMessage.begin(), xflMessage.end()) ==
            "xfl add/subtract");
        expectFuel(xflExecution.getFieldU64(sfHookInstructionCount), 293927);

        testcase("Execute packaged xahauFloatV1 multiply");
        auto xflMultiplyHook = hsoVersioned(xflMultiplyCode, 1);
        xflMultiplyHook[jss::Flags] = hsfOVERRIDE;
        xflEnv(
            jtx::hook(alice, {{xflMultiplyHook}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        xflEnv.close();
        xflEnv(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
        xflEnv.close();

        auto const multiplyMeta = xflEnv.meta();
        BEAST_EXPECT(!!multiplyMeta);
        if (!multiplyMeta || !multiplyMeta->isFieldPresent(sfHookExecutions))
            return;
        auto const multiplyExecutions =
            multiplyMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(multiplyExecutions.size() == 1);
        if (multiplyExecutions.size() != 1)
            return;
        auto const& multiplyExecution = multiplyExecutions[0];
        BEAST_EXPECT(
            multiplyExecution.getFieldU8(sfHookResult) ==
            static_cast<std::uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(multiplyExecution.getFieldU64(sfHookReturnCode) == 6070);
        auto const multiplyMessage =
            multiplyExecution.getFieldVL(sfHookReturnString);
        BEAST_EXPECT(
            std::string(multiplyMessage.begin(), multiplyMessage.end()) ==
            "xfl multiply");
        expectFuel(
            multiplyExecution.getFieldU64(sfHookInstructionCount), 196224);

        testcase("Execute packaged xahauFloatV1 fixed divide last digit");
        auto xflFixedDivideHook = hsoVersioned(xflFixedDivideCode, 1);
        xflFixedDivideHook[jss::Flags] = hsfOVERRIDE;
        xflEnv(
            jtx::hook(alice, {{xflFixedDivideHook}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        xflEnv.close();
        xflEnv(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
        xflEnv.close();

        auto const fixedDivideMeta = xflEnv.meta();
        BEAST_EXPECT(!!fixedDivideMeta);
        if (!fixedDivideMeta ||
            !fixedDivideMeta->isFieldPresent(sfHookExecutions))
            return;
        auto const fixedDivideExecutions =
            fixedDivideMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(fixedDivideExecutions.size() == 1);
        if (fixedDivideExecutions.size() != 1)
            return;
        auto const& fixedDivideExecution = fixedDivideExecutions[0];
        BEAST_EXPECT(
            fixedDivideExecution.getFieldU8(sfHookResult) ==
            static_cast<std::uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(
            fixedDivideExecution.getFieldU64(sfHookReturnCode) == 6071);
        auto const fixedDivideMessage =
            fixedDivideExecution.getFieldVL(sfHookReturnString);
        BEAST_EXPECT(
            std::string(fixedDivideMessage.begin(), fixedDivideMessage.end()) ==
            "xfl fixed divide");
        expectFuel(
            fixedDivideExecution.getFieldU64(sfHookInstructionCount), 198045);

        testcase("Return nominal Result for packaged divide by zero");
        auto xflDivideByZeroHook = hsoVersioned(xflDivideByZeroCode, 1);
        xflDivideByZeroHook[jss::Flags] = hsfOVERRIDE;
        xflEnv(
            jtx::hook(alice, {{xflDivideByZeroHook}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        xflEnv.close();
        xflEnv(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
        xflEnv.close();

        auto const divideByZeroMeta = xflEnv.meta();
        BEAST_EXPECT(!!divideByZeroMeta);
        if (!divideByZeroMeta ||
            !divideByZeroMeta->isFieldPresent(sfHookExecutions))
            return;
        auto const divideByZeroExecutions =
            divideByZeroMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(divideByZeroExecutions.size() == 1);
        if (divideByZeroExecutions.size() != 1)
            return;
        auto const& divideByZeroExecution = divideByZeroExecutions[0];
        BEAST_EXPECT(
            divideByZeroExecution.getFieldU8(sfHookResult) ==
            static_cast<std::uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(
            divideByZeroExecution.getFieldU64(sfHookReturnCode) == 6072);
        auto const divideByZeroMessage =
            divideByZeroExecution.getFieldVL(sfHookReturnString);
        BEAST_EXPECT(
            std::string(
                divideByZeroMessage.begin(), divideByZeroMessage.end()) ==
            "xfl divide by zero");
        expectFuel(
            divideByZeroExecution.getFieldU64(sfHookInstructionCount), 192186);

        testcase("Fail closed for packaged nearestEvenV1 arithmetic bypass");
        auto nearestEvenBypassHook = hsoVersioned(nearestEvenBypassCode, 1);
        nearestEvenBypassHook[jss::Flags] = hsfOVERRIDE;
        xflEnv(
            jtx::hook(alice, {{nearestEvenBypassHook}}, 0),
            fee(XRP(10)),
            ter(tesSUCCESS));
        xflEnv.close();
        xflEnv(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
        xflEnv.close();

        auto const bypassMeta = xflEnv.meta();
        BEAST_EXPECT(!!bypassMeta);
        if (!bypassMeta || !bypassMeta->isFieldPresent(sfHookExecutions))
            return;
        auto const bypassExecutions =
            bypassMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(bypassExecutions.size() == 1);
        if (bypassExecutions.size() != 1)
            return;
        auto const& bypassExecution = bypassExecutions[0];
        BEAST_EXPECT(
            bypassExecution.getFieldU8(sfHookResult) ==
            static_cast<std::uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(bypassExecution.getFieldU64(sfHookReturnCode) == 6061);
        auto const bypassMessage =
            bypassExecution.getFieldVL(sfHookReturnString);
        BEAST_EXPECT(
            std::string(bypassMessage.begin(), bypassMessage.end()) ==
            "xfl profile backstop");
        expectFuel(bypassExecution.getFieldU64(sfHookInstructionCount), 204271);

        auto const bypassStateKey = uint256::fromVoid(
            (std::array<uint8_t, 32>{
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 'x',   'f',
                 'l',   '-',   'b',   'y',   'p',   'a',   's',   's'})
                .data());
        BEAST_EXPECT(!xflEnv.le(keylet::hookState(
            alice.id(), bypassStateKey, uint256{beast::zero})));

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
            surfaceExecution.getFieldU64(sfHookInstructionCount), 102195);

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

        testcase("Execute the sealed F0 native API matrix on Wasmtime");
        auto f0NativeMatrixHook = hsoVersioned(f0NativeMatrixCode, 1);
        f0NativeMatrixHook[jss::Flags] = hsfOVERRIDE;
        env(jtx::hook(alice, {{f0NativeMatrixHook}}, 0),
            fee(XRP(100)),
            ter(tesSUCCESS));
        env.close();
        env(pay(bob, alice, XRP(1)), fee(XRP(100)), ter(tesSUCCESS));
        env.close();
        auto const f0NativeMatrixMeta = env.meta();
        BEAST_EXPECT(!!f0NativeMatrixMeta);
        if (!f0NativeMatrixMeta)
            return;
        auto const f0NativeMatrixExecutions =
            f0NativeMatrixMeta->getFieldArray(sfHookExecutions);
        BEAST_EXPECT(f0NativeMatrixExecutions.size() == 1);
        if (f0NativeMatrixExecutions.size() != 1)
            return;
        auto const& f0NativeMatrixExecution = f0NativeMatrixExecutions[0];
        BEAST_EXPECT(
            f0NativeMatrixExecution.getFieldU8(sfHookResult) ==
            static_cast<uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(
            f0NativeMatrixExecution.getFieldU64(sfHookReturnCode) == 88);
        auto const f0NativeMatrixMessage =
            f0NativeMatrixExecution.getFieldVL(sfHookReturnString);
        BEAST_EXPECT(
            std::string(
                f0NativeMatrixMessage.begin(), f0NativeMatrixMessage.end()) ==
            "f0-native-matrix");
        expectFuel(
            f0NativeMatrixExecution.getFieldU64(sfHookInstructionCount),
            6869429);

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
            rollbackExecutions[0].getFieldU64(sfHookInstructionCount), 81682);

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
            memoryGrowthExecution.getFieldU64(sfHookInstructionCount), 7457206);
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
            42541799);

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
            callbackExecution.getFieldU64(sfHookInstructionCount), 118216);
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
