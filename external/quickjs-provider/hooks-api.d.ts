export {};

declare const __stObjectBrand: unique symbol;
declare const __stArrayBrand: unique symbol;
declare const __providerValueBrand: unique symbol;
declare const __binarySchemaBrand: unique symbol;
declare const __resultBrand: unique symbol;
declare const __voidResultBrand: unique symbol;
declare const __recordFieldBrand: unique symbol;
declare const __serializedFieldBrand: unique symbol;
declare const __ledgerKeyletValueBrand: unique symbol;

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

/**
 * Every transaction type a Hook can observe or emit.
 *
 * Hidden module-scope inventory for the frozen TransactionType runtime namespace.
 * Consumers use the same-named global value and derived union type.
 */
interface TransactionTypeEnum {
  readonly Payment: 0;
  readonly Invoke: 99;
  readonly GenesisMint: 96;
  readonly Import: 97;
  readonly ClaimReward: 98;
  readonly SetHook: 22;
  readonly TrustSet: 20;
  readonly Remit: 95;
  readonly NFTokenBurn: 26;
  readonly URITokenMint: 45;
  readonly UNLReport: 104;
  readonly EmitFailure: 103;
  readonly UNLModify: 102;
  readonly SetFee: 101;
  readonly EnableAmendment: 100;
  readonly SetRemarks: 94;
  readonly CronSet: 93;
  readonly Cron: 92;
  readonly PermissionedDomainDelete: 72;
  readonly PermissionedDomainSet: 71;
  readonly NFTokenModify: 70;
  readonly CredentialDelete: 69;
  readonly CredentialAccept: 68;
  readonly CredentialCreate: 67;
  readonly MPTokenAuthorize: 66;
  readonly MPTokenIssuanceSet: 65;
  readonly MPTokenIssuanceDestroy: 64;
  readonly MPTokenIssuanceCreate: 63;
  readonly LedgerStateFix: 62;
  readonly OracleDelete: 61;
  readonly OracleSet: 60;
  readonly DIDDelete: 59;
  readonly DIDSet: 58;
  readonly XChainCreateBridge: 57;
  readonly XChainModifyBridge: 56;
  readonly XChainAddAccountCreateAttestation: 55;
  readonly XChainAddClaimAttestation: 54;
  readonly XChainAccountCreateCommit: 53;
  readonly XChainClaim: 52;
  readonly XChainCommit: 51;
  readonly XChainCreateClaimID: 50;
  readonly URITokenCancelSellOffer: 49;
  readonly URITokenCreateSellOffer: 48;
  readonly URITokenBuy: 47;
  readonly URITokenBurn: 46;
  readonly AMMDelete: 40;
  readonly AMMBid: 39;
  readonly AMMVote: 38;
  readonly AMMWithdraw: 37;
  readonly AMMDeposit: 36;
  readonly AMMCreate: 35;
  readonly AMMClawback: 31;
  readonly Clawback: 30;
  readonly NFTokenAcceptOffer: 29;
  readonly NFTokenCancelOffer: 28;
  readonly NFTokenCreateOffer: 27;
  readonly NFTokenMint: 25;
  readonly AccountDelete: 21;
  readonly DepositPreauth: 19;
  readonly CheckCancel: 18;
  readonly CheckCash: 17;
  readonly CheckCreate: 16;
  readonly PaymentChannelClaim: 15;
  readonly PaymentChannelFund: 14;
  readonly PaymentChannelCreate: 13;
  readonly SignerListSet: 12;
  readonly SpinalTap: 11;
  readonly TicketCreate: 10;
  readonly Contract: 9;
  readonly OfferCancel: 8;
  readonly OfferCreate: 7;
  readonly NicknameSet: 6;
  readonly SetRegularKey: 5;
  readonly EscrowCancel: 4;
  readonly AccountSet: 3;
  readonly EscrowFinish: 2;
  readonly EscrowCreate: 1;
}

/**
 * Every ledger-entry type the provider can observe.
 *
 * Hidden module-scope inventory for the frozen LedgerEntryType runtime
 * namespace. Consumers use the same-named global value and derived union type.
 */
interface LedgerEntryTypeEnum {
  readonly DID: 141;
  readonly PermissionedDomain: 130;
  readonly Credential: 129;
  readonly Oracle: 128;
  readonly MPToken: 127;
  readonly MPTokenIssuance: 126;
  readonly AMM: 121;
  readonly PayChannel: 120;
  readonly HookState: 118;
  readonly Escrow: 117;
  readonly XChainOwnedCreateAccountClaimID: 116;
  readonly FeeSettings: 115;
  readonly RippleState: 114;
  readonly XChainOwnedClaimID: 113;
  readonly DepositPreauth: 112;
  readonly Offer: 111;
  readonly Bridge: 105;
  readonly LedgerHashes: 104;
  readonly Amendments: 102;
  readonly DirectoryNode: 100;
  readonly AccountRoot: 97;
  readonly URIToken: 85;
  readonly Ticket: 84;
  readonly SignerList: 83;
  readonly UNLReport: 82;
  readonly NFTokenPage: 80;
  readonly NegativeUNL: 78;
  readonly ImportVLSequence: 73;
  readonly Hook: 72;
  readonly EmittedTxn: 69;
  readonly HookDefinition: 68;
  readonly Check: 67;
  readonly Cron: 65;
  readonly NFTokenOffer: 55;
}

/**
 * Every transaction-engine result a Hook can observe.
 *
 * Hidden module-scope inventory for the frozen TransactionResult runtime namespace.
 * Consumers use the same-named global value and derived union type.
 */
interface TransactionResultEnum {
  readonly tesSUCCESS: 0;
  readonly tecLAST_POSSIBLE_ENTRY: 255;
  readonly tecBAD_CREDENTIALS: 199;
  readonly tecLOCKED: 198;
  readonly tecARRAY_TOO_LARGE: 197;
  readonly tecARRAY_EMPTY: 196;
  readonly tecTOKEN_PAIR_NOT_FOUND: 195;
  readonly tecINVALID_UPDATE_TIME: 194;
  readonly tecEMPTY_DID: 193;
  readonly tecXCHAIN_BAD_PUBLIC_KEY_ACCOUNT_PAIR: 192;
  readonly tecINCOMPLETE: 191;
  readonly tecHAS_HOOK_STATE: 190;
  readonly tecTOO_MANY_REMARKS: 189;
  readonly tecIMMUTABLE: 188;
  readonly tecINSUF_RESERVE_SELLER: 187;
  readonly tecXCHAIN_CREATE_ACCOUNT_DISABLED: 186;
  readonly tecXCHAIN_SELF_COMMIT: 185;
  readonly tecXCHAIN_PAYMENT_FAILED: 184;
  readonly tecXCHAIN_ACCOUNT_CREATE_TOO_MANY: 183;
  readonly tecXCHAIN_ACCOUNT_CREATE_PAST: 182;
  readonly tecXCHAIN_INSUFF_CREATE_AMOUNT: 181;
  readonly tecXCHAIN_SENDING_ACCOUNT_MISMATCH: 180;
  readonly tecXCHAIN_NO_SIGNERS_LIST: 179;
  readonly tecXCHAIN_REWARD_MISMATCH: 178;
  readonly tecXCHAIN_WRONG_CHAIN: 177;
  readonly tecXCHAIN_CREATE_ACCOUNT_NONXRP_ISSUE: 176;
  readonly tecXCHAIN_PROOF_UNKNOWN_KEY: 175;
  readonly tecXCHAIN_CLAIM_NO_QUORUM: 174;
  readonly tecXCHAIN_BAD_CLAIM_ID: 173;
  readonly tecXCHAIN_NO_CLAIM_ID: 172;
  readonly tecXCHAIN_BAD_TRANSFER_ISSUE: 171;
  readonly tecPRECISION_LOSS: 170;
  readonly tecREQUIRES_FLAG: 169;
  readonly tecAMM_ACCOUNT: 168;
  readonly tecAMM_NOT_EMPTY: 167;
  readonly tecAMM_EMPTY: 166;
  readonly tecAMM_INVALID_TOKENS: 165;
  readonly tecAMM_FAILED: 164;
  readonly tecAMM_BALANCE: 163;
  readonly tecUNFUNDED_AMM: 162;
  readonly tecINSUFFICIENT_PAYMENT: 161;
  readonly tecOBJECT_NOT_FOUND: 160;
  readonly tecINSUFFICIENT_FUNDS: 159;
  readonly tecCANT_ACCEPT_OWN_NFTOKEN_OFFER: 158;
  readonly tecNFTOKEN_OFFER_TYPE_MISMATCH: 157;
  readonly tecNFTOKEN_BUY_SELL_MISMATCH: 156;
  readonly tecNO_SUITABLE_NFTOKEN_PAGE: 155;
  readonly tecMAX_SEQUENCE_REACHED: 154;
  readonly tecHOOK_REJECTED: 153;
  readonly tecTOO_SOON: 152;
  readonly tecHAS_OBLIGATIONS: 151;
  readonly tecKILLED: 150;
  readonly tecDUPLICATE: 149;
  readonly tecEXPIRED: 148;
  readonly tecINVARIANT_FAILED: 147;
  readonly tecCRYPTOCONDITION_ERROR: 146;
  readonly tecOVERSIZE: 145;
  readonly tecINTERNAL: 144;
  readonly tecDST_TAG_NEEDED: 143;
  readonly tecNEED_MASTER_KEY: 142;
  readonly tecINSUFFICIENT_RESERVE: 141;
  readonly tecNO_ENTRY: 140;
  readonly tecNO_PERMISSION: 139;
  readonly tecNO_TARGET: 138;
  readonly tecFROZEN: 137;
  readonly tecINSUFF_FEE: 136;
  readonly tecNO_LINE: 135;
  readonly tecNO_AUTH: 134;
  readonly tecNO_ISSUER: 133;
  readonly tecOWNERS: 132;
  readonly tecNO_REGULAR_KEY: 131;
  readonly tecNO_ALTERNATIVE_KEY: 130;
  readonly tecUNFUNDED: 129;
  readonly tecPATH_DRY: 128;
  readonly tecNO_LINE_REDUNDANT: 127;
  readonly tecNO_LINE_INSUF_RESERVE: 126;
  readonly tecNO_DST_INSUF_NATIVE: 125;
  readonly tecNO_DST: 124;
  readonly tecINSUF_RESERVE_OFFER: 123;
  readonly tecINSUF_RESERVE_LINE: 122;
  readonly tecDIR_FULL: 121;
  readonly tecFAILED_PROCESSING: 105;
  readonly tecUNFUNDED_PAYMENT: 104;
  readonly tecUNFUNDED_OFFER: 103;
  readonly tecUNFUNDED_ADD: 102;
  readonly tecPATH_PARTIAL: 101;
  readonly tecCLAIM: 100;
  readonly tesPARTIAL: 1;
  readonly terNO_HOOK: -86;
  readonly terNO_AMM: -87;
  readonly terPRE_TICKET: -88;
  readonly terQUEUED: -89;
  readonly terNO_RIPPLE: -90;
  readonly terLAST: -91;
  readonly terPRE_SEQ: -92;
  readonly terOWNERS: -93;
  readonly terNO_LINE: -94;
  readonly terNO_AUTH: -95;
  readonly terNO_ACCOUNT: -96;
  readonly terINSUF_FEE_B: -97;
  readonly terFUNDS_SPENT: -98;
  readonly terRETRY: -99;
  readonly tefINVALID_LEDGER_FIX_TYPE: -174;
  readonly tefIMPORT_BLACKHOLED: -175;
  readonly tefNONDIR_EMIT: -176;
  readonly tefPAST_IMPORT_VL_SEQ: -177;
  readonly tefPAST_IMPORT_SEQ: -178;
  readonly tefNFTOKEN_IS_NOT_TRANSFERABLE: -179;
  readonly tefNO_TICKET: -180;
  readonly tefTOO_BIG: -181;
  readonly tefINVARIANT_FAILED: -182;
  readonly tefBAD_AUTH_MASTER: -183;
  readonly tefNOT_MULTI_SIGNING: -184;
  readonly tefBAD_QUORUM: -185;
  readonly tefBAD_SIGNATURE: -186;
  readonly tefMAX_LEDGER: -187;
  readonly tefMASTER_DISABLED: -188;
  readonly tefWRONG_PRIOR: -189;
  readonly tefPAST_SEQ: -190;
  readonly tefNO_AUTH_REQUIRED: -191;
  readonly tefINTERNAL: -192;
  readonly tefEXCEPTION: -193;
  readonly tefCREATED: -194;
  readonly tefBAD_LEDGER: -195;
  readonly tefBAD_AUTH: -196;
  readonly tefBAD_ADD_AUTH: -197;
  readonly tefALREADY: -198;
  readonly tefFAILURE: -199;
  readonly temBAD_TRANSFER_FEE: -249;
  readonly temARRAY_TOO_LARGE: -250;
  readonly temARRAY_EMPTY: -251;
  readonly temEMPTY_DID: -252;
  readonly temHOOK_DATA_TOO_LARGE: -253;
  readonly temXCHAIN_TOO_MANY_ATTESTATIONS: -254;
  readonly temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT: -255;
  readonly temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT: -256;
  readonly temXCHAIN_BRIDGE_NONDOOR_OWNER: -257;
  readonly temXCHAIN_BRIDGE_BAD_ISSUES: -258;
  readonly temXCHAIN_BAD_PROOF: -259;
  readonly temXCHAIN_EQUAL_DOOR_ACCOUNTS: -260;
  readonly temBAD_AMM_TOKENS: -261;
  readonly temBAD_NFTOKEN_TRANSFER_FEE: -262;
  readonly temSEQ_AND_TICKET: -263;
  readonly temUNKNOWN: -264;
  readonly temUNCERTAIN: -265;
  readonly temINVALID_COUNT: -266;
  readonly temCANNOT_PREAUTH_SELF: -267;
  readonly temINVALID_ACCOUNT_ID: -268;
  readonly temBAD_TICK_SIZE: -269;
  readonly temBAD_WEIGHT: -270;
  readonly temBAD_QUORUM: -271;
  readonly temBAD_SIGNER: -272;
  readonly temDISABLED: -273;
  readonly temRIPPLE_EMPTY: -274;
  readonly temREDUNDANT: -275;
  readonly temINVALID_FLAG: -276;
  readonly temINVALID: -277;
  readonly temDST_NEEDED: -278;
  readonly temDST_IS_SRC: -279;
  readonly temBAD_TRANSFER_RATE: -280;
  readonly temBAD_SRC_ACCOUNT: -281;
  readonly temBAD_SIGNATURE: -282;
  readonly temBAD_SEQUENCE: -283;
  readonly temBAD_SEND_NATIVE_PATHS: -284;
  readonly temBAD_SEND_NATIVE_PARTIAL: -285;
  readonly temBAD_SEND_NATIVE_NO_DIRECT: -286;
  readonly temBAD_SEND_NATIVE_MAX: -287;
  readonly temBAD_SEND_NATIVE_LIMIT: -288;
  readonly temBAD_REGKEY: -289;
  readonly temBAD_PATH_LOOP: -290;
  readonly temBAD_PATH: -291;
  readonly temBAD_OFFER: -292;
  readonly temBAD_LIMIT: -293;
  readonly temBAD_ISSUER: -294;
  readonly temBAD_FEE: -295;
  readonly temBAD_EXPIRATION: -296;
  readonly temBAD_CURRENCY: -297;
  readonly temBAD_AMOUNT: -298;
  readonly temMALFORMED: -299;
  readonly telENV_RPC_FAILED: -380;
  readonly telCAN_NOT_QUEUE_IMPORT: -381;
  readonly telIMPORT_VL_KEY_NOT_RECOGNISED: -382;
  readonly telNON_LOCAL_EMITTED_TXN: -383;
  readonly telNETWORK_ID_MAKES_TX_NON_CANONICAL: -384;
  readonly telREQUIRES_NETWORK_ID: -385;
  readonly telWRONG_NETWORK: -386;
  readonly telCAN_NOT_QUEUE_FULL: -387;
  readonly telCAN_NOT_QUEUE_FEE: -388;
  readonly telCAN_NOT_QUEUE_BLOCKED: -389;
  readonly telCAN_NOT_QUEUE_BLOCKS: -390;
  readonly telCAN_NOT_QUEUE_BALANCE: -391;
  readonly telCAN_NOT_QUEUE: -392;
  readonly telNO_DST_PARTIAL: -393;
  readonly telINSUF_FEE_P: -394;
  readonly telFAILED_PROCESSING: -395;
  readonly telBAD_PUBLIC_KEY: -396;
  readonly telBAD_PATH_COUNT: -397;
  readonly telBAD_DOMAIN: -398;
  readonly telLOCAL_ERROR: -399;
}

/**
 * Negative values returned by Hook host functions on failure.
 *
 * Hidden module-scope inventory for the frozen HookReturnCode runtime namespace.
 * Consumers use the same-named global value and derived union type.
 */
interface HookReturnCodeEnum {
  readonly SUCCESS: 0;
  readonly OUT_OF_BOUNDS: -1;
  readonly INTERNAL_ERROR: -2;
  readonly TOO_BIG: -3;
  readonly TOO_SMALL: -4;
  readonly DOESNT_EXIST: -5;
  readonly NO_FREE_SLOTS: -6;
  readonly INVALID_ARGUMENT: -7;
  readonly ALREADY_SET: -8;
  readonly PREREQUISITE_NOT_MET: -9;
  readonly FEE_TOO_LARGE: -10;
  readonly EMISSION_FAILURE: -11;
  readonly TOO_MANY_NONCES: -12;
  readonly TOO_MANY_EMITTED_TXN: -13;
  readonly NOT_IMPLEMENTED: -14;
  readonly INVALID_ACCOUNT: -15;
  readonly GUARD_VIOLATION: -16;
  readonly INVALID_FIELD: -17;
  readonly PARSE_ERROR: -18;
  readonly RC_ROLLBACK: -19;
  readonly RC_ACCEPT: -20;
  readonly NO_SUCH_KEYLET: -21;
  readonly NOT_AN_ARRAY: -22;
  readonly NOT_AN_OBJECT: -23;
  readonly DIVISION_BY_ZERO: -25;
  readonly MANTISSA_OVERSIZED: -26;
  readonly MANTISSA_UNDERSIZED: -27;
  readonly EXPONENT_OVERSIZED: -28;
  readonly EXPONENT_UNDERSIZED: -29;
  readonly XFL_OVERFLOW: -30;
  readonly NOT_IOU_AMOUNT: -31;
  readonly NOT_AN_AMOUNT: -32;
  readonly CANT_RETURN_NEGATIVE: -33;
  readonly NOT_AUTHORIZED: -34;
  readonly PREVIOUS_FAILURE_PREVENTS_RETRY: -35;
  readonly TOO_MANY_PARAMS: -36;
  readonly INVALID_TXN: -37;
  readonly RESERVE_INSUFFICIENT: -38;
  readonly COMPLEX_NOT_SUPPORTED: -39;
  readonly DOES_NOT_MATCH: -40;
  readonly INVALID_KEY: -41;
  readonly NOT_A_STRING: -42;
  readonly MEM_OVERLAP: -43;
  readonly TOO_MANY_STATE_MODIFICATIONS: -44;
  readonly TOO_MANY_NAMESPACES: -45;
  readonly INVALID_FLOAT: -10024;
}


declare global {
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

  /**
   * The canonical JavaScript/TypeScript API declaration for Xahau Hooks.
   *
   * Shaped by lazy rich native values and batched host round-trips. Consequently:
   * no buffer parameters and no output-length arguments. Ordinary negative host
   * statuses are tagged `HostResult` values; exceptions are reserved for hook
   * termination and JavaScript/runtime programming faults.
   *
   * An uncaught JavaScript exception is an execution error, not an implicit
   * call to `rollback`. Xahau records `WASM_ERROR`, rejects the transaction, and
   * discards provisional Hook effects. Use `Result` plus an explicit
   * `rollback` policy for failures that form part of normal contract control
   * flow; Hooks should not need `try`/`catch` around ordinary API operations.
   *
   * This file is the public specification. Runtime profiles may implement a
   * deliberately versioned subset; tooling must not silently widen that subset.
   */

  /**
   * MODULE CONTRACT — how a hook binds to the provider. A hook is one
   * ES module; the provider invokes its EXPORTS by name:
   *
   * - `export function main(): never` — the hook entry
   *   point. The Wasm `qjs_hook` export locates the module's exported
   *   `main` and calls it. A hook terminates through `accept` /
   *   `rollback` (hence `never`); returning without a terminal is an
   *   execution error, not an implicit accept.
   * - `export function callback(info: CallbackInfo): never` — the
   *   emitted-transaction callback entry, invoked through `qjs_cbak`.
   *   Required exactly when the hook expects callbacks; the provider
   *   raises a TypeError when the export is missing or not callable.
   * - `export const hookConfig = defineHookConfig({ ... })` — optional
   *   module configuration; unmarked `XFLDecimal` arithmetic resolves
   *   its profile through this export (declared-then-bare, 0060).
   *
   * The C SDK's entry-point NAME (`hook`) is a global NAMESPACE here;
   * the module entry is `main`, never `hook` — `export function hook`
   * shadows the namespace and does not compile.
   */
  type HookEntry = () => never;
  type CallbackEntry = (info: CallbackInfo) => never;

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
  type UInt32OrHash = UInt32 | Hash256;
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
  /** A provider-minted value with one canonical ledger representation. */
  type SerializedType = (
    | { readonly [__providerValueBrand]: string }
    | { readonly [__stObjectBrand]: void }
    | { readonly [__stArrayBrand]: void }
  ) & {
    toBytes(options?: SerializationOptions): Uint8Array;
  };
  type BytePart = BytesLike | SerializedType;
  /** State-key input: octets, string text encoded as UTF-8, or a serial value. */
  type StateKeyLike = BytesLike | string | SerializedType;
  /** State-value input: octets, string text encoded as UTF-8, or a serial value. */
  type StateValueLike = BytesLike | string | SerializedType;
  type BatchKeys = Record<string, StateKeyLike>;
  type BatchValues<T extends Record<string, unknown>> = { readonly [K in keyof T]: STBlob | undefined };


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

  /**
   * One-layer result from a schema-aware host-byte read (state or params).
   * Failure remains distinguishable as either the original host status
   * (`code`) or a codec validation issue (`issue`); successful absence
   * remains `undefined`.
   *
   * Canonical triage keeps all three states distinct: first handle `!read.ok`
   * (preserving a host code or rejecting malformed bytes), then treat
   * `read.value === undefined` as the absent/default case, and only then use the
   * decoded value. Persisted malformed bytes must not silently take the absent
   * default.
   *
   * CAPACITY MODEL: untyped state reads retain the provider's full 4,096-byte
   * capacity. The opt-in state schema overload instead passes the schema's
   * exact `byteLength` to the same host call. A short successful read reaches
   * the schema and becomes `ParseError` `wrong-length`; an oversized value is
   * refused by the host as `TOO_SMALL`, so no fabricated `actualLength` is
   * reported. Other host errors and successful absence retain their channels.
   */
  type SchemaReadResult<T> = Result<T | undefined, HostError | ParseError>;
  /** Same type; kept so existing `state.get` prose still type-checks. */
  type StateReadResult<T> = SchemaReadResult<T>;

  /**
   * Per-state disposition for a policied schema read — the third
   * argument of `otxn.param` / `hook.param` / `state.get` /
   * `state.foreign(...).get`. Pure tokens, all three states mandatory:
   * `"none"` folds that state to `undefined` in the return;
   * `"rollback"` is terminal with the read's own diagnostic;
   * `{ rollback: msg }` is terminal with the site's exact message;
   * `hostError: "propagate"` keeps the host failure as a Result.
   * Defaulting a value stays at the call site — `read(...) ?? fallback`
   * — visible, matching the UNIFORM C `!= width` fold (host failure,
   * wrong size, and absence all took the default). C idioms that split
   * wrong-length BY DIRECTION (`tl > 0 && tl != N`: short rolls back,
   * oversize silently defaults through the C buffer's TOO_SMALL) have
   * no single policy token: under the capacity model oversize is a
   * ParseError like any other, and the split is written explicitly
   * against `error.actualLength` (`okOrHandle` on the plain read).
   */
  interface ReadPolicies {
    readonly missing: "none" | "rollback" | { readonly rollback: string };
    readonly invalid: "none" | "rollback" | { readonly rollback: string };
    readonly hostError:
      | "none"
      | "rollback"
      | { readonly rollback: string }
      | "propagate";
  }
  /** `undefined` is reachable exactly when some policy is `"none"`. */
  type PolicyOutcome<T, P extends ReadPolicies> = "none" extends
    | P["missing"]
    | P["invalid"]
    | P["hostError"]
    ? T | undefined
    : T;
  /**
   * Return of a policied read: a bare value, unless `hostError:
   * "propagate"` keeps the host channel as a `HostResult`. A policy
   * object whose `hostError` is an un-narrowed union that merely
   * INCLUDES `"propagate"` types as the honest union of both shapes,
   * forcing the caller to discriminate (gemini s6 review: the earlier
   * tuple check silently chose the bare shape there).
   */
  type PolicyRead<T, P extends ReadPolicies> =
    | ("propagate" extends P["hostError"]
        ? HostResult<PolicyOutcome<T, P>>
        : never)
    | (Exclude<P["hostError"], "propagate"> extends never
        ? never
        : PolicyOutcome<T, P>);

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
    /**
     * True when every bit in the nonzero flag is present. A composite flag is
     * an ALL-bits test, never an ANY-bits test.
     */
    hasFlag(flag: UIntInput<Bits>): boolean;
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

  /**
   * A named scalar schema. Parsing requires exactly `byteLength`; encoding and
   * parsing reuse the element's representation. Runtime schema objects are frozen.
   */
  interface ScalarSchema<
    Name extends string,
    T,
    Width extends number,
  > extends BinarySchema<T> {
    readonly name: Name;
    readonly byteLength: Width;
    safeParse(value: BytesLike | STBlob): ParseResult<T>;
    parse(value: BytesLike | STBlob): T;
    /** Result-valued encode: rejects out-of-domain values as EncodeError. */
    safeEncode(value: T): EncodeResult;
    /**
     * Assertion form for programmer-guaranteed values. Throws when the value
     * leaves the codec's domain; prefer `safeEncode` for data-driven values.
     */
    encode(value: T): STBlob;
  }

  /**
   * One-element record: a width-known codec, no offset.
   * `cell("Hash256", record.hash(32))`.
   */
  function cell<
    const Name extends string,
    T,
    const Width extends number,
  >(
    name: Name,
    field: RecordField<T, Width>,
  ): ScalarSchema<Name, T, Width>;

  type RecordFieldValue<T> =
    T extends RecordField<infer V, number> ? V : never;

  type OverlayValue<T extends { readonly [K: string]: RecordField<unknown, number> }> = {
    -readonly [K in keyof T as RecordFieldValue<T[K]> extends never ? never : K]: RecordFieldValue<T[K]>;
  };

  interface RecordLayoutClaim {
    readonly expectOffset: number;
  }

  type RecordEntry<
    Name extends string = string,
    T = unknown,
    Width extends number = number,
  > =
    | readonly [name: Name, field: RecordField<T, Width>]
    | readonly [name: Name, field: RecordField<T, Width>, layout: RecordLayoutClaim]
    | RecordField<never, number>;

  type RecordEntries = readonly RecordEntry[];

  type RecordValueFromEntries<E extends RecordEntries> = {
    [T in E[number] as T extends readonly [infer N extends string, infer F, ...unknown[]]
      ? RecordFieldValue<F> extends never ? never : N
      : never
    ]: T extends readonly [string, infer F, ...unknown[]] ? RecordFieldValue<F> : never;
  };

  type RecordPatch<Value> = { [K in keyof Value]?: Value[K] };

  interface RecordSchema<
    Name extends string,
    Size extends number,
    Value,
  > extends BinarySchema<Value> {
    readonly name: Name;
    readonly byteLength: Size;

    /**
     * Decode a record after validating its size and field representations.
     * Prefer this result-valued form for state or transaction-derived bytes.
     */
    safeParse(value: BytesLike | STBlob): ParseResult<Value>;

    /**
     * Assertion form for a programmer-guaranteed record. Throws on malformed
     * input; it must not become the default for untrusted persisted bytes.
     */
    parse(value: BytesLike | STBlob): Value;

    /**
     * Result-valued encode: validates every field against its codec domain
     * and returns the exact record bytes, or an EncodeError naming the first
     * out-of-domain field.
     */
    safeEncode(value: Value): EncodeResult;

    /**
     * Assertion form for programmer-guaranteed values. Throws on
     * out-of-domain field values; prefer `safeEncode` for data-driven values.
     */
    encode(value: Value): STBlob;
    patch(
      source: BytesLike | STBlob,
      values: RecordPatch<Value>,
    ): ParseResult<STBlob>;
  }

  /**
   * Sequential fixed-width record. Each entry is an independent unit that
   * names its own length; the array order assigns offsets. `expectOffset` is
   * an assertion about the derived cursor, never a position. Reserved bytes
   * are `record.padding(n)` as a bare entry (no dummy name). Accidental overlap is unrepresentable; use
   * `record.overlay({ ... })` for equal-width reinterpretations of one range.
   * Construction rejects duplicate entry names.
   *
   * Construction refuses when the derived extent is not `byteLength`.
   */
  function record<
    const Name extends string,
    const Size extends number,
    const Entries extends RecordEntries,
  >(
    name: Name,
    byteLength: Size,
    fields: Entries,
  ): RecordSchema<Name, Size, RecordValueFromEntries<Entries>>;

  namespace record {
    function u8(): RecordField<number, 1>;
    namespace u8 {
      function uint(): RecordField<UInt8, 1>;
    }
    function u16be(): RecordField<number, 2>;
    function u16le(): RecordField<number, 2>;
    namespace u16be {
      function uint(): RecordField<UInt16, 2>;
    }
    namespace u16le {
      function uint(): RecordField<UInt16, 2>;
    }
    function u32be(): RecordField<number, 4>;
    function u32le(): RecordField<number, 4>;
    namespace u32be {
      function uint(): RecordField<UInt32, 4>;
    }
    namespace u32le {
      function uint(): RecordField<UInt32, 4>;
    }
    function i32be(): RecordField<number, 4>;
    function i32le(): RecordField<number, 4>;
    function u64be(): RecordField<bigint, 8>;
    function u64le(): RecordField<bigint, 8>;
    namespace u64be {
      function uint(): RecordField<UInt64, 8>;
    }
    namespace u64le {
      function uint(): RecordField<UInt64, 8>;
    }
    function i64be(): RecordField<bigint, 8>;
    function i64le(): RecordField<bigint, 8>;
    function xflbe(): RecordField<XFLDecimal, 8>;
    function xflle(): RecordField<XFLDecimal, 8>;
    function bytes<const Width extends number>(byteLength: Width): RecordField<STBlob, Width>;
    function hash(byteLength: 32): RecordField<Hash256, 32>;
    function hash<const Width extends HashWidth>(byteLength: Width): RecordField<HashByWidth[Width], Width>;
    function accountID(): RecordField<AccountID, 20>;
    function currency(): RecordField<Currency, 20>;

    /**
     * Exact `byteLength` ASCII bytes (0x00–0x7F). No padding, no NUL trim.
     * Wrong length or a non-ASCII byte is a parse failure.
     */
    function ascii<const Width extends number>(byteLength: Width): RecordField<string, Width>;
    /**
     * Exact `byteLength` UTF-8 bytes. No padding or terminator. Invalid UTF-8
     * or wrong encoded length is a parse failure.
     */
    function utf8<const Width extends number>(byteLength: Width): RecordField<string, Width>;
    /**
     * Buffer of `byteLength` bytes containing a NUL-terminated C string.
     * Decode stops at the first 0x00; trailing bytes after that must be 0x00.
     * Missing terminator or a non-zero tail is a parse failure.
     */
    function cString<const Width extends number>(byteLength: Width): RecordField<string, Width>;

    /** Occupies `byteLength` bytes and is omitted from parsed values. */
    function padding<const Width extends number>(byteLength: Width): RecordField<never, Width>;

    /**
     * One computed range with equal-width named interpretations. Construction
     * refuses mixed widths.
     */
    function overlay<
      const Width extends number,
      const Shape extends { readonly [K: string]: RecordField<unknown, Width> },
    >(interpretations: Shape): RecordField<OverlayValue<Shape>, Width>;
  }

  /**
   * Source-length admission witness: the explicit range of source byte
   * lengths a read contract accepts (reader authority — mandatory and
   * defaultless). `exact` restates writer authority at a read;
   * `between` admits an observed foreign/legacy range with a
   * contractual ceiling; `atLeast` is ONLY for a historical capacity
   * proven to be an implementation artifact the C program never
   * observed — an explicit choice, never a default.
   */
  interface SourceLength {
    readonly [__providerValueBrand]: "SourceLength";
    readonly min: number;
    /** `null`: no contractual ceiling (artifact branch of bound authority). */
    readonly max: number | null;
  }

  namespace view {
    interface PrefixOptions {
      readonly sourceLength: SourceLength;
    }
    /**
     * Reader-authority GUARDED-PREFIX projection: admits any source
     * whose byte length the witness accepts and decodes the record as
     * the source's prefix — the C idiom that length-guards a read and
     * then uses the leading fields (`if (len < N) …; use first N`). A
     * source outside the admitted range is a `wrong-length` ParseError;
     * nothing is ever filled or truncated. The admitted minimum must
     * cover `record.byteLength`; the provider throws at construction
     * on a range the prefix cannot satisfy (programmer error, like a
     * malformed `cell` name). For the OTHER fixed-capacity C idiom —
     * the pre-zeroed oversized buffer whose short reads keep a zero
     * tail — use `zeroPadded`, which declares the fill.
     */
    function prefix<Name extends string, Size extends number, Value>(
      record: RecordSchema<Name, Size, Value>,
      options: PrefixOptions,
    ): BinarySchema<Value>;

    /**
     * Reader-authority ZERO-FILL projection: admits any source whose
     * byte length the witness accepts, explicitly zero-fills a source
     * shorter than the record up to `record.byteLength`, then decodes
     * — the dominant C state-read idiom (`PE_ZERO(gv, 80);
     * state_foreign(gv, 80, …)`: short values leave the untouched tail
     * zero and the code reads fields out of it). Choosing this schema
     * IS the zero-fill declaration; nothing is ever filled implicitly
     * anywhere else. The witness may admit down to zero bytes —
     * `SourceLength.between(0, cap)` is the at-most form. A source
     * past the admitted ceiling is a `wrong-length` ParseError exactly
     * as in `prefix`.
     */
    function zeroPadded<Name extends string, Size extends number, Value>(
      record: RecordSchema<Name, Size, Value>,
      options: PrefixOptions,
    ): BinarySchema<Value>;
  }

  interface ByteCompareOptions {
    readonly caseSensitive?: boolean;
  }

  interface ByteFindOptions extends ByteCompareOptions {
    readonly start?: number;
  }

  interface SerializationOptions {
    readonly field?: string | number;
    readonly includeFieldHeader?: boolean;
  }

  /** @serial Blob */
  interface STBlob {
    readonly [__providerValueBrand]: "STBlob";
    readonly byteLength: number;
    byteAt(index: number): number;
    slice(start: number, end?: number): STBlob;
    toBytes(): Uint8Array;
    toHex(): HexString;
    /** Asserts exactly 1 byte. */
    toUint8(): number;
    /** Asserts exactly 4 bytes. */
    toUint32(endian?: "big" | "little"): number;
    /** Asserts exactly 8 bytes. */
    toUint64(endian?: "big" | "little"): bigint;
    /** Asserts exactly 8 bytes. */
    toXFL(endian?: "big" | "little"): XFLDecimal;
    /** Asserts exactly 20 bytes. */
    toAccountID(): AccountID;
    /** Asserts exactly 20 bytes. */
    toCurrency(): Currency;
    /** Asserts exactly 16 bytes. */
    toHash128(): Hash128;
    /** Asserts exactly 20 bytes. */
    toHash160(): Hash160;
    /** Asserts exactly 24 bytes. */
    toHash192(): Hash192;
    /** Asserts exactly 32 bytes. */
    toHash256(): Hash256;
    /** Asserts exactly 48 bytes. */
    toHash384(): Hash384;
    /** Asserts exactly 64 bytes. */
    toHash512(): Hash512;
    isZero(): boolean;
    /** Whole-value byte equality; values of differing lengths are unequal. */
    equals(other: BytesLike | STBlob): boolean;
    compare(other: BytesLike | STBlob, options?: ByteCompareOptions): -1 | 0 | 1;
    indexOf(needle: BytesLike | STBlob, options?: ByteFindOptions): number | undefined;
  }

  interface STBlobFactory extends RuntimeType<STBlob> {
    from(value: BytesLike | STBlob): STBlob;
    /** Decode an even-length hexadecimal literal. */
    fromHex(value: HexString): STBlob;
    /**
     * Concatenate raw byte-like parts without interpreting strings or rich
     * serialized values. Use `util.bytes` when parts include UTF-8 strings or
     * provider values that must first be encoded through their byte contract.
     */
    concat(...parts: (BytesLike | STBlob)[]): STBlob;
    /** Asserts an integer in 0..255. */
    fromUint8(value: number): STBlob;
    /** Asserts an integer in 0..2**32-1. */
    fromUint32(value: number, endian?: "big" | "little"): STBlob;
    /** Asserts an integer in 0..2**64-1. */
    fromUint64(value: bigint | number, endian?: "big" | "little"): STBlob;
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
    from(value: BytesLike | STBlob): Hash256;
    /** Decode exactly 32 bytes from an even-length hexadecimal literal. */
    fromHex(value: HexString): Hash256;
  }

  const Hash256: Hash256Factory;

  /** @serial Hash384 */
  interface Hash384 extends Hash<48> {
    readonly [__providerValueBrand]: "Hash384";
  }
  const Hash384: RuntimeType<Hash384> & {
    readonly zero: Hash384;
    from(value: BytesLike | STBlob | Hash<48>): Hash384;
  };

  /** @serial Hash512 */
  interface Hash512 extends Hash<64> {
    readonly [__providerValueBrand]: "Hash512";
  }
  const Hash512: RuntimeType<Hash512> & {
    readonly zero: Hash512;
    from(value: BytesLike | STBlob | Hash<64>): Hash512;
  };

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
  const STArray: RuntimeType<STArray<STObject>>;
  const NativeAmount: RuntimeType<NativeAmount>;
  const IOUAmount: RuntimeType<IOUAmount>;
  const MPTAmount: RuntimeType<MPTAmount>;

  interface HashByWidth {
    readonly 16: Hash128;
    readonly 20: Hash160;
    readonly 24: Hash192;
    readonly 32: Hash256;
    readonly 48: Hash384;
    readonly 64: Hash512;
  }

  /** @serial AccountID */
  interface AccountID {
    readonly [__providerValueBrand]: "AccountID";
    readonly r: string;
    toString(): string;
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
    from(value: BytesLike | STBlob): AccountID;
    /** Decode exactly 20 bytes from an even-length hexadecimal literal. */
    fromHex(value: HexString): AccountID;
    from(value: BytesLike | STBlob | Hash160 | string): AccountID;
    fromRAddress(value: string): AccountID;
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

  interface CurrencyFactory extends RuntimeType<Currency> {
    readonly native: Currency;
    from(value: BytesLike | STBlob | string): Currency;
  }

  const Currency: CurrencyFactory;

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

  interface IssueFactory extends RuntimeType<Issue> {
    native(): Issue;
    iou(currency: Currency, issuer: AccountID): Issue;
    mpt(mptIssuanceId: Hash192): Issue;
  }

  const Issue: IssueFactory;

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

  /** Failure from constructing or operating on an `XFLDecimal`. */
  interface XFLError {
    readonly domain: "xfl";
    readonly issue:
      | "overflow"
      | "underflow"
      | "division-by-zero"
      | "out-of-range"
      | "invalid";
  }
  type XFLResult<T> = Result<T, XFLError>;

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
   * Packed XLS-17 XFL word: mantissa, exponent, raw bits. Not the public
   * scalar. Poison words are not values.
   */
  interface XFLWord {
    readonly [__providerValueBrand]: "XFLWord";
    readonly raw: bigint;
    mantissa(): bigint;
    exponent(): number;
    toDecimal(): XFLDecimal;
  }
  const XFLWord: RuntimeType<XFLWord> & {
    fromRaw(raw: bigint): XFLResult<XFLWord>;
    /**
     * Compatibility boundary for ported C: admits exactly the words the
     * C `> 0` predicate accepts, preserving noncanonical positive words
     * as-is. For ported semantics only — new code uses `fromRaw`.
     */
    fromRawCompat(raw: bigint): XFLResult<XFLWord>;
    fromDecimal(value: XFLDecimal): XFLWord;
  };

  /**
   * Immutable profile-bound arithmetic. Local override of the artifact
   * profile: `XFLMath.nearestEvenV1.add(left, right)`.
   */
  const XFLMath: RuntimeType<XFLMath> & {
    readonly xahauFloatV1: XFLMath;
    readonly nearestEvenV1: XFLMath;
    for(profile: XFLProfile): XFLMath;
  };
  interface XFLMath {
    readonly [__providerValueBrand]: "XFLMath";
    readonly profile: XFLProfile;
    add(left: XFLDecimal, right: XFLDecimal): XFLResult<XFLDecimal>;
    subtract(left: XFLDecimal, right: XFLDecimal): XFLResult<XFLDecimal>;
    multiply(left: XFLDecimal, right: XFLDecimal): XFLResult<XFLDecimal>;
    divide(left: XFLDecimal, right: XFLDecimal): XFLResult<XFLDecimal>;
    invert(value: XFLDecimal): XFLResult<XFLDecimal>;
    multiplyRatio(
      value: XFLDecimal,
      opts: {
        readonly numerator: number;
        readonly denominator: number;
        readonly roundUp?: boolean;
      },
    ): XFLResult<XFLDecimal>;
    log(value: XFLDecimal): XFLResult<XFLDecimal>;
    root(value: XFLDecimal, degree: number): XFLResult<XFLDecimal>;
  }

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
    add(other: XFLDecimal): XFLResult<XFLDecimal>;
    subtract(other: XFLDecimal): XFLResult<XFLDecimal>;
    multiply(other: XFLDecimal): XFLResult<XFLDecimal>;
    divide(other: XFLDecimal): XFLResult<XFLDecimal>;
    negate(): XFLDecimal;
    invert(): XFLResult<XFLDecimal>;
    multiplyRatio(opts: {
      readonly numerator: number;
      readonly denominator: number;
      readonly roundUp?: boolean;
    }): XFLResult<XFLDecimal>;
    log(): XFLResult<XFLDecimal>;
    root(degree: number): XFLResult<XFLDecimal>;
    /**
     * C's `float_int` as a domain Result: the value times
     * 10^`decimalPlaces` (default 0), truncated toward zero to an
     * integer — `decimalPlaces: 6` turns an XFL amount into micro
     * units. Fails when the scaled result exceeds the 16-significant-
     * decimal-digit XFL result domain (max 9999999999999999) or when
     * the value is negative — C's float_int cannot return a negative;
     * the ratified posture for C's `absolute` flag (0055, 0060) is an
     * explicit `.negate()` at the site, never an option here.
     */
    toInt(decimalPlaces?: number): XFLResult<bigint>;
    toString(): string;
    equals(other: unknown): boolean;
    compare(other: XFLDecimal): -1 | 0 | 1;
  }

  interface XFLDecimalFactory extends RuntimeType<XFLDecimal> {
    readonly zero: XFLDecimal;
    readonly one: XFLDecimal;
    /**
     * Construct `mantissa × 10^exponent`. The value comes first so ordinary
     * calls read in the same order as the decimal quantity they express.
     */
    from(mantissa: bigint | number, exponent?: number): XFLResult<XFLDecimal>;
  }

  const XFLDecimal: XFLDecimalFactory;

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

  interface AmountFactory extends RuntimeType<Amount> {
    from(value: BytesLike | STBlob): Amount;
    drops(value: Drops): NativeAmount;
    /**
     * Encode an issued amount from an existing scalar and issue. A non-IOU
     * issue is an ordinary local encoding failure, never a host failure.
     */
    iou(value: XFLDecimal, issue: Issue): Result<IOUAmount, EncodeError>;
    iou(value: XFLDecimal, currency: Currency, issuer: AccountID): IOUAmount;
    mpt(value: bigint, mptIssuanceId: Hash192): MPTAmount;
  }

  const Amount: AmountFactory;

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
  class STObject {
    protected constructor();
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

  /** @serial Transaction */
  class Transaction extends STObject {
    protected constructor();

    readonly TransactionType: TransactionType;
    readonly Flags: UInt32;
    readonly SourceTag?: UInt32;
    readonly Account: AccountID;
    readonly Sequence: UInt32;
    readonly PreviousTxnID?: Hash256;
    readonly LastLedgerSequence?: UInt32;
    readonly AccountTxnID?: Hash256;
    readonly Fee: Amount;
    readonly OperationLimit?: UInt32;
    readonly Memos?: STArray;
    readonly SigningPubKey: STBlob;
    readonly TicketSequence?: UInt32;
    readonly TxnSignature?: STBlob;
    readonly Signers?: STArray;
    readonly EmitDetails?: STObject;
    readonly FirstLedgerSequence?: UInt32;
    readonly NetworkID?: UInt32;
    readonly HookParameters?: STArray;
    readonly HookName?: STBlob;
  }

  /** Format-proven Transaction narrowed to Payment. */
  class Payment extends Transaction {
    private constructor();

    readonly Destination: AccountID;
    readonly Amount: Amount;
    readonly SendMax?: Amount;
    readonly Paths?: PathSet;
    readonly InvoiceID?: Hash256;
    readonly DestinationTag?: UInt32;
    readonly DeliverMin?: Amount;
    readonly CredentialIDs?: Vector256;
    readonly TransactionType: typeof TransactionType.Payment;
  }

  /** Format-proven ledger-entry base. */
  class LedgerEntry extends STObject {
    protected constructor();

    readonly LedgerIndex?: Hash256;
    readonly LedgerEntryType: LedgerEntryType;
    readonly Flags: UInt32;
    readonly Remarks?: STArray;
  }

  /** Format-proven LedgerEntry narrowed to AccountRoot. */
  class AccountRoot extends LedgerEntry {
    private constructor();

    readonly Account: AccountID;
    readonly Sequence: UInt32;
    readonly Balance: NativeAmount;
    readonly OwnerCount: UInt32;
    readonly PreviousTxnID: Hash256;
    readonly PreviousTxnLgrSeq: UInt32;
    readonly AccountTxnID?: Hash256;
    readonly RegularKey?: AccountID;
    readonly EmailHash?: Hash128;
    readonly WalletLocator?: Hash256;
    readonly WalletSize?: UInt32;
    readonly MessageKey?: STBlob;
    readonly TransferRate?: UInt32;
    readonly Domain?: STBlob;
    readonly TickSize?: UInt8;
    readonly TicketCount?: UInt32;
    readonly NFTokenMinter?: AccountID;
    readonly MintedNFTokens?: UInt32;
    readonly BurnedNFTokens?: UInt32;
    readonly HookStateCount?: UInt32;
    readonly HookNamespaces?: Vector256;
    readonly RewardLgrFirst?: LedgerSequence;
    readonly RewardLgrLast?: LedgerSequence;
    readonly RewardTime?: RippleTime;
    readonly RewardAccumulator?: UInt64;
    readonly FirstNFTokenSequence?: UInt32;
    readonly ImportSequence?: UInt32;
    readonly GovernanceFlags?: Hash256;
    readonly GovernanceMarks?: Hash256;
    readonly AccountIndex?: UInt64;
    readonly TouchCount?: UInt64;
    readonly HookStateScale?: UInt16;
    readonly Cron?: Hash256;
    readonly AMMID?: Hash256;
    readonly LedgerEntryType: typeof LedgerEntryType.AccountRoot;
  }

  /** Format-proven LedgerEntry narrowed to a URI token. */
  class URIToken extends LedgerEntry {
    private constructor();

    readonly Owner: AccountID;
    readonly OwnerNode: UInt64;
    readonly Issuer: AccountID;
    readonly URI: STBlob;
    readonly Digest?: Hash256;
    readonly Amount?: Amount;
    readonly Destination?: AccountID;
    readonly PreviousTxnID: Hash256;
    readonly PreviousTxnLgrSeq: UInt32;
    readonly LedgerEntryType: typeof LedgerEntryType.URIToken;
  }

  /** Installed Hook object held inside a Hook ledger entry's `Hooks` array. */
  interface InstalledHook extends STObject {
    readonly HookHash?: Hash256;
  }

  /** Serialized-array wrapper for one installed Hook object. */
  interface HookArrayEntry extends STObject {
    readonly Hook: InstalledHook;
  }

  /** Account-level ledger entry containing its fixed-position Hook array. */
  interface HookLedger extends STObject {
    readonly LedgerEntryType: "Hook";
    readonly Hooks: STArray<HookArrayEntry>;
  }

  /** Ledger entry containing one installed Hook implementation. */
  interface HookDefinition extends STObject {
    readonly LedgerEntryType: "HookDefinition";
    readonly HookHash: Hash256;
  }

  interface ActiveValidator extends STObject {
    readonly PublicKey: STBlob;
    readonly Account?: AccountID;
  }

  interface ActiveValidatorArrayEntry extends STObject {
    readonly ActiveValidator: ActiveValidator;
  }

  interface UNLReport extends STObject {
    readonly LedgerEntryType: "UNLReport";
    readonly ActiveValidators?: STArray<ActiveValidatorArrayEntry>;
  }

  interface NFToken extends STObject {
    readonly NFTokenID: Hash256;
    readonly URI?: STBlob;
  }

  /** @serial Metadata */
  interface TxMeta extends STObject {
    readonly TransactionResult: TransactionResult;

    /** Find an affected NFToken by ID, including a token removed by a burn. */
    findNFToken(id: Hash256): NFToken | undefined;
  }

  interface XPop {
    readonly transaction: Transaction;
    readonly metadata: TxMeta;
  }

  /**
   * Host-backed object. Bytes still live on the host; each named field and
   * `get` is a crossing. This is not `STObject`: after mint, getters are total.
   * Nested objects are further `HostObject` subclasses
   * (`instanceof HostObject` and `instanceof HostPayment`). The provider
   * constructs them; there is no public constructor.
   */
  interface HostObject {
    readonly [__providerValueBrand]: "HostObject";
    get<T>(field: SerializedField<T>): HostResult<T | undefined>;
    get(field: string): HostResult<SerializedValue>;
    materialize(): HostResult<STObject>;
  }

  /**
   * Host-backed STArray. The wire form is terminator-delimited, not counted:
   * `count()` walks, `at(i)` is O(i) from the start. `forEach` is the cheap
   * traversal — one walk, one error channel.
   */
  /** Host-view runtime nouns (0085 close). */
  const HostObject: RuntimeType<HostObject>;
  const HostArray: RuntimeType<HostArray<HostObject>>;
  const HostTx: RuntimeType<HostTx>;
  const HostPayment: RuntimeType<HostPayment>;
  const HostAccountRoot: RuntimeType<HostAccountRoot>;
  const HostInstalledHook: RuntimeType<HostInstalledHook>;
  const HostHookArrayEntry: RuntimeType<HostHookArrayEntry>;
  const HostHookLedger: RuntimeType<HostHookLedger>;
  const HostHookDefinition: RuntimeType<HostHookDefinition>;
  const HostActiveValidator: RuntimeType<HostActiveValidator>;
  const HostActiveValidatorArrayEntry: RuntimeType<HostActiveValidatorArrayEntry>;
  const HostUNLReport: RuntimeType<HostUNLReport>;
  const HostNFToken: RuntimeType<HostNFToken>;
  const HostTxMeta: RuntimeType<HostTxMeta>;

  interface HostArray<E extends HostObject = HostObject> {
    forEach(body: (element: E, index: number) => void): HostVoidResult;
    /** Walks the array. Not a field. */
    count(): HostResult<number>;
    /** O(index) scan from the start. Prefer `forEach`. */
    at(index: number): HostResult<E | undefined>;
  }

  interface HostTx extends HostObject {
    readonly TransactionType: HostResult<TransactionType>;
    readonly Account: HostResult<AccountID>;
    readonly Destination: HostResult<AccountID | undefined>;
    readonly Amount: HostResult<Amount | undefined>;
    readonly Amounts: HostResult<HostArray | undefined>;
    readonly Fee: HostResult<Amount | undefined>;
    readonly Flags: HostResult<UInt32>;
    readonly Sequence: HostResult<UInt32 | undefined>;
    readonly Blob: HostResult<STBlob | undefined>;
    readonly NFTokenID: HostResult<Hash256 | undefined>;
    readonly HookParameters: HostResult<HostArray | undefined>;
    materialize(): HostResult<Transaction>;
    /**
     * Narrow to Payment after reading `TransactionType`. One crossing.
     * Full per-format leaves are 0001.
     */
    asPayment(): HostResult<HostPayment>;
  }

  interface HostPayment extends HostObject {
    readonly TransactionType: typeof TransactionType.Payment;
    readonly Account: HostResult<AccountID>;
    readonly Destination: HostResult<AccountID>;
    readonly Amount: HostResult<Amount>;
    readonly Amounts: HostResult<HostArray | undefined>;
    readonly Fee: HostResult<Amount | undefined>;
    readonly Flags: HostResult<UInt32>;
    readonly Sequence: HostResult<UInt32 | undefined>;
    readonly Blob: HostResult<STBlob | undefined>;
    readonly NFTokenID: HostResult<Hash256 | undefined>;
    readonly HookParameters: HostResult<HostArray | undefined>;
    materialize(): HostResult<Payment>;
  }

  interface HostAccountRoot extends HostObject {
    readonly LedgerEntryType: "AccountRoot";
    readonly Account: HostResult<AccountID>;
    readonly Balance: HostResult<NativeAmount>;
    readonly Flags: HostResult<UInt32>;
    readonly ImportSequence: HostResult<UInt32 | undefined>;
    readonly RewardAccumulator: HostResult<UInt64 | undefined>;
    readonly RewardLgrFirst: HostResult<LedgerSequence | undefined>;
    readonly RewardLgrLast: HostResult<LedgerSequence | undefined>;
    readonly RewardTime: HostResult<RippleTime | undefined>;
    readonly Sequence: HostResult<UInt32>;
    readonly OwnerCount: HostResult<UInt32>;
    readonly PreviousTxnID: HostResult<Hash256>;
    readonly PreviousTxnLgrSeq: HostResult<UInt32>;
    readonly AccountTxnID: HostResult<Hash256 | undefined>;
    readonly RegularKey: HostResult<AccountID | undefined>;
    readonly EmailHash: HostResult<Hash128 | undefined>;
    readonly WalletLocator: HostResult<Hash256 | undefined>;
    readonly WalletSize: HostResult<UInt32 | undefined>;
    readonly MessageKey: HostResult<STBlob | undefined>;
    readonly TransferRate: HostResult<UInt32 | undefined>;
    readonly Domain: HostResult<STBlob | undefined>;
    readonly TickSize: HostResult<UInt8 | undefined>;
    readonly TicketCount: HostResult<UInt32 | undefined>;
    readonly NFTokenMinter: HostResult<AccountID | undefined>;
    readonly MintedNFTokens: HostResult<UInt32 | undefined>;
    readonly BurnedNFTokens: HostResult<UInt32 | undefined>;
    readonly HookStateCount: HostResult<UInt32 | undefined>;
    readonly FirstNFTokenSequence: HostResult<UInt32 | undefined>;
    readonly GovernanceFlags: HostResult<Hash256 | undefined>;
    readonly GovernanceMarks: HostResult<Hash256 | undefined>;
    readonly AccountIndex: HostResult<UInt64 | undefined>;
    readonly TouchCount: HostResult<UInt64 | undefined>;
    readonly HookStateScale: HostResult<UInt16 | undefined>;
    readonly Cron: HostResult<Hash256 | undefined>;
    readonly AMMID: HostResult<Hash256 | undefined>;
    readonly LedgerIndex: HostResult<Hash256 | undefined>;
    readonly Remarks: HostResult<HostArray | undefined>;
    materialize(): HostResult<AccountRoot>;
  }

  interface HostInstalledHook extends HostObject {
    readonly HookHash: HostResult<Hash256 | undefined>;
    materialize(): HostResult<InstalledHook>;
  }

  interface HostHookArrayEntry extends HostObject {
    readonly Hook: HostResult<HostInstalledHook>;
    materialize(): HostResult<HookArrayEntry>;
  }

  interface HostHookLedger extends HostObject {
    readonly LedgerEntryType: "Hook";
    readonly Hooks: HostResult<HostArray<HostHookArrayEntry>>;
    materialize(): HostResult<HookLedger>;
  }

  interface HostHookDefinition extends HostObject {
    readonly LedgerEntryType: "HookDefinition";
    readonly HookHash: HostResult<Hash256>;
    materialize(): HostResult<HookDefinition>;
  }

  interface HostActiveValidator extends HostObject {
    readonly PublicKey: HostResult<STBlob>;
    readonly Account: HostResult<AccountID | undefined>;
    materialize(): HostResult<ActiveValidator>;
  }

  interface HostActiveValidatorArrayEntry extends HostObject {
    readonly ActiveValidator: HostResult<HostActiveValidator>;
    materialize(): HostResult<ActiveValidatorArrayEntry>;
  }

  interface HostUNLReport extends HostObject {
    readonly LedgerEntryType: "UNLReport";
    readonly ActiveValidators: HostResult<HostArray<HostActiveValidatorArrayEntry> | undefined>;
    materialize(): HostResult<UNLReport>;
  }

  interface HostNFToken extends HostObject {
    readonly NFTokenID: HostResult<Hash256>;
    readonly URI: HostResult<STBlob | undefined>;
    materialize(): HostResult<NFToken>;
  }

  interface HostTxMeta extends HostObject {
    readonly TransactionResult: HostResult<TransactionResult>;
    materialize(): HostResult<TxMeta>;
  }

  const TransactionType: TransactionTypeEnum;
  type TransactionType = (typeof TransactionType)[keyof typeof TransactionType];

  const LedgerEntryType: LedgerEntryTypeEnum;
  type LedgerEntryType = (typeof LedgerEntryType)[keyof typeof LedgerEntryType];

  const TransactionResult: TransactionResultEnum;
  type TransactionResult = (typeof TransactionResult)[keyof typeof TransactionResult];

  /**
   * Bit flags for transaction `Flags` (`tf*` / `hsf*`).
   *
   * Flattened across types. Shared names are the same bit; different names
   * often share a value (`tfRequireDestTag` and `tfLPToken` are both 65536).
   * The name is only meaningful for the object you are inspecting. `Flags`
   * stays `UInt32` — do not type that field as this enum.
   */
  const enum TransactionFlag {
    tfTestSuite = 2147483648,
    tfFullyCanonicalSig = 2147483648,
    tfTwoAssetIfEmpty = 8388608,
    tfClearDeepFreeze = 8388608,
    tfLimitLPToken = 4194304,
    tfSetDeepFreeze = 4194304,
    tfOneAssetLPToken = 2097152,
    tfAllowXRP = 2097152,
    tfClearFreeze = 2097152,
    tfTwoAsset = 1048576,
    tfDisallowXRP = 1048576,
    tfSetFreeze = 1048576,
    tfSingleAsset = 524288,
    tfOptionalAuth = 524288,
    tfSell = 524288,
    tfOneAssetWithdrawAll = 262144,
    tfRequireAuth = 262144,
    tfFillOrKill = 262144,
    tfLimitQuality = 262144,
    tfClearNoRipple = 262144,
    tfWithdrawAll = 131072,
    tfOptionalDestTag = 131072,
    tfLostMajority = 131072,
    tfImmediateOrCancel = 131072,
    tfPartialPayment = 131072,
    tfClose = 131072,
    tfSetNoRipple = 131072,
    tfLPToken = 65536,
    tfRequireDestTag = 65536,
    tfGotMajority = 65536,
    tfPassive = 65536,
    tfNoRippleDirect = 65536,
    tfRenew = 65536,
    tfSetfAuth = 65536,
    tfClearAccountCreateAmount = 65536,
    tfStrongTSH = 32768,
    tfMPTCanClawback = 64,
    tfMPTCanTransfer = 32,
    tfMPTCanTrade = 16,
    tfMutable = 16,
    tfMPTCanEscrow = 8,
    tfTransferable = 8,
    tfMPTRequireAuth = 4,
    tfTrustLine = 4,
    hsfCOLLECT = 4,
    tfMPTCanLock = 2,
    tfMPTUnlock = 2,
    tfOnlyXRP = 2,
    hsfNSDELETE = 2,
    tfClawTwoAssets = 1,
    tfOptOut = 1,
    tfCronUnset = 1,
    tfMPTUnauthorize = 1,
    tfMPTLock = 1,
    tfSellNFToken = 1,
    tfBurnable = 1,
    hsfOVERRIDE = 1,
    tfImmutable = 1,
  }

  /**
   * Bit flags for ledger-entry `Flags` (`lsf*`).
   *
   * Flattened across entry types. Shared names are the same bit; different
   * names often share a value. The name is only meaningful for the object
   * you are inspecting. `Flags` stays `UInt32`.
   */
  const enum LedgerFlag {
    lsfDisallowIncomingRemit = 2147483648,
    lsfURITokenIssuer = 1073741824,
    lsfDisallowIncomingTrustline = 536870912,
    lsfDisallowIncomingPayChan = 268435456,
    lsfDisallowIncomingCheck = 134217728,
    lsfDisallowIncomingNFTokenOffer = 67108864,
    lsfHighDeepFreeze = 67108864,
    lsfTshCollect = 33554432,
    lsfLowDeepFreeze = 33554432,
    lsfDepositAuth = 16777216,
    lsfAMMNode = 16777216,
    lsfDefaultRipple = 8388608,
    lsfHighFreeze = 8388608,
    lsfGlobalFreeze = 4194304,
    lsfLowFreeze = 4194304,
    lsfNoFreeze = 2097152,
    lsfHighNoRipple = 2097152,
    lsfDisableMaster = 1048576,
    lsfLowNoRipple = 1048576,
    lsfDisallowXRP = 524288,
    lsfHighAuth = 524288,
    lsfRequireAuth = 262144,
    lsfLowAuth = 262144,
    lsfRequireDestTag = 131072,
    lsfSell = 131072,
    lsfHighReserve = 131072,
    lsfPasswordSpent = 65536,
    lsfAccepted = 65536,
    lsfPassive = 65536,
    lsfLowReserve = 65536,
    lsfOneOwnerCount = 65536,
    lsfAllowTrustLineClawback = 4096,
    lsfMPTCanClawback = 64,
    lsfMPTCanTransfer = 32,
    lsfMPTCanTrade = 16,
    lsfMPTCanEscrow = 8,
    lsfEmittedDir = 4,
    lsfMPTRequireAuth = 4,
    lsfNFTokenSellOffers = 2,
    lsfMPTAuthorized = 2,
    lsfMPTCanLock = 2,
    lsfNFTokenBuyOffers = 1,
    lsfMPTLocked = 1,
    lsfSellNFToken = 1,
    lsfBurnable = 1,
  }

  /** AccountSet `SetFlag` / `ClearFlag` ordinals (`asf*`). These are not bitmasks. */
  const enum AccountSetFlag {
    asfAllowTrustLineClawback = 17,
    asfDisallowIncomingRemit = 16,
    asfDisallowIncomingTrustline = 15,
    asfDisallowIncomingPayChan = 14,
    asfDisallowIncomingCheck = 13,
    asfDisallowIncomingNFTokenOffer = 12,
    asfTshCollect = 11,
    asfAuthorizedNFTokenMinter = 10,
    asfDepositAuth = 9,
    asfDefaultRipple = 8,
    asfGlobalFreeze = 7,
    asfNoFreeze = 6,
    asfAccountTxnID = 5,
    asfDisableMaster = 4,
    asfDisallowXRP = 3,
    asfRequireAuth = 2,
    asfRequireDest = 1,
  }

  const enum HookExecutionMode {
    /** Strong pre-apply execution. */
    Strong = "strong",

    /**
     * Weak post-apply execution. Xahau has no `hefWEAK` symbol: this is the
     * execution state with neither `hefSTRONG` nor `hefCALLBACK` set.
     */
    Weak = "weak",

    /** Again-as-weak execution requested by a preceding strong pass. */
    Again = "again",

    /** Emitted-transaction callback execution. */
    Callback = "callback",
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

    /**
     * Correlation id for this callback's transaction. When `applied`, the
     * originating transaction id. When not applied, `sfEmittedTxnID` when
     * present, else the originating id when available. Returns `undefined`
     * when neither source is available.
     */
    emittedTransactionId(): Hash256 | undefined;
  }

  /**
   * Typed ledger locator. `T` is erased at runtime and records the minted
   * ledger-object shape for `ledger.lookup`. `ledger.get` returns the matching
   * `HostObject` subtype.
   */
  interface LedgerKeylet<T extends STObject = STObject> {
    readonly [__providerValueBrand]: "LedgerKeylet";
    readonly [__ledgerKeyletValueBrand]?: T;
    readonly byteLength: 34;
    readonly type: number;
    toBytes(): Uint8Array;
    toHex(): HexString;
  }
  /** Typed-locator runtime noun (0085 close shape). */
  const LedgerKeylet: RuntimeType<LedgerKeylet> & {
    /** Import a raw 34-byte locator carrying no minted object-type claim. */
    fromRaw(value: BytesLike | STBlob): ParseResult<LedgerKeylet>;
  };

  namespace otxn {
    function raw(): HostResult<STBlob>;
    /**
     * Minted originating transaction. Total getters. Existence is an
     * execution invariant.
     */
    function object(): Transaction;
    /**
     * Host-backed originating transaction. Field reads are `HostResult`.
     * The handle itself is total.
     */
    function hostObject(): HostTx;
    function type(): TransactionType;
    function id(flags?: number): HostResult<Hash256>;
    function generation(): HostResult<number>;
    function burden(): HostResult<bigint>;
    /**
     * Transaction-carried hook parameters (`otxn_param`). Names and values
     * are blobs. Absent and empty are the same host status (`DOESNT_EXIST`)
     * and both surface as `undefined`. At most 16 parameters, key ≤ 32
     * bytes, value ≤ 256 bytes — same numeric caps as install-time params,
     * but the 16 is per originating transaction, not per hook.
     */
    function param(name: StateKeyLike): HostResult<STBlob | undefined>;
    function param<T>(name: StateKeyLike, schema: BinarySchema<T>): SchemaReadResult<T>;
    /** Policied read: disposition declared at the site (`ReadPolicies`). */
    function param<T, const P extends ReadPolicies>(
      name: StateKeyLike,
      schema: BinarySchema<T>,
      policies: P,
    ): PolicyRead<T, P>;
    function params(names: readonly StateKeyLike[]): HostResult<readonly (STBlob | undefined)[]>;
    function params<const T extends BatchKeys>(names: T): HostResult<BatchValues<T>>;
    /**
     * Schema batch. Object key is the wire name; value is a codec.
     * One host Result for the crossing; on success the record maps
     * EVERY key to its own `BatchOutcome`: missing → ok(`undefined`),
     * malformed → its own `ParseError` (`issue: "invalid-field"`,
     * `field` = that key), so a valid field is consumed beside a
     * malformed sibling. Malformed must not become absent.
     *
     * Blob batch is `{ localAlias: wireName }` because both sides are names.
     * Schema batch is `{ wireName: codec }` because the value is not a name.
     * Quoted keys cover any UTF-8 name (`"FEE-BPS"`). Singular
     * `param(name, schema)` is for names that are not valid UTF-8
     * (`BytesLike` wire names that cannot round-trip through a JS string).
     */
    function params<
      const T extends { readonly [K: string]: BatchSchemaField },
    >(fields: T): HostResult<BatchOutcomes<T>>;
    /**
     * Minted originating-transaction metadata, if the host supplied it.
     */
    function metaObject(): HostResult<TxMeta | undefined>;
    function metaHostObject(): HostResult<HostTxMeta | undefined>;
    function xpop(): HostResult<XPop | undefined>;
  }

  namespace state {
    /**
     * Key layout. `length` is the final key width in bytes (state keys
     * are 32). Padding fills with ZERO bytes (0x00): `"right"` appends
     * zeros after the payload — payload left-aligned, the C idiom
     * `uint8_t k[32] = {0}; k[0] = tag;` — and `"left"` prepends them
     * (payload right-aligned). `"none"` demands the parts already
     * total exactly `length` bytes; a mismatch throws (programmer
     * error, like a malformed `cell` name). Parts longer than `length`
     * always throw; padding never truncates.
     */
    interface KeyOptions {
      readonly length?: number;
      readonly pad?: "left" | "right" | "none";
    }

    interface Put {
      readonly key: StateKeyLike;
      readonly value?: StateValueLike;
    }

    interface ForeignAccessor {
      get(key: StateKeyLike): HostResult<STBlob | undefined>;
      /**
       * Opt-in exact-capacity read. Host `TOO_SMALL` remains a host failure;
       * short successful values and malformed exact-size values are parse
       * failures, and missing state remains successful `undefined`.
       */
      get<T>(key: StateKeyLike, schema: BinarySchema<T>): StateReadResult<T>;
      set(key: StateKeyLike, value: StateValueLike): HostVoidResult;
      del(key: StateKeyLike): HostVoidResult;
    }

    /** Broad schema/batch intent layered over the implemented blob accessor. */
    interface ForeignSchemaAccessor extends ForeignAccessor {
      get(key: StateKeyLike): HostResult<STBlob | undefined>;
      get<T>(key: StateKeyLike, schema: BinarySchema<T>): StateReadResult<T>;
      /** Policied read: disposition declared at the site (`ReadPolicies`). */
      get<T, const P extends ReadPolicies>(
        key: StateKeyLike,
        schema: BinarySchema<T>,
        policies: P,
      ): PolicyRead<T, P>;
      getMany(keys: readonly StateKeyLike[]): HostResult<readonly (STBlob | undefined)[]>;
      getMany<const T extends BatchKeys>(keys: T): HostResult<BatchValues<T>>;
    }

    /** String parts are encoded as UTF-8 state-key text. */
    function key(part: string | BytePart, options?: KeyOptions): STBlob;
    function key(parts: readonly (string | BytePart)[], options?: KeyOptions): STBlob;
    function get(key: string | BytesLike | STBlob | Hash256 | AccountID): HostResult<STBlob | undefined>;
    function get(key: StateKeyLike): HostResult<STBlob | undefined>;
    function get<T>(key: StateKeyLike, schema: BinarySchema<T>): StateReadResult<T>;
    /** Policied read: disposition declared at the site (`ReadPolicies`). */
    function get<T, const P extends ReadPolicies>(
      key: StateKeyLike,
      schema: BinarySchema<T>,
      policies: P,
    ): PolicyRead<T, P>;
    function getMany(keys: readonly StateKeyLike[]): HostResult<readonly (STBlob | undefined)[]>;
    function getMany<const T extends BatchKeys>(keys: T): HostResult<BatchValues<T>>;
    function set(
      key: string | BytesLike | STBlob | Hash256 | AccountID,
      value: string | BytesLike | STBlob | Hash256 | AccountID,
    ): HostVoidResult;
    function set(key: StateKeyLike, value: StateValueLike): HostVoidResult;
    function del(key: string | BytesLike | STBlob | Hash256 | AccountID): HostVoidResult;
    function del(key: StateKeyLike): HostVoidResult;
    function setMany(items: readonly Put[]): HostVoidResult;
    /**
     * Captured accessor over another account's namespaced state. Constructing
     * it is local and total; each read or grant-gated mutation owns its host
     * crossing and preserves the exact host status.
     */
    function foreign(account: AccountID, namespace: Hash256): ForeignSchemaAccessor;
    function foreign(account: AccountID, namespace: Hash256): ForeignAccessor;
  }

  namespace emit {
    interface EmittedTransaction {
      readonly [__providerValueBrand]: void;
      readonly blob: STBlob;
      readonly kind: TransactionType;
    }

    /**
     * Failed build stage. `"details"` and `"fee"` are host stages while
     * finalizing an emission; `"encode"` is the builder refusing malformed
     * local arguments before any host crossing.
     */
    type BuildError =
      | (EncodeError & { readonly stage: "encode" })
      | (HostError & { readonly stage: "details" | "fee" });
    type BuildResult = Result<EmittedTransaction, BuildError>;

    /**
     * Serialized transaction flags. A list is folded with bitwise OR; builders
     * may add protocol-required infrastructure flags.
     */
    type TransactionFlags =
      | UInt32
      | TransactionFlag
      | readonly TransactionFlag[];

    interface HookParameter {
      readonly HookParameterName: StateKeyLike;
      readonly HookParameterValue: StateValueLike;
    }

    interface HookGrant {
      readonly HookHash: Hash256;
      readonly Authorize: AccountID;
    }

    /**
     * Selected typed emitted-transaction builders.
     *
     * This is not yet the complete Xahau transaction catalogue. Every omitted
     * transaction type must eventually be projected or carry an explicit
     * unsupported/not-emittable disposition; see the builder coverage gate.
     */
    namespace build {
      interface InvokeOptions {
        /** Sending account (0087 wave-1: emitted Invokes set it in C). */
        readonly Account?: AccountID;
        readonly Destination?: AccountID;
        readonly HookParameters?: readonly HookParameter[];
        readonly Blob?: StateValueLike;
      }

      interface HookReference {
        /**
         * Hook-chain slot to override. The builder orders entries by `$position`,
         * fills omitted lower positions with canonical no-op Hook objects, and
         * serializes `Flags = tfHookOverride` for this action.
         */
        readonly $position: number;
        readonly HookHash: Hash256;
        readonly HookNamespace?: Hash256;
        readonly HookParameters?: readonly HookParameter[];
        readonly HookGrants?: readonly HookGrant[];
      }

      interface HookDeletion {
        /**
         * Delete one chain slot. `$delete: true` serializes the canonical
         * override/delete object: `Flags = tfHookOverride`, zero-length
         * CreateCode, and no HookHash. It is never a zero Hash256.
         */
        readonly $position: number;
        readonly $delete: true;
      }

      type HookSetEntry = HookReference | HookDeletion;

      interface HookSetOptions {
        readonly Account?: AccountID;
        readonly Flags?: TransactionFlags;
        readonly HookParameters?: readonly HookParameter[];
        /** Unique in-range position actions; at least one is required. */
        readonly Hooks: readonly HookSetEntry[];
      }

      interface PaymentOptions {
        /** Sending account (0087 wave-1: emitted Payments set it in C). */
        readonly Account?: AccountID;
        readonly Destination: AccountID;
        readonly Amount: Amount;
        readonly SourceTag?: UInt32;
        readonly DestinationTag?: UInt32;
        /** The builder always includes `TransactionFlag.tfFullyCanonicalSig`. */
        readonly Flags?: TransactionFlags;
        readonly InvoiceID?: Hash256;
        readonly SendMax?: Amount;
        readonly DeliverMin?: Amount;
        readonly HookParameters?: readonly HookParameter[];
      }

      /** Build an OfferCreate for direct DEX placement. */
      interface OfferCreateOptions {
        readonly Account?: AccountID;
        readonly TakerPays: Amount;
        readonly TakerGets: Amount;
        readonly Expiration?: RippleTime;
        readonly Flags?: TransactionFlags;
        readonly HookParameters?: readonly HookParameter[];
      }

      /** Build a TrustSet for trustline limits, qualities, and flags. */
      interface TrustSetOptions {
        readonly Account?: AccountID;
        readonly LimitAmount?: Amount;
        readonly QualityIn?: UInt32;
        readonly QualityOut?: UInt32;
        readonly Flags?: TransactionFlags;
        readonly HookParameters?: readonly HookParameter[];
      }

      interface RemitOptions {
        /** Sending account (0070:286, 0084:161-169). */
        readonly Account?: AccountID;
        readonly Destination: AccountID;
        readonly MintURIToken?: StateValueLike;
        /**
         * An EMPTY amounts array is refused locally at BUILD time with stage
         * "encode" — not deferred to emit (0070:294-297, 0084:161-169).
         */
        readonly Amounts?: readonly Amount[];
        readonly SourceTag?: UInt32;
        readonly DestinationTag?: UInt32;
        readonly Flags?: TransactionFlags;
        readonly HookParameters?: readonly HookParameter[];
      }

      interface ClaimRewardOptions {
        readonly Account?: AccountID;
        readonly Issuer: AccountID;
        readonly Flags?: TransactionFlags;
        readonly HookParameters?: readonly HookParameter[];
      }

      interface SignerEntry {
        readonly Account: AccountID;
        readonly SignerWeight: UInt16;
      }

      interface SignerListSetOptions {
        readonly Account?: AccountID;
        readonly SignerQuorum: UInt32;
        readonly SignerEntries: readonly SignerEntry[];
        readonly Flags?: TransactionFlags;
        readonly HookParameters?: readonly HookParameter[];
      }

      interface URITokenMintOptions {
        readonly Account?: AccountID;
        readonly Destination?: AccountID;
        readonly URI: StateValueLike;
        readonly Amount?: Amount;
        readonly Digest?: Hash256;
        readonly Flags?: TransactionFlags;
        readonly HookParameters?: readonly HookParameter[];
      }

      interface GenesisMintBaseOptions {
        readonly Account?: AccountID;
        readonly Flags?: TransactionFlags;
        readonly HookParameters?: readonly HookParameter[];
      }

      interface GenesisMintEntry {
        readonly Account: AccountID;
        readonly Amount: NativeAmount | Drops;
      }

      interface GenesisMintRawOptions extends GenesisMintBaseOptions {
        readonly $rawGenesisMints: StateValueLike;
        readonly GenesisMints?: never;
      }

      interface GenesisMintEntriesOptions extends GenesisMintBaseOptions {
        readonly GenesisMints: readonly GenesisMintEntry[];
        readonly $rawGenesisMints?: never;
      }

      type GenesisMintOptions = GenesisMintRawOptions | GenesisMintEntriesOptions;

      /** Final builders acquire host-derived fee/details and identify a failed stage. */
      function invoke(options: InvokeOptions): BuildResult;
      function hookSet(options: HookSetOptions): BuildResult;
      function payment(options: PaymentOptions): BuildResult;
      function offerCreate(options: OfferCreateOptions): BuildResult;
      function trustSet(options: TrustSetOptions): BuildResult;
      function remit(options: RemitOptions): BuildResult;
      function claimReward(options: ClaimRewardOptions): BuildResult;
      function signerListSet(options: SignerListSetOptions): BuildResult;
      function genesisMint(options: GenesisMintOptions): BuildResult;
      function uriTokenMint(options: URITokenMintOptions): BuildResult;
    }

    /**
     * Reserve the maximum emitted-transaction count before building or
     * submitting. The host owns the one-shot execution-scoped reservation;
     * builders report a missing reservation at their `"details"` stage.
     */
    function reserve(count: number): HostVoidResult;
    function tx(transaction: BytesLike | STBlob): HostResult<Hash256>;
    function tx(transaction: BytesLike | STBlob | EmittedTransaction): HostResult<Hash256>;
    /**
     * Attempt every transaction in input order and retain each individual host
     * result. Use `rollback.onAnyFail` when all emissions are required or
     * `rollback.onAllFail` when partial success is part of the contract.
     */
    function txMany(
      transactions: readonly (BytesLike | STBlob | EmittedTransaction)[],
    ): readonly HostResult<Hash256>[];
    function prepare(partial: BytesLike | STBlob): HostResult<STBlob>;
    function prepare(partial: BytesLike | STBlob | STObject): HostResult<STBlob>;
    function details(): HostResult<STBlob>;
    function feeBase(transaction: BytesLike | STBlob | EmittedTransaction): HostResult<Drops>;
    function nonce(): HostResult<Hash256>;
    function generation(): HostResult<number>;
    function burden(): HostResult<bigint>;
  }

  namespace util {
    namespace keylet {
      function account(account: AccountID): LedgerKeylet<AccountRoot>;
      function uriToken(id: Hash256): LedgerKeylet<URIToken>;
      function hook(account: AccountID): LedgerKeylet<HookLedger>;
      function hookDefinition(hash: Hash256): LedgerKeylet<HookDefinition>;
      function hookState(account: AccountID, key: Hash256, namespace: Hash256): LedgerKeylet;
      function hookStateDir(account: AccountID, namespace: Hash256): LedgerKeylet;
      /** Account order is normalized by the host when deriving the trust-line key. */
      function line(accountA: AccountID, accountB: AccountID, currency: Currency): LedgerKeylet;
      function ownerDir(account: AccountID): LedgerKeylet;
      function signers(account: AccountID): LedgerKeylet;
      function did(account: AccountID): LedgerKeylet;
      function oracle(account: AccountID, sequence: UInt32): LedgerKeylet;
      function offer(account: AccountID, sequence: UInt32OrHash): LedgerKeylet;
      function check(account: AccountID, sequence: UInt32OrHash): LedgerKeylet;
      function escrow(account: AccountID, sequence: UInt32OrHash): LedgerKeylet;
      function nftOffer(account: AccountID, sequence: UInt32OrHash): LedgerKeylet;
      function cron(account: AccountID, sequence: UInt32): LedgerKeylet;
      function paychan(source: AccountID, destination: AccountID, sequence: UInt32OrHash): LedgerKeylet;
      function depositPreauth(owner: AccountID, authorized: AccountID): LedgerKeylet;
      function child(hash: Hash256): LedgerKeylet;
      function emittedTxn(hash: Hash256): LedgerKeylet;
      function unchecked(hash: Hash256): LedgerKeylet;
      function page(hash: Hash256, index: UInt64 | number): LedgerKeylet;
      function quality(directory: LedgerKeylet, quality: UInt64 | number): LedgerKeylet;
      function skip(position?: UInt32): LedgerKeylet;
      function amendments(): LedgerKeylet;
      function fees(): LedgerKeylet;
      function negativeUNL(): LedgerKeylet;
      function emittedDir(): LedgerKeylet;
      function amm(left: Issue, right: Issue): LedgerKeylet;
    }

    function sha512h(data: BytesLike | STBlob): Hash256;
    function verify(publicKey: BytesLike, signature: BytesLike, message: BytesLike | STBlob): boolean;
    /** Concatenate byte parts; string parts are encoded as UTF-8 text. */
    function bytes(...parts: readonly (string | BytePart)[]): STBlob;
    function toRAddress(account: AccountID | BytesLike): string;
    /**
     * Asserts a well-formed r-address (checksummed base58); throws
     * TypeError otherwise. Intended for source literals — no port consumes
     * r-addresses as data; a Result-shaped variant waits until one does.
     */
    function fromRAddress(account: string): AccountID;
    function encodeObject(value: STObject): STBlob;
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
    /** Total base fee for the current ledger view. */
    const feeBase: Drops;
    function nonce(): HostResult<Hash256>;
    function accountRoot(account: AccountID): HostResult<AccountRoot | undefined>;
    function unlReport(): HostResult<UNLReport | undefined>;
    /**
     * Host-backed ledger object. Outer `HostResult` is existence; field
     * reads on the handle are crossings.
     */
    function get(locator: LedgerKeylet<AccountRoot>): HostResult<HostAccountRoot | undefined>;
    function get(locator: LedgerKeylet<HookLedger>): HostResult<HostHookLedger | undefined>;
    function get(locator: LedgerKeylet<HookDefinition>): HostResult<HostHookDefinition | undefined>;
    function get(locator: LedgerKeylet<UNLReport>): HostResult<HostUNLReport | undefined>;
    function get(locator: LedgerKeylet): HostResult<HostObject | undefined>;
    function get(locator: Hash256): HostResult<HostObject | undefined>;
    function lookup(locator: LedgerKeylet<AccountRoot>): HostResult<AccountRoot | undefined>;
    function lookup(locator: LedgerKeylet<URIToken>): HostResult<URIToken | undefined>;
    function lookup<T extends STObject>(locator: LedgerKeylet<T>): HostResult<T | undefined>;
    function lookup(locator: Hash256): HostResult<STObject | undefined>;
    function lookupMany(locators: readonly (LedgerKeylet | Hash256)[]): HostResult<readonly (STObject | undefined)[]>;
    function nextKeylet(lo: LedgerKeylet, hi: LedgerKeylet): HostResult<LedgerKeylet | undefined>;
  }


  /** Metadata and configuration for the currently executing Hook. */
  namespace hook {
    /** Hook account for this invocation; provider construction is total. */
    function account(): AccountID;
    function hash(): HostResult<Hash256>;
    function position(): HostResult<number>;
    function mode(): HookExecutionMode;
    function hashAt(position: number): HostResult<Hash256 | undefined>;
    /**
     * Install-time hook parameters (`hook_param`). Names and values are
     * blobs. Absent and empty are the same host status (`DOESNT_EXIST`)
     * and both surface as `undefined`. At most 16 parameters per installed
     * hook, key ≤ 32 bytes, value ≤ 256 bytes.
     *
     * The value may have been substituted or deleted by an earlier hook in
     * this chain via `paramSet`; it is not necessarily the SetHook
     * install-time blob.
     *
     * The schema overload is the same triage as `state.get`: `!ok` is a
     * host code or a codec issue, `undefined` is absent, otherwise decoded.
     * `schema.byteLength` must be ≤ 256; the provider rejects a larger
     * schema at runtime.
     */
    function param(name: StateKeyLike): HostResult<STBlob | undefined>;
    function param<T>(name: StateKeyLike, schema: BinarySchema<T>): SchemaReadResult<T>;
    /** Policied read: disposition declared at the site (`ReadPolicies`). */
    function param<T, const P extends ReadPolicies>(
      name: StateKeyLike,
      schema: BinarySchema<T>,
      policies: P,
    ): PolicyRead<T, P>;
    function params(names: readonly StateKeyLike[]): HostResult<readonly (STBlob | undefined)[]>;
    function params<const T extends BatchKeys>(names: T): HostResult<BatchValues<T>>;
    /**
     * Schema batch. Object key is the wire name; value is a codec.
     * Product law as on `otxn.params`: one host Result, then per-key
     * `BatchOutcome`s — missing is ok(`undefined`), malformed carries
     * its own `ParseError` naming `field` as that key. Same
     * 16-per-hook cap as the blob batch. Quoted keys cover any UTF-8
     * name; singular `param` is for non-UTF-8 `BytesLike` names.
     */
    function params<
      const T extends { readonly [K: string]: BatchSchemaField },
    >(fields: T): HostResult<BatchOutcomes<T>>;
    function paramSet(targetHook: Hash256, name: StateKeyLike, value: BytesLike): HostVoidResult;
    function skip(targetHook: Hash256, remove?: boolean): HostVoidResult;
    /**
     * Nominate the current strong Hook for one later Again-as-weak execution
     * if the transaction reaches the native post-apply path and the same Hook
     * remains installed. Success confirms nomination, not eventual delivery.
     */
    function again(): HostVoidResult;

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
   * Omitted codes default to zero. Do not copy C `__LINE__`; explicitly
   * meaningful codes remain caller-owned.
   */
  function accept(message?: string | BytesLike | STBlob, code?: number): never;

  namespace accept {
    /**
     * Continue only when this Result succeeded with a present (non-nullish)
     * value; otherwise `accept` — this invocation is done, nothing to act
     * on. Falsy-but-present successes (`0`, `0n`, `false`, and `""`) are
     * values and continue.
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
     * Require `condition` to continue; otherwise `accept`. TypeScript narrows
     * from the asserted proposition after this call. The condition is ordinary
     * JavaScript truthiness at runtime; callers should pass an explicit boolean
     * proposition such as `transaction instanceof Payment`.
     */
    function require(
      condition: boolean,
      message?: string | BytesLike | STBlob,
      code?: number,
    ): asserts condition;
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
  /**
   * Serialized types with no declaration of their own, recorded so coverage over
   * definitions.json is total rather than silently partial.
   *
   * @serial-scalar   UInt8 UInt16 UInt32 as number
   * @serial-scalar   UInt64 as bigint
   * @serial-scalar   Number as string
   * @serial-sentinel Done NotPresent Unknown
   * @serial-unmapped UInt96 UInt192 UInt384 UInt512 Validation
   *
   * The remaining unmapped row is a decision list, not a silent coverage hole.
   * Vector256, Number, Currency, Issue, and XChainBridge are now explicit
   * materialized values in the complete recursive-object closure.
   */

  const HookReturnCode: HookReturnCodeEnum;
  type HookReturnCode = (typeof HookReturnCode)[keyof typeof HookReturnCode];


  /**
   * Every serialized protocol field, by name.
   *
   * `code` is what a host function expects: (typeCode << 16) | fieldCode,
   * the same encoding as sfAccount in the C headers. Plain data, not a
   * rich type — a contract that needs to name a field it has no typed
   * accessor for can reach for this.
   */
  const Field: {
    readonly LedgerEntryType: SerializedField<LedgerEntryType, 65537, 1, 1>;
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
