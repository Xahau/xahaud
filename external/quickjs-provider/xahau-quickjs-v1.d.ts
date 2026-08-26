export {};

declare const __stObjectBrand: unique symbol;
declare const __stArrayBrand: unique symbol;
declare const __providerValueBrand: unique symbol;
declare const __binarySchemaBrand: unique symbol;
declare const __resultBrand: unique symbol;
declare const __voidResultBrand: unique symbol;
declare const __recordFieldBrand: unique symbol;
declare const __serializedFieldBrand: unique symbol;

/**
 * Type-only surface shared by every nominal provider-produced Result.
 *
 * There are five ways to leave a value Result; anything else is a bug:
 * `okOr`, `okOrHandle`, `okMapOr`, exhaustive `.ok` narrowing, or a
 * terminal consumer (`rollback.*`, `accept.unlessPresent`,
 * `accept.unlessTruthy`). `moot` lives on the effect family
 * (`__VoidResultOps`), which is nominally not a Result.
 */
interface __ResultOps<T, Error> {
  readonly [__resultBrand]: [T, Error];

  /**
   * Return `.value` whenever `.ok` is true, including a successful
   * `undefined`; return `fallback` only when `.ok` is false. The fallback
   * expression is evaluated before this method is called; use `okOrHandle`
   * when producing it has work or side effects.
  */
  okOr<Fallback>(fallback: Fallback): T | Fallback;

  /**
   * Return `.value` whenever `.ok` is true; otherwise invoke `handler` once
   * with `.error` and return the value it produces.
   *
   * A handler may terminate instead of producing a fallback — a
   * `never`-returning handler is legitimate Result elimination, and
   * `rollback.onFail` is the idiomatic spelling of that pattern.
   */
  okOrHandle<Fallback>(handler: (error: Error) => Fallback): T | Fallback;

  /**
   * Transform a successful value and return it directly; return `fallback`
   * when this Result is a failure. The fallback expression is evaluated
   * before this method is called.
   *
   * Use this only when discarding the failure is deliberate. In particular,
   * malformed persisted state should normally remain loud rather than be
   * collapsed into the same value used for absent state.
   */
  okMapOr<Value, Fallback>(
    handler: (value: T) => Value,
    fallback: Fallback,
  ): Value | Fallback;

}

/**
 * Nominal family of effect Results: host writes whose success carries no
 * value. Deliberately NOT assignable to `Result<T, Error>` — an effect
 * outcome is consumed by `moot`, an eliminator, or `rollback.onFail`,
 * never by a value verb.
 */
interface __VoidResultOps<Error> {
  readonly [__voidResultBrand]: Error;

  /**
   * Declare the failure of this effect Result moot: neither outcome bears
   * on contract correctness. JavaScript exceptions and provider faults are
   * not Result failures and are not suppressed.
   */
  moot(): void;

  /** As on value Results; a successful effect yields `undefined`. */
  okOr<Fallback>(fallback: Fallback): undefined | Fallback;
  okOrHandle<Fallback>(handler: (error: Error) => Fallback): undefined | Fallback;
  okMapOr<Value, Fallback>(
    handler: (value: void) => Value,
    fallback: Fallback,
  ): Value | Fallback;
}

/**
 * Member inventory of the frozen `XFLProfile` enum namespace. The public
 * value is the `XFLProfile` global; the literal member types are the one
 * profile-code table shared by source, runtime, provider validation, and
 * the XQJS artifact (0 means absence only and is never declarable).
 */
interface XFLProfileEnum {
  readonly xahauFloatV1: 1;
  readonly nearestEvenV1: 2;
}

declare global {
  /**
   * Exact JavaScript surface implemented by the sealed xahau-quickjs-v1
   * provider. The accompanying API artifact manifest closes this declaration,
   * its broader specification, and the provider surface manifest by hash.
   */

  /**
   * The one shape of a provider value's public runtime noun (0085:780-…):
   * a same-named, frozen, NON-constructible type object that owns the
   * type's factories/statics and classifies its instances —
   * `value instanceof Type` works and narrows. `new` is not part of the
   * API; no `.prototype` is promised.
   */
  interface RuntimeType<T> {
    [Symbol.hasInstance](value: unknown): value is T;
  }

  type BytesLike = Uint8Array | ArrayBuffer | readonly number[];

  /** Contiguous serialized-object input; indexed JavaScript arrays are excluded. */
  type ObjectBytes = Uint8Array | ArrayBuffer | STBlob;

  type HexString = string;

  type UIntWidth = 8 | 16 | 32 | 64;

  type UIntInput<Bits extends UIntWidth = UIntWidth> = UInt<Bits> | bigint | number;

  type Drops = bigint;

  type LedgerSequence = number;

  type RippleTime = number;

  type HashWidth = 16 | 20 | 24 | 32 | 48 | 64;

  type JSFalsy = false | 0 | 0n | "" | null | undefined;

  /**
   * Rich provider values (AccountID, Hash256, STBlob, the UInt family,
   * XFLDecimal, Amount, Currency…) are ordinary objects and therefore
   * ALWAYS JS-truthy — including every zero and sentinel state, e.g.
   * `UInt64.zero`, `Hash256.zero`, and `AccountID.zero`. Their
   * zero/native/sentinel STATES are domain facts, tested with
   * `isZero()`, `isNative`, `equals(...)` and friends — never with
   * truthiness. Presence is its own question: the Present verbs. These
   * aliases name the LANGUAGE's coercion law, not a domain claim.
   */
  type JSTruthy<T> = Exclude<T, JSFalsy>;

  /** A successful, non-nullish value; falsy-but-present values qualify. */
  type Present<T> = Exclude<T, null | undefined>;

  /** Conceptual result nouns (0085 close): the runtime already
   * classifies via isResult/isEffectResult; these make it public. */
  const Result: RuntimeType<Result<unknown, unknown>>;

  const VoidResult: RuntimeType<VoidResult<unknown>>;

  type VoidResultSuccess<Error> = __VoidResultOps<Error> & {
    readonly ok: true;
  };

  type VoidResultFailure<Error> = __VoidResultOps<Error> & {
    readonly ok: false;
    readonly error: Error;
  };

  type VoidResult<Error> = VoidResultSuccess<Error> | VoidResultFailure<Error>;

  type HostVoidResult = VoidResult<HostError>;

  /** Shared discriminated carrier; each domain owns its error payload. */
  type ResultSuccess<T, Error> = __ResultOps<T, Error> & {
    readonly ok: true;
    readonly value: T;
  };

  type ResultFailure<T, Error> = __ResultOps<T, Error> & {
    readonly ok: false;
    readonly error: Error;
  };

  type Result<T, Error> = ResultSuccess<T, Error> | ResultFailure<T, Error>;

  interface HostError {
    readonly domain: "host";
    readonly code: HookReturnCode;
  }

  /**
   * The result of an operation governed by Hooks host-status semantics.
   *
   * A negative Hooks status is ordinary contract-visible data, not an exception.
   * Successful optional reads may still contain `undefined` when the operation
   * explicitly folds `DOESNT_EXIST` into absence; every other host failure keeps
   * its exact `HookReturnCode`. Hook termination and JavaScript/runtime faults
   * remain exceptions.
   *
   * This carrier does not prove that a Wasm host bridge was physically crossed.
   * Provider-side rich facades may return `HostResult` when they enforce the
   * same host rules and preserve the corresponding Hook status exactly.
   */
  type HostResult<T> = Result<T, HostError>;

  /** A pure binary-codec validation result; no Hooks host call occurred. */
  type ParseError =
    | {
        readonly domain: "parse";
        readonly issue: "wrong-length";
        readonly expectedLength: number;
        readonly actualLength: number;
      }
    | {
        readonly domain: "parse";
        readonly issue: "invalid-value";
      }
    | {
        readonly domain: "parse";
        readonly issue: "invalid-field";
        readonly field: string;
      };

  type ParseResult<T> = Result<T, ParseError>;

  /** Application-value failure while encoding for the wire. */
  type EncodeError = {
    readonly domain: "encode";
    readonly issue: "invalid-value";
    readonly field?: string;
  };

  type EncodeResult = Result<STBlob, EncodeError>;

  /** Data failure from certifying one complete serialized STObject root. */
  type ObjectParseError =
    | { readonly domain: "parse"; readonly issue: "malformed"; readonly offset: number }
    | { readonly domain: "parse"; readonly issue: "unknown-field"; readonly fieldCode: number; readonly offset: number }
    | { readonly domain: "parse"; readonly issue: "invalid-field"; readonly fieldCode: number; readonly offset: number }
    | { readonly domain: "parse"; readonly issue: "duplicate-field"; readonly fieldCode: number; readonly offset: number }
    | { readonly domain: "parse"; readonly issue: "wrong-terminator"; readonly expected: "object-end" | "array-end" | "root-eof"; readonly actual: number; readonly offset: number }
    | { readonly domain: "parse"; readonly issue: "trailing-bytes"; readonly offset: number }
    | { readonly domain: "parse"; readonly issue: "resource-limit"; readonly limit: "bytes" | "fields" | "scopes" | "depth"; readonly maximum: number; readonly actualAtLeast: number; readonly offset: number };

  /** One-pass serialized-object certification/indexing result. */
  type ObjectParseResult<T> = Result<T, ObjectParseError>;

  /** Failure from constructing or operating on a bounded unsigned integer. */
  interface UIntError {
    readonly domain: "uint";
    readonly issue:
      | "out-of-range"
      | "overflow"
      | "underflow"
      | "division-by-zero";
    readonly bits: UIntWidth;
  }

  type UIntResult<T> = Result<T, UIntError>;

  /**
   * Immutable provider-minted fixed-width unsigned integer. Width interfaces
   * extend this type-only nominal base. The value-level `UIntN` globals are
   * frozen, non-constructible factory/type objects: `instanceof` classifies
   * and narrows; `new` and `.prototype` are not API.
   *
   * JavaScript operators intentionally remain available through the default
   * primitive record projections (`u32be`, `u64be`). Choose this value type
   * when the contract wants its width and overflow policy carried with the
   * value rather than re-established around every arithmetic expression.
   */
  interface UInt<Bits extends UIntWidth = UIntWidth> {
    readonly [__providerValueBrand]: `UInt${Bits}`;
    readonly bits: Bits;
    readonly byteLength: Bits extends 8 ? 1 : Bits extends 16 ? 2 : Bits extends 32 ? 4 : 8;
    toBigInt(): bigint;
    toString(): string;
    /** Conversion is total through 32 bits; UInt64 may exceed JS safe integer. */
    toNumber(): Bits extends 64 ? UIntResult<number> : number;
    isZero(): boolean;
    equals(other: unknown): boolean;
    compare(other: UInt<Bits>): -1 | 0 | 1;
    add(other: UIntInput<Bits>): UIntResult<UInt<Bits>>;
    subtract(other: UIntInput<Bits>): UIntResult<UInt<Bits>>;
    saturatingAdd(other: UInt<Bits>): UInt<Bits>;
    saturatingSubtract(other: UInt<Bits>): UInt<Bits>;
    /**
     * Modular arithmetic at the declared width — C's silent wrap under an
     * honest name. Total by construction; the disposition is the name.
     */
    wrappingAdd(other: UInt<Bits>): UInt<Bits>;
    wrappingSubtract(other: UInt<Bits>): UInt<Bits>;
    wrappingMultiply(other: UInt<Bits>): UInt<Bits>;
  }

  interface UInt8 extends UInt<8> {}

  interface UInt8Factory extends RuntimeType<UInt8> {
    readonly zero: UInt8;
    readonly max: UInt8;
    from(value: bigint | number): UIntResult<UInt8>;
    /**
     * Exact integer floor of (multiplicand x multiplier) / divisor:
     * arbitrary-precision intermediate, no XFL rounding (contrast
     * `UInt64.mulDivXfl`, the C-hook XFL idiom). Fails on a zero
     * divisor, an operand outside the width, or a result that
     * overflows it.
     */
    mulDiv(
      multiplicand: UIntInput<8>,
      multiplier: UIntInput<8>,
      divisor: UIntInput<8>,
    ): UIntResult<UInt8>;
  }

  const UInt8: UInt8Factory;

  interface UInt16 extends UInt<16> {}

  interface UInt16Factory extends RuntimeType<UInt16> {
    readonly zero: UInt16;
    readonly max: UInt16;
    from(value: bigint | number): UIntResult<UInt16>;
    /**
     * Exact integer floor of (multiplicand x multiplier) / divisor:
     * arbitrary-precision intermediate, no XFL rounding (contrast
     * `UInt64.mulDivXfl`, the C-hook XFL idiom). Fails on a zero
     * divisor, an operand outside the width, or a result that
     * overflows it.
     */
    mulDiv(
      multiplicand: UIntInput<16>,
      multiplier: UIntInput<16>,
      divisor: UIntInput<16>,
    ): UIntResult<UInt16>;
  }

  const UInt16: UInt16Factory;

  interface UInt32 extends UInt<32> {}

  interface UInt32Factory extends RuntimeType<UInt32> {
    readonly zero: UInt32;
    readonly max: UInt32;
    from(value: bigint | number): UIntResult<UInt32>;
    /**
     * Exact integer floor of (multiplicand x multiplier) / divisor:
     * arbitrary-precision intermediate, no XFL rounding (contrast
     * `UInt64.mulDivXfl`, the C-hook XFL idiom). Fails on a zero
     * divisor, an operand outside the width, or a result that
     * overflows it.
     */
    mulDiv(
      multiplicand: UIntInput<32>,
      multiplier: UIntInput<32>,
      divisor: UIntInput<32>,
    ): UIntResult<UInt32>;
  }

  const UInt32: UInt32Factory;

  interface UInt64 extends UInt<64> {}

  interface UInt64Factory extends RuntimeType<UInt64> {
    readonly zero: UInt64;
    readonly max: UInt64;
    from(value: bigint | number): UIntResult<UInt64>;
    /**
     * Exact integer floor of (multiplicand x multiplier) / divisor:
     * arbitrary-precision intermediate, no XFL rounding (contrast
     * `UInt64.mulDivXfl`, the C-hook XFL idiom). Fails on a zero
     * divisor, an operand outside the width, or a result that
     * overflows it.
     */
    mulDiv(
      multiplicand: UIntInput<64>,
      multiplier: UIntInput<64>,
      divisor: UIntInput<64>,
    ): UIntResult<UInt64>;
    /**
     * The C-hook XFL mulDiv idiom, exactly: `floor(a * b / c)` computed
     * through XFL. Operands and every intermediate are normalized to 16
     * significant decimal digits (the intermediate is decimal, NOT
     * exact), the quotient is truncated toward zero by `float_int`, and
     * the result domain is 0..9999999999999999 — `float_int`'s maximum,
     * not INT64_MAX. Fails on a zero divisor or a result at or above
     * 1e16. This is the boundary the surviving arithmetic-domain reject
     * flipped (0070:1731): the ported C rolls back exactly where this
     * op fails.
     */
    mulDivXfl(
      multiplicand: UIntInput<64>,
      multiplier: UIntInput<64>,
      divisor: UIntInput<64>,
    ): UIntResult<UInt64>;
  }

  const UInt64: UInt64Factory;

  /**
   * Decoding contract accepted by typed state reads. Sealed: schemas are
   * provider-minted proof objects (`cell`/`record` factories).
   */
  interface BinarySchema<T> {
    readonly [__binarySchemaBrand]: T;
    readonly byteLength: number;
    safeParse(value: BytesLike | STBlob): ParseResult<T>;
  }

  /**
   * Schema-batch values: object key is the wire name, value is a codec.
   * A `RecordField` is a nameless unit codec; the batch supplies the name
   * from the key (also `ParseError.field`). A `BinarySchema` already has a
   * name and is accepted as-is.
   */
  type BatchSchemaField = RecordField<unknown, number> | BinarySchema<unknown>;

  type BatchSchemaValue<F> = F extends BinarySchema<infer U>
    ? U
    : F extends RecordField<infer U, number>
      ? U
      : never;

  /**
   * Per-key outcome of a schema batch: a provider Result, so the whole
   * verb family consumes it directly — `rollback.requirePresent(b.KEY,
   * msg)` demands it, `.ok` narrows it. Missing is a SUCCESSFUL
   * `undefined`, not a failure, so `.okOr(fallback)` replaces only
   * INVALID keys; defaulting a missing key is `.okOr(undefined) ??
   * fallback`. Invalid carries the ParseError; host failure is
   * BATCH-level (one crossing, one host Result).
   */
  type BatchOutcome<T> = Result<T | undefined, ParseError>;

  type BatchOutcomes<T extends Record<string, BatchSchemaField>> = {
    readonly [K in keyof T]: BatchOutcome<BatchSchemaValue<T[K]>>;
  };

  /** Width-known element codec. Offset is not part of the unit; composition assigns it. */
  interface RecordField<T, Width extends number = number> {
    readonly [__recordFieldBrand]: T;
    readonly byteLength: Width;
  }

  /** @serial Blob */
  interface STBlob {
    readonly [__providerValueBrand]: "STBlob";
    readonly byteLength: number;
    byteAt(index: number): number;
    toBytes(): Uint8Array;
    toHex(): HexString;
    /** Whole-value byte equality; values of differing lengths are unequal. */
    equals(other: BytesLike | STBlob): boolean;
  }

  interface STBlobFactory extends RuntimeType<STBlob> {
    from(value: BytesLike): STBlob;
    /** Decode an even-length hexadecimal literal. */
    fromHex(value: HexString): STBlob;
  }

  const STBlob: STBlobFactory;

  /** @inner-rich-type Hash */
  type HashBrandName<Width extends HashWidth> = {
    16: "Hash128";
    20: "Hash160";
    24: "Hash192";
    32: "Hash256";
    48: "Hash384";
    64: "Hash512";
  }[Width];

  interface Hash<Width extends HashWidth = HashWidth> {
    readonly [__providerValueBrand]: HashBrandName<Width>;
    readonly byteLength: Width;
    toBytes(): Uint8Array;
    toHex(): HexString;
    isZero(): boolean;
    equals(other: BytesLike | Hash<Width>): boolean;
    compare(other: Hash<Width>): -1 | 0 | 1;
  }

  /** @serial Hash128 */
  interface Hash128 extends Hash<16> {
    readonly [__providerValueBrand]: "Hash128";
    readonly byteLength: 16;
    toBytes(): Uint8Array;
    toHex(): HexString;
    isZero(): boolean;
    equals(other: Hash128): boolean;
    compare(other: Hash128): -1 | 0 | 1;
  }

  /** @serial Hash160 */
  interface Hash160 extends Hash<20> {
    readonly [__providerValueBrand]: "Hash160";
    readonly byteLength: 20;
    toBytes(): Uint8Array;
    toHex(): HexString;
    isZero(): boolean;
    equals(other: Hash160): boolean;
    compare(other: Hash160): -1 | 0 | 1;
  }

  /** @serial Hash192 */
  interface Hash192 extends Hash<24> {
    readonly [__providerValueBrand]: "Hash192";
    readonly byteLength: 24;
    toBytes(): Uint8Array;
    toHex(): HexString;
    isZero(): boolean;
    equals(other: Hash192): boolean;
    compare(other: Hash192): -1 | 0 | 1;
  }

  /** @serial Hash256 */
  interface Hash256 extends Hash<32> {
    readonly [__providerValueBrand]: "Hash256";
    toHex(): HexString;
    toBytes(): Uint8Array;
    isZero(): boolean;
    equals(other: Hash256): boolean;
  }

  interface Hash256Factory extends RuntimeType<Hash256> {
    readonly zero: Hash256;
    from(value: BytesLike): Hash256;
    /** Decode exactly 32 bytes from an even-length hexadecimal literal. */
    fromHex(value: HexString): Hash256;
  }

  const Hash256: Hash256Factory;

  /**
   * Runtime nouns for provider values without authoring factories
   * (0085 close): the noun classifies; minting stays provider-side
   * until a factory is earned.
   */
  const Hash: RuntimeType<Hash>;

  const Hash128: RuntimeType<Hash128>;

  const Hash160: RuntimeType<Hash160>;

  const Hash192: RuntimeType<Hash192>;

  const UInt: RuntimeType<UInt>;

  const PathHop: RuntimeType<PathHop>;

  const Path: RuntimeType<Path>;

  const PathSet: RuntimeType<PathSet>;

  const Vector256: RuntimeType<Vector256>;

  const XChainBridge: RuntimeType<XChainBridge>;

  const STObject: RuntimeType<STObject>;

  const STArray: RuntimeType<STArray<STObject>>;

  const NativeAmount: RuntimeType<NativeAmount>;

  const IOUAmount: RuntimeType<IOUAmount>;

  const MPTAmount: RuntimeType<MPTAmount>;

  /** @serial AccountID */
  interface AccountID {
    readonly [__providerValueBrand]: "AccountID";
    toHex(): HexString;
    toBytes(): Uint8Array;
    isZero(): boolean;
    equals(other: AccountID): boolean;
  }

  interface AccountIDFactory extends RuntimeType<AccountID> {
    /** XRP's native-issue account: 20 zero bytes. */
    readonly zero: AccountID;
    /** Ripple's no-account sentinel: integer one as a 20-byte AccountID. */
    readonly one: AccountID;
    from(value: BytesLike): AccountID;
    /** Decode exactly 20 bytes from an even-length hexadecimal literal. */
    fromHex(value: HexString): AccountID;
  }

  const AccountID: AccountIDFactory;

  /** @serial Currency */
  interface Currency {
    readonly [__providerValueBrand]: "Currency";
    readonly byteLength: 20;
    readonly isNative: boolean;
    toBytes(): Uint8Array;
    toHex(): HexString;
    toString(): string;
    equals(other: Currency): boolean;
  }

  const Currency: RuntimeType<Currency>;

  /** @serial Issue */
  interface Issue {
    readonly [__providerValueBrand]: "Issue";
    readonly kind: "native" | "iou" | "mpt";
    readonly currency?: Currency;
    readonly issuer?: AccountID;
    readonly mptIssuanceId?: Hash192;
    toBytes(): Uint8Array;
    equals(other: Issue): boolean;
  }

  const Issue: RuntimeType<Issue>;

  /** Immutable provider-minted sequence for the serialized Vector256 wire type. */
  /** @serial Vector256 */
  interface Vector256 extends Iterable<Hash256> {
    readonly [__providerValueBrand]: "Vector256";
    readonly length: number;
    readonly [index: number]: Hash256 | undefined;
    at(index: number): Hash256 | undefined;
    toBytes(): Uint8Array;
    [Symbol.iterator](): IterableIterator<Hash256>;
  }

  /** Immutable provider-minted cross-chain bridge value. */
  /** @serial XChainBridge */
  interface XChainBridge {
    readonly [__providerValueBrand]: "XChainBridge";
    readonly LockingChainDoor: AccountID;
    readonly LockingChainIssue: Issue;
    readonly IssuingChainDoor: AccountID;
    readonly IssuingChainIssue: Issue;
    toBytes(): Uint8Array;
    equals(other: XChainBridge): boolean;
  }

  /**
   * Artifact-declared last-digit rule for profile-sensitive `XFLDecimal`
   * arithmetic. A frozen enum namespace: two literal profile codes, no
   * constructor, no instances.
   *
   * `xahauFloatV1` (code 1) matches live C-hook `float_*` numbers at the
   * pin (Results, not poison). `nearestEvenV1` (code 2) is the 16-digit
   * XFL-domain projection of pinned `ripple::Number`. Code 0 means "no
   * profile declared" and is not a declarable value.
   */
  const XFLProfile: XFLProfileEnum;

  /** The two declarable profile codes: `1 | 2`. */
  type XFLProfile = (typeof XFLProfile)[keyof typeof XFLProfile];

  /**
   * The one-member artifact configuration: which arithmetic profile this
   * Hook's unmarked profile-sensitive operations use. Declared once, in
   * the source entry file, as
   * `export const hookConfig = defineHookConfig({ xflArithmetic: ... })`.
   */
  interface HookConfig {
    readonly xflArithmetic: XFLProfile;
  }

  /**
   * Freeze a Hook's immutable configuration. The compiler requires the
   * exported source-entry `hookConfig` to be a direct call of this
   * function with one exact canonical profile member; the provider
   * validates the evaluated export by mint identity, not shape.
   */
  function defineHookConfig<const C extends HookConfig>(config: C): C;

  /**
   * XFLDecimal — the bounded decimal value used for issued amounts on
   * Xahau: sign × mantissa × 10^exponent, 16 significant digits, exponent
   * −96..80, one canonical zero. Every instance is a valid number. There
   * is no raw word, mantissa, or exponent accessor here; ABI poison is a
   * Result, never an `XFLDecimal`.
   *
   * Profile-sensitive arithmetic is declared-then-bare: it exists only
   * under an explicit source-entry `hookConfig` profile (`XFLProfile`).
   * The value kernel — `sign`, `negate`, `equals`, `compare` — is
   * profile-independent.
   *
   * @see XFLProfile
   * @inner-rich-type XFLDecimal
   */
  interface XFLDecimal {
    readonly [__providerValueBrand]: "XFLDecimal";
    isNegative(): boolean;
    isZero(): boolean;
    sign(): -1 | 0 | 1;
    negate(): XFLDecimal;
    equals(other: unknown): boolean;
    compare(other: XFLDecimal): -1 | 0 | 1;
  }

  const XFLDecimal: RuntimeType<XFLDecimal>;

  /** @serial Amount */
  interface Amount {
    readonly [__providerValueBrand]: "Amount";
    readonly kind: "native" | "iou" | "mpt";
    readonly issue: Issue;
    readonly currency?: Currency;
    readonly issuer?: AccountID;
    readonly mptIssuanceId?: Hash192;
    readonly value?: XFLDecimal | bigint;
    readonly drops?: Drops;
    readonly byteLength: 8 | 33 | 48;
    toBytes(): Uint8Array;
    toXFL(): XFLDecimal;
    toString(): string;
    isNative(): this is NativeAmount;
    isIOU(): this is IOUAmount;
    isMPT(): this is MPTAmount;
    /**
     * Value-narrowing companions of `isNative` / `isIOU` / `isMPT`.
     * `rollback.requirePresent(amt.asNative(), msg)` unwraps; `isNative()`
     * as a boolean cannot restore the discriminant after the call.
     */
    asNative(): NativeAmount | undefined;
    asIOU(): IOUAmount | undefined;
    asMPT(): MPTAmount | undefined;
    equals(other: Amount): boolean;
    compare(other: Amount): -1 | 0 | 1;
  }

  const Amount: RuntimeType<Amount>;

  interface NativeAmount extends Amount {
    readonly kind: "native";
    readonly drops: Drops;
    readonly currency: undefined;
    readonly issuer: undefined;
    readonly mptIssuanceId: undefined;
    readonly value: undefined;
  }

  interface IOUAmount extends Amount {
    readonly kind: "iou";
    readonly drops: undefined;
    readonly currency: Currency;
    readonly issuer: AccountID;
    readonly mptIssuanceId: undefined;
    readonly value: XFLDecimal;
  }

  interface MPTAmount extends Amount {
    readonly kind: "mpt";
    readonly drops: undefined;
    readonly currency: undefined;
    readonly issuer: undefined;
    readonly mptIssuanceId: Hash192;
    readonly value: bigint;
  }

  interface PathHop {
    readonly [__providerValueBrand]: "PathHop";
    readonly account?: AccountID;
    readonly currency?: Currency;
    readonly issuer?: AccountID;
  }

  interface Path extends Iterable<PathHop> {
    readonly [__providerValueBrand]: "Path";
    readonly length: number;
    at(index: number): PathHop | undefined;
    [Symbol.iterator](): IterableIterator<PathHop>;
  }

  /** @serial PathSet */
  interface PathSet extends Iterable<Path> {
    readonly [__providerValueBrand]: "PathSet";
    readonly length: number;
    at(index: number): Path | undefined;
    toBytes(): Uint8Array;
    [Symbol.iterator](): IterableIterator<Path>;
  }

  /**
   * A protocol field descriptor derived from Xahau definitions.
   *
   * `T` is type-only; the runtime value contains the three numeric codes. The
   * literal code parameters let drift checkers retain exact source equality
   * while `T` lets generic object access infer the decoded value. `Code` is
   * redundant by design: it equals `(TypeCode << 16) | FieldCode`, but
   * TypeScript cannot express that numeric-literal arithmetic directly. The
   * declaration checker enforces the relationship instead.
   */
  interface SerializedField<
    T,
    Code extends number = number,
    TypeCode extends number = number,
    FieldCode extends number = number,
  > {
    readonly [__serializedFieldBrand]: T;
    readonly code: Code;
    readonly typeCode: TypeCode;
    readonly fieldCode: FieldCode;
  }

  /** Field-descriptor runtime noun (0085 close). */
  const SerializedField: RuntimeType<
    SerializedField<unknown, number, number, number>
  >;

  type SerializedFieldValue<T> =
    T extends SerializedField<infer V> ? unknown extends V ? never : V : never;

  /** Values derived mechanically from the protocol `Field` descriptor table. */
  type ProtocolFieldValue = {
    [K in keyof typeof Field]: SerializedFieldValue<(typeof Field)[K]>;
  }[keyof typeof Field];

  /** A decoded protocol field value, plus absence for an unset field. */
  type SerializedValue = ProtocolFieldValue | undefined;

  /**
   * Immutable decoded-object view.
   *
   * `withField` and `withoutField` return a new logical value and never mutate
   * this object. Implementations may structurally share backing bytes, decoded
   * values, and patch overlays so long as that sharing is unobservable.
   *
   * @serial STObject LedgerEntry
   * @inner-rich-type STObject
   */
  interface STObject {
    readonly [__stObjectBrand]: void;
    has(field: string | SerializedField<unknown>): boolean;
    get<T>(field: SerializedField<T>): T | undefined;
    get(field: string): SerializedValue;
    /**
     * Canonical field VALUE payload as an STBlob: excludes the field
     * header and any VL length prefix; includes a nested object/array
     * close marker; `undefined` for absent or unknown fields. This is
     * not a source-image or whole-field-framing API (0093 pinned
     * contract; native fixtures are the arbiter at the F0 join).
     */
    fieldBytes(field: string | number | SerializedField<unknown>): STBlob | undefined;
    withField<T>(field: SerializedField<T>, value: T | Uint8Array | ArrayBuffer): STObject;
    withField(field: string | number, value: Exclude<SerializedValue, undefined> | Uint8Array | ArrayBuffer): STObject;
    withoutField(field: string | number | SerializedField<unknown>): STObject;
    toBytes(): Uint8Array;
    toJSON(): unknown;
  }

  /**
   * Minted array of certified objects. `length` is known from the certification
   * index. Numeric access materializes an element on demand; an out-of-range
   * index produces `undefined`. Dense; iterable in index order.
   *
   * @serial STArray
   * @inner-rich-type STArray
   */
  interface STArray<T extends STObject = STObject> extends Iterable<T> {
    readonly [__stArrayBrand]: T;
    readonly length: number;
    readonly [index: number]: T | undefined;
    at(index: number): T | undefined;
    toJSON(): unknown;
    [Symbol.iterator](): IterableIterator<T>;
  }

  const enum TransactionType {
    Payment = 0,
    Invoke = 99,
    GenesisMint = 96,
    Import = 97,
    ClaimReward = 98,
    SetHook = 22,
    TrustSet = 20,
    Remit = 95,
    NFTokenBurn = 26,
    URITokenMint = 45,
    UNLReport = 104,
    EmitFailure = 103,
    UNLModify = 102,
    SetFee = 101,
    EnableAmendment = 100,
    SetRemarks = 94,
    CronSet = 93,
    Cron = 92,
    PermissionedDomainDelete = 72,
    PermissionedDomainSet = 71,
    NFTokenModify = 70,
    CredentialDelete = 69,
    CredentialAccept = 68,
    CredentialCreate = 67,
    MPTokenAuthorize = 66,
    MPTokenIssuanceSet = 65,
    MPTokenIssuanceDestroy = 64,
    MPTokenIssuanceCreate = 63,
    LedgerStateFix = 62,
    OracleDelete = 61,
    OracleSet = 60,
    DIDDelete = 59,
    DIDSet = 58,
    XChainCreateBridge = 57,
    XChainModifyBridge = 56,
    XChainAddAccountCreateAttestation = 55,
    XChainAddClaimAttestation = 54,
    XChainAccountCreateCommit = 53,
    XChainClaim = 52,
    XChainCommit = 51,
    XChainCreateClaimID = 50,
    URITokenCancelSellOffer = 49,
    URITokenCreateSellOffer = 48,
    URITokenBuy = 47,
    URITokenBurn = 46,
    AMMDelete = 40,
    AMMBid = 39,
    AMMVote = 38,
    AMMWithdraw = 37,
    AMMDeposit = 36,
    AMMCreate = 35,
    AMMClawback = 31,
    Clawback = 30,
    NFTokenAcceptOffer = 29,
    NFTokenCancelOffer = 28,
    NFTokenCreateOffer = 27,
    NFTokenMint = 25,
    AccountDelete = 21,
    DepositPreauth = 19,
    CheckCancel = 18,
    CheckCash = 17,
    CheckCreate = 16,
    PaymentChannelClaim = 15,
    PaymentChannelFund = 14,
    PaymentChannelCreate = 13,
    SignerListSet = 12,
    SpinalTap = 11,
    TicketCreate = 10,
    Contract = 9,
    OfferCancel = 8,
    OfferCreate = 7,
    NicknameSet = 6,
    SetRegularKey = 5,
    EscrowCancel = 4,
    AccountSet = 3,
    EscrowFinish = 2,
    EscrowCreate = 1,
  }

  const enum TransactionResult {
    tesSUCCESS = 0,
    tecLAST_POSSIBLE_ENTRY = 255,
    tecBAD_CREDENTIALS = 199,
    tecLOCKED = 198,
    tecARRAY_TOO_LARGE = 197,
    tecARRAY_EMPTY = 196,
    tecTOKEN_PAIR_NOT_FOUND = 195,
    tecINVALID_UPDATE_TIME = 194,
    tecEMPTY_DID = 193,
    tecXCHAIN_BAD_PUBLIC_KEY_ACCOUNT_PAIR = 192,
    tecINCOMPLETE = 191,
    tecHAS_HOOK_STATE = 190,
    tecTOO_MANY_REMARKS = 189,
    tecIMMUTABLE = 188,
    tecINSUF_RESERVE_SELLER = 187,
    tecXCHAIN_CREATE_ACCOUNT_DISABLED = 186,
    tecXCHAIN_SELF_COMMIT = 185,
    tecXCHAIN_PAYMENT_FAILED = 184,
    tecXCHAIN_ACCOUNT_CREATE_TOO_MANY = 183,
    tecXCHAIN_ACCOUNT_CREATE_PAST = 182,
    tecXCHAIN_INSUFF_CREATE_AMOUNT = 181,
    tecXCHAIN_SENDING_ACCOUNT_MISMATCH = 180,
    tecXCHAIN_NO_SIGNERS_LIST = 179,
    tecXCHAIN_REWARD_MISMATCH = 178,
    tecXCHAIN_WRONG_CHAIN = 177,
    tecXCHAIN_CREATE_ACCOUNT_NONXRP_ISSUE = 176,
    tecXCHAIN_PROOF_UNKNOWN_KEY = 175,
    tecXCHAIN_CLAIM_NO_QUORUM = 174,
    tecXCHAIN_BAD_CLAIM_ID = 173,
    tecXCHAIN_NO_CLAIM_ID = 172,
    tecXCHAIN_BAD_TRANSFER_ISSUE = 171,
    tecPRECISION_LOSS = 170,
    tecREQUIRES_FLAG = 169,
    tecAMM_ACCOUNT = 168,
    tecAMM_NOT_EMPTY = 167,
    tecAMM_EMPTY = 166,
    tecAMM_INVALID_TOKENS = 165,
    tecAMM_FAILED = 164,
    tecAMM_BALANCE = 163,
    tecUNFUNDED_AMM = 162,
    tecINSUFFICIENT_PAYMENT = 161,
    tecOBJECT_NOT_FOUND = 160,
    tecINSUFFICIENT_FUNDS = 159,
    tecCANT_ACCEPT_OWN_NFTOKEN_OFFER = 158,
    tecNFTOKEN_OFFER_TYPE_MISMATCH = 157,
    tecNFTOKEN_BUY_SELL_MISMATCH = 156,
    tecNO_SUITABLE_NFTOKEN_PAGE = 155,
    tecMAX_SEQUENCE_REACHED = 154,
    tecHOOK_REJECTED = 153,
    tecTOO_SOON = 152,
    tecHAS_OBLIGATIONS = 151,
    tecKILLED = 150,
    tecDUPLICATE = 149,
    tecEXPIRED = 148,
    tecINVARIANT_FAILED = 147,
    tecCRYPTOCONDITION_ERROR = 146,
    tecOVERSIZE = 145,
    tecINTERNAL = 144,
    tecDST_TAG_NEEDED = 143,
    tecNEED_MASTER_KEY = 142,
    tecINSUFFICIENT_RESERVE = 141,
    tecNO_ENTRY = 140,
    tecNO_PERMISSION = 139,
    tecNO_TARGET = 138,
    tecFROZEN = 137,
    tecINSUFF_FEE = 136,
    tecNO_LINE = 135,
    tecNO_AUTH = 134,
    tecNO_ISSUER = 133,
    tecOWNERS = 132,
    tecNO_REGULAR_KEY = 131,
    tecNO_ALTERNATIVE_KEY = 130,
    tecUNFUNDED = 129,
    tecPATH_DRY = 128,
    tecNO_LINE_REDUNDANT = 127,
    tecNO_LINE_INSUF_RESERVE = 126,
    tecNO_DST_INSUF_NATIVE = 125,
    tecNO_DST = 124,
    tecINSUF_RESERVE_OFFER = 123,
    tecINSUF_RESERVE_LINE = 122,
    tecDIR_FULL = 121,
    tecFAILED_PROCESSING = 105,
    tecUNFUNDED_PAYMENT = 104,
    tecUNFUNDED_OFFER = 103,
    tecUNFUNDED_ADD = 102,
    tecPATH_PARTIAL = 101,
    tecCLAIM = 100,
    tesPARTIAL = 1,
    terNO_HOOK = -86,
    terNO_AMM = -87,
    terPRE_TICKET = -88,
    terQUEUED = -89,
    terNO_RIPPLE = -90,
    terLAST = -91,
    terPRE_SEQ = -92,
    terOWNERS = -93,
    terNO_LINE = -94,
    terNO_AUTH = -95,
    terNO_ACCOUNT = -96,
    terINSUF_FEE_B = -97,
    terFUNDS_SPENT = -98,
    terRETRY = -99,
    tefINVALID_LEDGER_FIX_TYPE = -174,
    tefIMPORT_BLACKHOLED = -175,
    tefNONDIR_EMIT = -176,
    tefPAST_IMPORT_VL_SEQ = -177,
    tefPAST_IMPORT_SEQ = -178,
    tefNFTOKEN_IS_NOT_TRANSFERABLE = -179,
    tefNO_TICKET = -180,
    tefTOO_BIG = -181,
    tefINVARIANT_FAILED = -182,
    tefBAD_AUTH_MASTER = -183,
    tefNOT_MULTI_SIGNING = -184,
    tefBAD_QUORUM = -185,
    tefBAD_SIGNATURE = -186,
    tefMAX_LEDGER = -187,
    tefMASTER_DISABLED = -188,
    tefWRONG_PRIOR = -189,
    tefPAST_SEQ = -190,
    tefNO_AUTH_REQUIRED = -191,
    tefINTERNAL = -192,
    tefEXCEPTION = -193,
    tefCREATED = -194,
    tefBAD_LEDGER = -195,
    tefBAD_AUTH = -196,
    tefBAD_ADD_AUTH = -197,
    tefALREADY = -198,
    tefFAILURE = -199,
    temBAD_TRANSFER_FEE = -249,
    temARRAY_TOO_LARGE = -250,
    temARRAY_EMPTY = -251,
    temEMPTY_DID = -252,
    temHOOK_DATA_TOO_LARGE = -253,
    temXCHAIN_TOO_MANY_ATTESTATIONS = -254,
    temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT = -255,
    temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT = -256,
    temXCHAIN_BRIDGE_NONDOOR_OWNER = -257,
    temXCHAIN_BRIDGE_BAD_ISSUES = -258,
    temXCHAIN_BAD_PROOF = -259,
    temXCHAIN_EQUAL_DOOR_ACCOUNTS = -260,
    temBAD_AMM_TOKENS = -261,
    temBAD_NFTOKEN_TRANSFER_FEE = -262,
    temSEQ_AND_TICKET = -263,
    temUNKNOWN = -264,
    temUNCERTAIN = -265,
    temINVALID_COUNT = -266,
    temCANNOT_PREAUTH_SELF = -267,
    temINVALID_ACCOUNT_ID = -268,
    temBAD_TICK_SIZE = -269,
    temBAD_WEIGHT = -270,
    temBAD_QUORUM = -271,
    temBAD_SIGNER = -272,
    temDISABLED = -273,
    temRIPPLE_EMPTY = -274,
    temREDUNDANT = -275,
    temINVALID_FLAG = -276,
    temINVALID = -277,
    temDST_NEEDED = -278,
    temDST_IS_SRC = -279,
    temBAD_TRANSFER_RATE = -280,
    temBAD_SRC_ACCOUNT = -281,
    temBAD_SIGNATURE = -282,
    temBAD_SEQUENCE = -283,
    temBAD_SEND_NATIVE_PATHS = -284,
    temBAD_SEND_NATIVE_PARTIAL = -285,
    temBAD_SEND_NATIVE_NO_DIRECT = -286,
    temBAD_SEND_NATIVE_MAX = -287,
    temBAD_SEND_NATIVE_LIMIT = -288,
    temBAD_REGKEY = -289,
    temBAD_PATH_LOOP = -290,
    temBAD_PATH = -291,
    temBAD_OFFER = -292,
    temBAD_LIMIT = -293,
    temBAD_ISSUER = -294,
    temBAD_FEE = -295,
    temBAD_EXPIRATION = -296,
    temBAD_CURRENCY = -297,
    temBAD_AMOUNT = -298,
    temMALFORMED = -299,
    telENV_RPC_FAILED = -380,
    telCAN_NOT_QUEUE_IMPORT = -381,
    telIMPORT_VL_KEY_NOT_RECOGNISED = -382,
    telNON_LOCAL_EMITTED_TXN = -383,
    telNETWORK_ID_MAKES_TX_NON_CANONICAL = -384,
    telREQUIRES_NETWORK_ID = -385,
    telWRONG_NETWORK = -386,
    telCAN_NOT_QUEUE_FULL = -387,
    telCAN_NOT_QUEUE_FEE = -388,
    telCAN_NOT_QUEUE_BLOCKED = -389,
    telCAN_NOT_QUEUE_BLOCKS = -390,
    telCAN_NOT_QUEUE_BALANCE = -391,
    telCAN_NOT_QUEUE = -392,
    telNO_DST_PARTIAL = -393,
    telINSUF_FEE_P = -394,
    telFAILED_PROCESSING = -395,
    telBAD_PUBLIC_KEY = -396,
    telBAD_PATH_COUNT = -397,
    telBAD_DOMAIN = -398,
    telLOCAL_ERROR = -399,
  }

  /** Information supplied to an emitted-transaction callback entry point. */
  interface CallbackInfo {
    /** Exact whole-word applied predicate: `rawFlags === 0`. */
    readonly applied: boolean;
    /**
     * Exact bit-zero observation of the callback word. Not the inverse of
     * `applied`: a non-zero word with bit zero clear is neither applied nor
     * flagged here.
     */
    readonly failureBitSet: boolean;
    /**
     * Exact uint32 callback word supplied by Xahau. Prefer named properties;
     * this is retained for diagnostics and forward-compatible expert use.
     */
    readonly rawFlags: number;
  }

  namespace otxn {
    function type(): HostResult<TransactionType>;
  }

  namespace state {
    function get(key: string | BytesLike | STBlob | Hash256 | AccountID): HostResult<STBlob | undefined>;
    function set(
      key: string | BytesLike | STBlob | Hash256 | AccountID,
      value: string | BytesLike | STBlob | Hash256 | AccountID,
    ): HostVoidResult;
  }

  namespace emit {
    function reserve(count: number): HostVoidResult;
    function tx(transaction: BytesLike | STBlob): HostResult<Hash256>;
    function prepare(partial: BytesLike | STBlob): HostResult<STBlob>;
  }

  namespace util {
    /**
     * Decode ledger-serialized bytes. Assertion form: malformed input
     * throws TypeError. Gate untrusted bytes with `util.validateObject`
     * first — the total predicate exists exactly so decode can assert.
     */
    function decodeObject(value: ObjectBytes): STObject;
    /**
     * Certify and mint in one pass. Data/cap failures are returned; contract
     * violations and runtime OOM throw.
     */
    function safeDecodeObject(value: ObjectBytes): ObjectParseResult<STObject>;
    /**
     * Returns false for object data/cap failure. Contract violations and
     * runtime OOM may throw.
     */
    function validateObject(value: ObjectBytes): boolean;
  }

  namespace ledger {
    const sequence: LedgerSequence;
    const lastTime: RippleTime;
    const lastHash: Hash256;
  }

  /** Metadata and configuration for the currently executing Hook. */
  namespace hook {
    /** Hook account for this invocation; provider construction is total. */
    function account(): AccountID;
  }

  /**
   * Accept and terminate this hook execution. The host records the terminal
   * result and unwinds the current Wasm invocation; no JavaScript statement
   * after this call can run, hence the `never` return type.
   *
   * C Hooks have the same observable behavior even though their import ABI is
   * declared as returning `int64_t`: Xahaud turns `RC_ACCEPT` into engine
   * termination before the import returns to guest code. C source therefore
   * uses both `accept(...)` and the redundant `return accept(...)` spelling.
   * TypeScript should normally use the direct call.
   *
   * A supplied `code` becomes the integer HookReturnCode recorded on-ledger.
   * Omit it to let the compiler/provider record source location. Do not copy
   * C `__LINE__`. Explicitly meaningful codes remain caller-owned.
   */
  function accept(message?: string | BytesLike | STBlob, code?: number): never;

  namespace accept {
    /**
     * Continue only when this Result succeeded with a present (non-nullish)
     * value; otherwise `accept` — this invocation is done, nothing to act
     * on. Falsy-but-present successes (`0`, `0n`, `false`, and `""`) are
     * values and continue.
     * There is no `accept.require`; that name is a compile error on purpose.
     */
    function unlessPresent<T, Error>(
      result: Result<T, Error>,
      message?: string | BytesLike | STBlob,
      code?: number,
    ): Present<T>;
    /**
     * Continue only when this direct value is present (non-nullish);
     * otherwise `accept` — the named form of `value ?? accept(...)`.
     */
    function unlessPresent<T>(
      value: T,
      message?: string | BytesLike | STBlob,
      code?: number,
    ): Present<T>;
    /**
     * Continue only when this Result succeeded with an ordinarily truthy
     * value; failure and any falsy success accept.
     */
    function unlessTruthy<T, Error>(
      result: Result<T, Error>,
      message?: string | BytesLike | STBlob,
      code?: number,
    ): JSTruthy<T>;
    /**
     * Continue only when this direct value is truthy; otherwise `accept`.
     * `false`, `0`, `0n`, `""`, `null`, `undefined`, and `NaN` accept.
     */
    function unlessTruthy<T>(
      value: T,
      message?: string | BytesLike | STBlob,
      code?: number,
    ): JSTruthy<T>;
    /**
     * If `condition` is true, `accept`. If false, return and continue.
     */
    function when(
      condition: boolean,
      message?: string | BytesLike | STBlob,
      code?: number,
    ): void;
  }

  /**
   * Reject, atomically roll back, and terminate this hook execution. Like
   * `accept`, the host unwinds the Wasm invocation and this call never returns.
   * Its `never` return is intentionally usable in expression positions such as
   * `value ?? rollback("value is required")`.
   */
  function rollback(message?: string | BytesLike | STBlob, code?: number): never;

  namespace rollback {
    /**
     * Return a successful host value, including a legitimate `undefined`, or
     * atomically roll back with the exact failed host status as the terminal
     * code. Supply `message` when the surrounding contract gives that failure
     * more useful context than the default host-status diagnostic.
     */
    function onFail<T>(
      result: HostResult<T>,
      message?: string | BytesLike | STBlob,
    ): T;
    /**
     * Effect form: roll back with the exact failed host status; a
     * successful effect returns nothing.
     */
    function onFail(
      result: HostVoidResult,
      message?: string | BytesLike | STBlob,
    ): void;
    /**
     * Apply a contract-owned terminal policy to a result whose failure does not
     * already carry a Hook status.
     */
    function onFail<T, Error>(
      result: Result<T, Error>,
      message: string | BytesLike | STBlob,
      code?: number,
    ): T;
    /** Effect form with a contract-owned policy for uncoded domains. */
    function onFail<Error>(
      result: VoidResult<Error>,
      message: string | BytesLike | STBlob,
      code?: number,
    ): void;
    /**
     * Require a present (non-nullish) value from a Result. Failure and a
     * nullish success both apply the contract-owned rollback policy; falsy
     * successes (`0`, `0n`, `false`, and `""`) are values and return.
     */
    function requirePresent<T, Error>(
      result: Result<T, Error>,
      message: string | BytesLike | STBlob,
      code?: number,
    ): Present<T>;
    /**
     * Require a present (non-nullish) direct value — the named form of
     * `value ?? rollback(...)`. `0`, `0n`, `false`, and `""` are values
     * and return.
     */
    function requirePresent<T>(
      value: T,
      message: string | BytesLike | STBlob,
      code?: number,
    ): Present<T>;
    /**
     * Require a Result to have succeeded with an ordinarily truthy value.
     * Failure and any falsy success apply the rollback policy.
     */
    function requireTruthy<T, Error>(
      result: Result<T, Error>,
      message: string | BytesLike | STBlob,
      code?: number,
    ): JSTruthy<T>;
    /**
     * Require an ordinarily truthy direct value. `false`, `0`, `0n`, `""`,
     * `null`, `undefined`, and `NaN` apply the rollback policy.
     */
    function requireTruthy<T>(
      value: T,
      message: string | BytesLike | STBlob,
      code?: number,
    ): JSTruthy<T>;
    /**
     * If `condition` is true, `rollback`. If false, return and continue.
     */
    function when(
      condition: boolean,
      message?: string | BytesLike | STBlob,
      code?: number,
    ): void;
    /**
     * Return every value when every host operation succeeded. If any failed,
     * roll back with the first failure's host code, in input order. JavaScript
     * evaluates every array element before this helper runs; use sequential
     * checks when later host calls must be conditional on earlier success.
     */
    function onAnyFail<T>(
      results: readonly HostResult<T>[],
      message?: string | BytesLike | STBlob,
    ): readonly T[];
    /** Return one `undefined` slot per successful host effect. */
    function onAnyFail(
      results: readonly HostVoidResult[],
      message?: string | BytesLike | STBlob,
    ): readonly undefined[];
    /** Domain effect form with a contract-owned policy. */
    function onAnyFail<Error>(
      results: readonly VoidResult<Error>[],
      message: string | BytesLike | STBlob,
      code: number,
    ): readonly undefined[];
    /** Apply a contract-owned rollback policy when any domain result failed. */
    function onAnyFail<T, Error>(
      results: readonly Result<T, Error>[],
      message: string | BytesLike | STBlob,
      code: number,
    ): readonly T[];
    /**
     * Return the successful values in input order when at least one operation
     * succeeded. If all operations failed, apply the supplied contract-owned
     * rollback policy. This accepts `ParseResult` and other result domains as
     * well as host results. An empty input therefore rolls back. All array
     * elements are evaluated before this helper inspects their Results.
     */
    function onAllFail<T, Error>(
      results: readonly Result<T, Error>[],
      message: string | BytesLike | STBlob,
      code: number,
    ): readonly T[];
    /**
     * Effect form: one `undefined` slot per SUCCESSFUL effect (failures
     * are skipped, not slotted); rolls back only when every one failed.
     */
    function onAllFail<Error>(
      results: readonly VoidResult<Error>[],
      message: string | BytesLike | STBlob,
      code: number,
    ): readonly undefined[];
  }

  /**
   * Trace a diagnostic value. The provider renders scalar values and emits
   * byte-bearing values as hexadecimal without requiring encoding-specific
   * contract calls.
   */
  function trace(label: string, value?: unknown): void;

  /** Negative values returned by host functions on failure. */
  const enum HookReturnCode {
    SUCCESS = 0,
    OUT_OF_BOUNDS = -1,
    INTERNAL_ERROR = -2,
    TOO_BIG = -3,
    TOO_SMALL = -4,
    DOESNT_EXIST = -5,
    NO_FREE_SLOTS = -6,
    INVALID_ARGUMENT = -7,
    ALREADY_SET = -8,
    PREREQUISITE_NOT_MET = -9,
    FEE_TOO_LARGE = -10,
    EMISSION_FAILURE = -11,
    TOO_MANY_NONCES = -12,
    TOO_MANY_EMITTED_TXN = -13,
    NOT_IMPLEMENTED = -14,
    INVALID_ACCOUNT = -15,
    GUARD_VIOLATION = -16,
    INVALID_FIELD = -17,
    PARSE_ERROR = -18,
    RC_ROLLBACK = -19,
    RC_ACCEPT = -20,
    NO_SUCH_KEYLET = -21,
    NOT_AN_ARRAY = -22,
    NOT_AN_OBJECT = -23,
    DIVISION_BY_ZERO = -25,
    MANTISSA_OVERSIZED = -26,
    MANTISSA_UNDERSIZED = -27,
    EXPONENT_OVERSIZED = -28,
    EXPONENT_UNDERSIZED = -29,
    XFL_OVERFLOW = -30,
    NOT_IOU_AMOUNT = -31,
    NOT_AN_AMOUNT = -32,
    CANT_RETURN_NEGATIVE = -33,
    NOT_AUTHORIZED = -34,
    PREVIOUS_FAILURE_PREVENTS_RETRY = -35,
    TOO_MANY_PARAMS = -36,
    INVALID_TXN = -37,
    RESERVE_INSUFFICIENT = -38,
    COMPLEX_NOT_SUPPORTED = -39,
    DOES_NOT_MATCH = -40,
    INVALID_KEY = -41,
    NOT_A_STRING = -42,
    MEM_OVERLAP = -43,
    TOO_MANY_STATE_MODIFICATIONS = -44,
    TOO_MANY_NAMESPACES = -45,
    INVALID_FLOAT = -10024,
  }

  /**
   * Every serialized protocol field, by name.
   *
   * `code` is what a host function expects: (typeCode << 16) | fieldCode,
   * the same encoding as sfAccount in the C headers. Plain data, not a
   * rich type — a contract that needs to name a field it has no typed
   * accessor for can reach for this.
   */
  const Field: {
    readonly LedgerEntryType: SerializedField<UInt16, 65537, 1, 1>;
    readonly TransactionType: SerializedField<TransactionType, 65538, 1, 2>;
    readonly SignerWeight: SerializedField<UInt16, 65539, 1, 3>;
    readonly TransferFee: SerializedField<UInt16, 65540, 1, 4>;
    readonly Version: SerializedField<UInt16, 65552, 1, 16>;
    readonly HookStateChangeCount: SerializedField<UInt16, 65553, 1, 17>;
    readonly HookEmitCount: SerializedField<UInt16, 65554, 1, 18>;
    readonly HookExecutionIndex: SerializedField<UInt16, 65555, 1, 19>;
    readonly HookApiVersion: SerializedField<UInt16, 65556, 1, 20>;
    readonly HookStateScale: SerializedField<UInt16, 65557, 1, 21>;
    readonly NetworkID: SerializedField<UInt32, 131073, 2, 1>;
    readonly Flags: SerializedField<UInt32, 131074, 2, 2>;
    readonly SourceTag: SerializedField<UInt32, 131075, 2, 3>;
    readonly Sequence: SerializedField<UInt32, 131076, 2, 4>;
    readonly PreviousTxnLgrSeq: SerializedField<UInt32, 131077, 2, 5>;
    readonly LedgerSequence: SerializedField<UInt32, 131078, 2, 6>;
    readonly CloseTime: SerializedField<UInt32, 131079, 2, 7>;
    readonly ParentCloseTime: SerializedField<UInt32, 131080, 2, 8>;
    readonly SigningTime: SerializedField<UInt32, 131081, 2, 9>;
    readonly Expiration: SerializedField<UInt32, 131082, 2, 10>;
    readonly TransferRate: SerializedField<UInt32, 131083, 2, 11>;
    readonly WalletSize: SerializedField<UInt32, 131084, 2, 12>;
    readonly OwnerCount: SerializedField<UInt32, 131085, 2, 13>;
    readonly DestinationTag: SerializedField<UInt32, 131086, 2, 14>;
    readonly HighQualityIn: SerializedField<UInt32, 131088, 2, 16>;
    readonly HighQualityOut: SerializedField<UInt32, 131089, 2, 17>;
    readonly LowQualityIn: SerializedField<UInt32, 131090, 2, 18>;
    readonly LowQualityOut: SerializedField<UInt32, 131091, 2, 19>;
    readonly QualityIn: SerializedField<UInt32, 131092, 2, 20>;
    readonly QualityOut: SerializedField<UInt32, 131093, 2, 21>;
    readonly StampEscrow: SerializedField<UInt32, 131094, 2, 22>;
    readonly BondAmount: SerializedField<UInt32, 131095, 2, 23>;
    readonly LoadFee: SerializedField<UInt32, 131096, 2, 24>;
    readonly OfferSequence: SerializedField<UInt32, 131097, 2, 25>;
    readonly FirstLedgerSequence: SerializedField<UInt32, 131098, 2, 26>;
    readonly LastLedgerSequence: SerializedField<UInt32, 131099, 2, 27>;
    readonly TransactionIndex: SerializedField<UInt32, 131100, 2, 28>;
    readonly OperationLimit: SerializedField<UInt32, 131101, 2, 29>;
    readonly ReferenceFeeUnits: SerializedField<UInt32, 131102, 2, 30>;
    readonly ReserveBase: SerializedField<UInt32, 131103, 2, 31>;
    readonly ReserveIncrement: SerializedField<UInt32, 131104, 2, 32>;
    readonly SetFlag: SerializedField<UInt32, 131105, 2, 33>;
    readonly ClearFlag: SerializedField<UInt32, 131106, 2, 34>;
    readonly SignerQuorum: SerializedField<UInt32, 131107, 2, 35>;
    readonly CancelAfter: SerializedField<UInt32, 131108, 2, 36>;
    readonly FinishAfter: SerializedField<UInt32, 131109, 2, 37>;
    readonly SignerListID: SerializedField<UInt32, 131110, 2, 38>;
    readonly SettleDelay: SerializedField<UInt32, 131111, 2, 39>;
    readonly TicketCount: SerializedField<UInt32, 131112, 2, 40>;
    readonly TicketSequence: SerializedField<UInt32, 131113, 2, 41>;
    readonly NFTokenTaxon: SerializedField<UInt32, 131114, 2, 42>;
    readonly MintedNFTokens: SerializedField<UInt32, 131115, 2, 43>;
    readonly BurnedNFTokens: SerializedField<UInt32, 131116, 2, 44>;
    readonly HookStateCount: SerializedField<UInt32, 131117, 2, 45>;
    readonly EmitGeneration: SerializedField<UInt32, 131118, 2, 46>;
    readonly LockCount: SerializedField<UInt32, 131121, 2, 49>;
    readonly FirstNFTokenSequence: SerializedField<UInt32, 131122, 2, 50>;
    readonly StartTime: SerializedField<UInt32, 131165, 2, 93>;
    readonly RepeatCount: SerializedField<UInt32, 131166, 2, 94>;
    readonly DelaySeconds: SerializedField<UInt32, 131167, 2, 95>;
    readonly XahauActivationLgrSeq: SerializedField<UInt32, 131168, 2, 96>;
    readonly ImportSequence: SerializedField<UInt32, 131169, 2, 97>;
    readonly RewardTime: SerializedField<UInt32, 131170, 2, 98>;
    readonly RewardLgrFirst: SerializedField<UInt32, 131171, 2, 99>;
    readonly RewardLgrLast: SerializedField<UInt32, 131172, 2, 100>;
    readonly IndexNext: SerializedField<UInt64, 196609, 3, 1>;
    readonly IndexPrevious: SerializedField<UInt64, 196610, 3, 2>;
    readonly BookNode: SerializedField<UInt64, 196611, 3, 3>;
    readonly OwnerNode: SerializedField<UInt64, 196612, 3, 4>;
    readonly BaseFee: SerializedField<UInt64, 196613, 3, 5>;
    readonly ExchangeRate: SerializedField<UInt64, 196614, 3, 6>;
    readonly LowNode: SerializedField<UInt64, 196615, 3, 7>;
    readonly HighNode: SerializedField<UInt64, 196616, 3, 8>;
    readonly DestinationNode: SerializedField<UInt64, 196617, 3, 9>;
    readonly Cookie: SerializedField<UInt64, 196618, 3, 10>;
    readonly ServerVersion: SerializedField<UInt64, 196619, 3, 11>;
    readonly NFTokenOfferNode: SerializedField<UInt64, 196620, 3, 12>;
    readonly EmitBurden: SerializedField<UInt64, 196621, 3, 13>;
    readonly HookInstructionCount: SerializedField<UInt64, 196625, 3, 17>;
    readonly HookReturnCode: SerializedField<UInt64, 196626, 3, 18>;
    readonly ReferenceCount: SerializedField<UInt64, 196627, 3, 19>;
    readonly TouchCount: SerializedField<UInt64, 196705, 3, 97>;
    readonly AccountIndex: SerializedField<UInt64, 196706, 3, 98>;
    readonly AccountCount: SerializedField<UInt64, 196707, 3, 99>;
    readonly RewardAccumulator: SerializedField<UInt64, 196708, 3, 100>;
    readonly EmailHash: SerializedField<Hash128, 262145, 4, 1>;
    readonly LedgerHash: SerializedField<Hash256, 327681, 5, 1>;
    readonly ParentHash: SerializedField<Hash256, 327682, 5, 2>;
    readonly TransactionHash: SerializedField<Hash256, 327683, 5, 3>;
    readonly AccountHash: SerializedField<Hash256, 327684, 5, 4>;
    readonly PreviousTxnID: SerializedField<Hash256, 327685, 5, 5>;
    readonly LedgerIndex: SerializedField<Hash256, 327686, 5, 6>;
    readonly WalletLocator: SerializedField<Hash256, 327687, 5, 7>;
    readonly RootIndex: SerializedField<Hash256, 327688, 5, 8>;
    readonly AccountTxnID: SerializedField<Hash256, 327689, 5, 9>;
    readonly NFTokenID: SerializedField<Hash256, 327690, 5, 10>;
    readonly EmitParentTxnID: SerializedField<Hash256, 327691, 5, 11>;
    readonly EmitNonce: SerializedField<Hash256, 327692, 5, 12>;
    readonly EmitHookHash: SerializedField<Hash256, 327693, 5, 13>;
    readonly ObjectID: SerializedField<Hash256, 327694, 5, 14>;
    readonly BookDirectory: SerializedField<Hash256, 327696, 5, 16>;
    readonly InvoiceID: SerializedField<Hash256, 327697, 5, 17>;
    readonly Nickname: SerializedField<Hash256, 327698, 5, 18>;
    readonly Amendment: SerializedField<Hash256, 327699, 5, 19>;
    readonly HookOn: SerializedField<Hash256, 327700, 5, 20>;
    readonly Digest: SerializedField<Hash256, 327701, 5, 21>;
    readonly Channel: SerializedField<Hash256, 327702, 5, 22>;
    readonly ConsensusHash: SerializedField<Hash256, 327703, 5, 23>;
    readonly CheckID: SerializedField<Hash256, 327704, 5, 24>;
    readonly ValidatedHash: SerializedField<Hash256, 327705, 5, 25>;
    readonly PreviousPageMin: SerializedField<Hash256, 327706, 5, 26>;
    readonly NextPageMin: SerializedField<Hash256, 327707, 5, 27>;
    readonly NFTokenBuyOffer: SerializedField<Hash256, 327708, 5, 28>;
    readonly NFTokenSellOffer: SerializedField<Hash256, 327709, 5, 29>;
    readonly HookStateKey: SerializedField<Hash256, 327710, 5, 30>;
    readonly HookHash: SerializedField<Hash256, 327711, 5, 31>;
    readonly HookNamespace: SerializedField<Hash256, 327712, 5, 32>;
    readonly HookSetTxnID: SerializedField<Hash256, 327713, 5, 33>;
    readonly OfferID: SerializedField<Hash256, 327714, 5, 34>;
    readonly EscrowID: SerializedField<Hash256, 327715, 5, 35>;
    readonly URITokenID: SerializedField<Hash256, 327716, 5, 36>;
    readonly Cron: SerializedField<Hash256, 327775, 5, 95>;
    readonly HookCanEmit: SerializedField<Hash256, 327776, 5, 96>;
    readonly EmittedTxnID: SerializedField<Hash256, 327777, 5, 97>;
    readonly GovernanceMarks: SerializedField<Hash256, 327778, 5, 98>;
    readonly GovernanceFlags: SerializedField<Hash256, 327779, 5, 99>;
    readonly Amount: SerializedField<Amount, 393217, 6, 1>;
    readonly Balance: SerializedField<Amount, 393218, 6, 2>;
    readonly LimitAmount: SerializedField<Amount, 393219, 6, 3>;
    readonly TakerPays: SerializedField<Amount, 393220, 6, 4>;
    readonly TakerGets: SerializedField<Amount, 393221, 6, 5>;
    readonly LowLimit: SerializedField<Amount, 393222, 6, 6>;
    readonly HighLimit: SerializedField<Amount, 393223, 6, 7>;
    readonly Fee: SerializedField<Amount, 393224, 6, 8>;
    readonly SendMax: SerializedField<Amount, 393225, 6, 9>;
    readonly DeliverMin: SerializedField<Amount, 393226, 6, 10>;
    readonly MinimumOffer: SerializedField<Amount, 393232, 6, 16>;
    readonly RippleEscrow: SerializedField<Amount, 393233, 6, 17>;
    readonly DeliveredAmount: SerializedField<Amount, 393234, 6, 18>;
    readonly NFTokenBrokerFee: SerializedField<Amount, 393235, 6, 19>;
    readonly HookCallbackFee: SerializedField<Amount, 393236, 6, 20>;
    readonly LockedBalance: SerializedField<Amount, 393237, 6, 21>;
    readonly BaseFeeDrops: SerializedField<Amount, 393238, 6, 22>;
    readonly ReserveBaseDrops: SerializedField<Amount, 393239, 6, 23>;
    readonly ReserveIncrementDrops: SerializedField<Amount, 393240, 6, 24>;
    readonly PublicKey: SerializedField<STBlob, 458753, 7, 1>;
    readonly MessageKey: SerializedField<STBlob, 458754, 7, 2>;
    readonly SigningPubKey: SerializedField<STBlob, 458755, 7, 3>;
    readonly TxnSignature: SerializedField<STBlob, 458756, 7, 4>;
    readonly URI: SerializedField<STBlob, 458757, 7, 5>;
    readonly Signature: SerializedField<STBlob, 458758, 7, 6>;
    readonly Domain: SerializedField<STBlob, 458759, 7, 7>;
    readonly FundCode: SerializedField<STBlob, 458760, 7, 8>;
    readonly RemoveCode: SerializedField<STBlob, 458761, 7, 9>;
    readonly ExpireCode: SerializedField<STBlob, 458762, 7, 10>;
    readonly CreateCode: SerializedField<STBlob, 458763, 7, 11>;
    readonly MemoType: SerializedField<STBlob, 458764, 7, 12>;
    readonly MemoData: SerializedField<STBlob, 458765, 7, 13>;
    readonly MemoFormat: SerializedField<STBlob, 458766, 7, 14>;
    readonly Fulfillment: SerializedField<STBlob, 458768, 7, 16>;
    readonly Condition: SerializedField<STBlob, 458769, 7, 17>;
    readonly MasterSignature: SerializedField<STBlob, 458770, 7, 18>;
    readonly UNLModifyValidator: SerializedField<STBlob, 458771, 7, 19>;
    readonly ValidatorToDisable: SerializedField<STBlob, 458772, 7, 20>;
    readonly ValidatorToReEnable: SerializedField<STBlob, 458773, 7, 21>;
    readonly HookStateData: SerializedField<STBlob, 458774, 7, 22>;
    readonly HookReturnString: SerializedField<STBlob, 458775, 7, 23>;
    readonly HookParameterName: SerializedField<STBlob, 458776, 7, 24>;
    readonly HookParameterValue: SerializedField<STBlob, 458777, 7, 25>;
    readonly Blob: SerializedField<STBlob, 458778, 7, 26>;
    readonly RemarkValue: SerializedField<STBlob, 458850, 7, 98>;
    readonly RemarkName: SerializedField<STBlob, 458851, 7, 99>;
    readonly Account: SerializedField<AccountID, 524289, 8, 1>;
    readonly Owner: SerializedField<AccountID, 524290, 8, 2>;
    readonly Destination: SerializedField<AccountID, 524291, 8, 3>;
    readonly Issuer: SerializedField<AccountID, 524292, 8, 4>;
    readonly Authorize: SerializedField<AccountID, 524293, 8, 5>;
    readonly Unauthorize: SerializedField<AccountID, 524294, 8, 6>;
    readonly RegularKey: SerializedField<AccountID, 524296, 8, 8>;
    readonly NFTokenMinter: SerializedField<AccountID, 524297, 8, 9>;
    readonly EmitCallback: SerializedField<AccountID, 524298, 8, 10>;
    readonly HookAccount: SerializedField<AccountID, 524304, 8, 16>;
    readonly Inform: SerializedField<AccountID, 524387, 8, 99>;
    readonly TransactionMetaData: SerializedField<STObject, 917506, 14, 2>;
    readonly CreatedNode: SerializedField<STObject, 917507, 14, 3>;
    readonly DeletedNode: SerializedField<STObject, 917508, 14, 4>;
    readonly ModifiedNode: SerializedField<STObject, 917509, 14, 5>;
    readonly PreviousFields: SerializedField<STObject, 917510, 14, 6>;
    readonly FinalFields: SerializedField<STObject, 917511, 14, 7>;
    readonly NewFields: SerializedField<STObject, 917512, 14, 8>;
    readonly TemplateEntry: SerializedField<STObject, 917513, 14, 9>;
    readonly Memo: SerializedField<STObject, 917514, 14, 10>;
    readonly SignerEntry: SerializedField<STObject, 917515, 14, 11>;
    readonly NFToken: SerializedField<STObject, 917516, 14, 12>;
    readonly EmitDetails: SerializedField<STObject, 917517, 14, 13>;
    readonly Hook: SerializedField<STObject, 917518, 14, 14>;
    readonly Signer: SerializedField<STObject, 917520, 14, 16>;
    readonly Majority: SerializedField<STObject, 917522, 14, 18>;
    readonly DisabledValidator: SerializedField<STObject, 917523, 14, 19>;
    readonly EmittedTxn: SerializedField<STObject, 917524, 14, 20>;
    readonly HookExecution: SerializedField<STObject, 917525, 14, 21>;
    readonly HookParameter: SerializedField<STObject, 917527, 14, 23>;
    readonly HookGrant: SerializedField<STObject, 917528, 14, 24>;
    readonly AmountEntry: SerializedField<STObject, 917595, 14, 91>;
    readonly MintURIToken: SerializedField<STObject, 917596, 14, 92>;
    readonly HookEmission: SerializedField<STObject, 917597, 14, 93>;
    readonly ImportVLKey: SerializedField<STObject, 917598, 14, 94>;
    readonly ActiveValidator: SerializedField<STObject, 917599, 14, 95>;
    readonly GenesisMint: SerializedField<STObject, 917600, 14, 96>;
    readonly Remark: SerializedField<STObject, 917601, 14, 97>;
    readonly Signers: SerializedField<STArray, 983043, 15, 3>;
    readonly SignerEntries: SerializedField<STArray, 983044, 15, 4>;
    readonly Template: SerializedField<STArray, 983045, 15, 5>;
    readonly Necessary: SerializedField<STArray, 983046, 15, 6>;
    readonly Sufficient: SerializedField<STArray, 983047, 15, 7>;
    readonly AffectedNodes: SerializedField<STArray, 983048, 15, 8>;
    readonly Memos: SerializedField<STArray, 983049, 15, 9>;
    readonly NFTokens: SerializedField<STArray, 983050, 15, 10>;
    readonly Hooks: SerializedField<STArray, 983051, 15, 11>;
    readonly Majorities: SerializedField<STArray, 983056, 15, 16>;
    readonly DisabledValidators: SerializedField<STArray, 983057, 15, 17>;
    readonly HookExecutions: SerializedField<STArray, 983058, 15, 18>;
    readonly HookParameters: SerializedField<STArray, 983059, 15, 19>;
    readonly HookGrants: SerializedField<STArray, 983060, 15, 20>;
    readonly Amounts: SerializedField<STArray, 983132, 15, 92>;
    readonly HookEmissions: SerializedField<STArray, 983133, 15, 93>;
    readonly ImportVLKeys: SerializedField<STArray, 983134, 15, 94>;
    readonly ActiveValidators: SerializedField<STArray, 983135, 15, 95>;
    readonly GenesisMints: SerializedField<STArray, 983136, 15, 96>;
    readonly Remarks: SerializedField<STArray, 983137, 15, 97>;
    readonly CloseResolution: SerializedField<UInt8, 1048577, 16, 1>;
    readonly Method: SerializedField<UInt8, 1048578, 16, 2>;
    readonly TransactionResult: SerializedField<TransactionResult, 1048579, 16, 3>;
    readonly TickSize: SerializedField<UInt8, 1048592, 16, 16>;
    readonly UNLModifyDisabling: SerializedField<UInt8, 1048593, 16, 17>;
    readonly HookResult: SerializedField<UInt8, 1048594, 16, 18>;
    readonly TakerPaysCurrency: SerializedField<Hash160, 1114113, 17, 1>;
    readonly TakerPaysIssuer: SerializedField<Hash160, 1114114, 17, 2>;
    readonly TakerGetsCurrency: SerializedField<Hash160, 1114115, 17, 3>;
    readonly TakerGetsIssuer: SerializedField<Hash160, 1114116, 17, 4>;
    readonly Paths: SerializedField<PathSet, 1179649, 18, 1>;
    readonly Indexes: SerializedField<Vector256, 1245185, 19, 1>;
    readonly Hashes: SerializedField<Vector256, 1245186, 19, 2>;
    readonly Amendments: SerializedField<Vector256, 1245187, 19, 3>;
    readonly NFTokenOffers: SerializedField<Vector256, 1245188, 19, 4>;
    readonly HookNamespaces: SerializedField<Vector256, 1245189, 19, 5>;
    readonly URITokenIDs: SerializedField<Vector256, 1245283, 19, 99>;
    readonly TradingFee: SerializedField<UInt16, 65541, 1, 5>;
    readonly DiscountedFee: SerializedField<UInt16, 65542, 1, 6>;
    readonly LedgerFixType: SerializedField<UInt16, 65558, 1, 22>;
    readonly LastUpdateTime: SerializedField<UInt32, 131087, 2, 15>;
    readonly VoteWeight: SerializedField<UInt32, 131120, 2, 48>;
    readonly OracleDocumentID: SerializedField<UInt32, 131123, 2, 51>;
    readonly XChainClaimID: SerializedField<UInt64, 196628, 3, 20>;
    readonly XChainAccountCreateCount: SerializedField<UInt64, 196629, 3, 21>;
    readonly XChainAccountClaimCount: SerializedField<UInt64, 196630, 3, 22>;
    readonly AssetPrice: SerializedField<UInt64, 196631, 3, 23>;
    readonly MaximumAmount: SerializedField<UInt64, 196632, 3, 24>;
    readonly OutstandingAmount: SerializedField<UInt64, 196633, 3, 25>;
    readonly MPTAmount: SerializedField<UInt64, 196634, 3, 26>;
    readonly IssuerNode: SerializedField<UInt64, 196635, 3, 27>;
    readonly SubjectNode: SerializedField<UInt64, 196636, 3, 28>;
    readonly AMMID: SerializedField<Hash256, 327695, 5, 15>;
    readonly DomainID: SerializedField<Hash256, 327717, 5, 37>;
    readonly HookOnOutgoing: SerializedField<Hash256, 327773, 5, 93>;
    readonly HookOnIncoming: SerializedField<Hash256, 327774, 5, 94>;
    readonly Amount2: SerializedField<Amount, 393227, 6, 11>;
    readonly BidMin: SerializedField<Amount, 393228, 6, 12>;
    readonly BidMax: SerializedField<Amount, 393229, 6, 13>;
    readonly LPTokenOut: SerializedField<Amount, 393241, 6, 25>;
    readonly LPTokenIn: SerializedField<Amount, 393242, 6, 26>;
    readonly EPrice: SerializedField<Amount, 393243, 6, 27>;
    readonly Price: SerializedField<Amount, 393244, 6, 28>;
    readonly SignatureReward: SerializedField<Amount, 393245, 6, 29>;
    readonly MinAccountCreateAmount: SerializedField<Amount, 393246, 6, 30>;
    readonly LPTokenBalance: SerializedField<Amount, 393247, 6, 31>;
    readonly TrustLineRewardAccumulator: SerializedField<Amount, 393315, 6, 99>;
    readonly DIDDocument: SerializedField<STBlob, 458779, 7, 27>;
    readonly Data: SerializedField<STBlob, 458780, 7, 28>;
    readonly AssetClass: SerializedField<STBlob, 458781, 7, 29>;
    readonly Provider: SerializedField<STBlob, 458782, 7, 30>;
    readonly MPTokenMetadata: SerializedField<STBlob, 458783, 7, 31>;
    readonly CredentialType: SerializedField<STBlob, 458784, 7, 32>;
    readonly HookName: SerializedField<STBlob, 458849, 7, 97>;
    readonly Holder: SerializedField<AccountID, 524299, 8, 11>;
    readonly OtherChainSource: SerializedField<AccountID, 524306, 8, 18>;
    readonly OtherChainDestination: SerializedField<AccountID, 524307, 8, 19>;
    readonly AttestationSignerAccount: SerializedField<AccountID, 524308, 8, 20>;
    readonly AttestationRewardAccount: SerializedField<AccountID, 524309, 8, 21>;
    readonly LockingChainDoor: SerializedField<AccountID, 524310, 8, 22>;
    readonly IssuingChainDoor: SerializedField<AccountID, 524311, 8, 23>;
    readonly Subject: SerializedField<AccountID, 524312, 8, 24>;
    readonly Number: SerializedField<string, 589825, 9, 1>;
    readonly VoteEntry: SerializedField<STObject, 917529, 14, 25>;
    readonly AuctionSlot: SerializedField<STObject, 917530, 14, 26>;
    readonly AuthAccount: SerializedField<STObject, 917531, 14, 27>;
    readonly XChainClaimProofSig: SerializedField<STObject, 917532, 14, 28>;
    readonly XChainCreateAccountProofSig: SerializedField<STObject, 917533, 14, 29>;
    readonly XChainClaimAttestationCollectionElement: SerializedField<STObject, 917534, 14, 30>;
    readonly XChainCreateAccountAttestationCollectionElement: SerializedField<STObject, 917535, 14, 31>;
    readonly PriceData: SerializedField<STObject, 917536, 14, 32>;
    readonly Credential: SerializedField<STObject, 917537, 14, 33>;
    readonly HighReward: SerializedField<STObject, 917602, 14, 98>;
    readonly LowReward: SerializedField<STObject, 917603, 14, 99>;
    readonly VoteSlots: SerializedField<STArray, 983052, 15, 12>;
    readonly XChainClaimAttestations: SerializedField<STArray, 983061, 15, 21>;
    readonly XChainCreateAccountAttestations: SerializedField<STArray, 983062, 15, 22>;
    readonly PriceDataSeries: SerializedField<STArray, 983064, 15, 24>;
    readonly AuthAccounts: SerializedField<STArray, 983065, 15, 25>;
    readonly AuthorizeCredentials: SerializedField<STArray, 983066, 15, 26>;
    readonly UnauthorizeCredentials: SerializedField<STArray, 983067, 15, 27>;
    readonly AcceptedCredentials: SerializedField<STArray, 983068, 15, 28>;
    readonly Scale: SerializedField<UInt8, 1048580, 16, 4>;
    readonly AssetScale: SerializedField<UInt8, 1048581, 16, 5>;
    readonly WasLockingChainSend: SerializedField<UInt8, 1048595, 16, 19>;
    readonly CredentialIDs: SerializedField<Vector256, 1245190, 19, 6>;
    readonly MPTokenIssuanceID: SerializedField<Hash192, 1376257, 21, 1>;
    readonly LockingChainIssue: SerializedField<Issue, 1572865, 24, 1>;
    readonly IssuingChainIssue: SerializedField<Issue, 1572866, 24, 2>;
    readonly Asset: SerializedField<Issue, 1572867, 24, 3>;
    readonly Asset2: SerializedField<Issue, 1572868, 24, 4>;
    readonly ClaimCurrency: SerializedField<Issue, 1572869, 24, 5>;
    readonly XChainBridge: SerializedField<XChainBridge, 1638401, 25, 1>;
    readonly BaseAsset: SerializedField<Currency, 1703937, 26, 1>;
    readonly QuoteAsset: SerializedField<Currency, 1703938, 26, 2>;
  };
}
