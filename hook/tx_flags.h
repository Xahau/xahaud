// Generated using generate_txflags.sh
#include "ls_flags.h"
#include <stdint.h>

enum UniversalFlags : uint32_t {
    tfFullyCanonicalSig = 0x80000000,
};

enum AccountSetFlags : uint32_t {
    tfRequireDestTag = 0x00010000,
    tfOptionalDestTag = 0x00020000,
    tfRequireAuth = 0x00040000,
    tfOptionalAuth = 0x00080000,
    tfDisallowXRP = 0x00100000,
    tfAllowXRP = 0x00200000,
};

enum AccountFlags : uint32_t {
    asfRequireDest = 1,
    asfRequireAuth = 2,
    asfDisallowXRP = 3,
    asfDisableMaster = 4,
    asfAccountTxnID = 5,
    asfNoFreeze = 6,
    asfGlobalFreeze = 7,
    asfDefaultRipple = 8,
    asfDepositAuth = 9,
    asfAuthorizedNFTokenMinter = 10,
    asfTshCollect = 11,
    asfDisallowIncomingNFTokenOffer = 12,
    asfDisallowIncomingCheck = 13,
    asfDisallowIncomingPayChan = 14,
    asfDisallowIncomingTrustline = 15,
    asfDisallowIncomingRemit = 16,
    asfAllowTrustLineClawback = 17,
};

enum OfferCreateFlags : uint32_t {
    tfPassive = 0x00010000,
    tfImmediateOrCancel = 0x00020000,
    tfFillOrKill = 0x00040000,
    tfSell = 0x00080000,
};

enum PaymentFlags : uint32_t {
    tfNoRippleDirect = 0x00010000,
    tfPartialPayment = 0x00020000,
    tfLimitQuality = 0x00040000,
};

enum TrustSetFlags : uint32_t {
    tfSetfAuth = 0x00010000,
    tfSetNoRipple = 0x00020000,
    tfClearNoRipple = 0x00040000,
    tfSetFreeze = 0x00100000,
    tfClearFreeze = 0x00200000,
    tfSetDeepFreeze = 0x00400000,
    tfClearDeepFreeze = 0x00800000
};

enum EnableAmendmentFlags : uint32_t {
    tfGotMajority = 0x00010000,
    tfLostMajority = 0x00020000,
    tfTestSuite = 0x80000000,
};

enum PaymentChannelClaimFlags : uint32_t {
    tfRenew = 0x00010000,
    tfClose = 0x00020000,
};

enum NFTokenMintFlags : uint32_t {
    tfBurnable = 0x00000001,
    tfOnlyXRP = 0x00000002,
    tfTrustLine = 0x00000004,
    tfTransferable = 0x00000008,
    tfMutable = 0x00000010,
    tfStrongTSH = 0x00008000,
};

enum MPTokenIssuanceCreateFlags : uint32_t {
    tfMPTCanLock = lsfMPTCanLock,
    tfMPTRequireAuth = lsfMPTRequireAuth,
    tfMPTCanEscrow = lsfMPTCanEscrow,
    tfMPTCanTrade = lsfMPTCanTrade,
    tfMPTCanTransfer = lsfMPTCanTransfer,
    tfMPTCanClawback = lsfMPTCanClawback,
};

enum MPTokenAuthorizeFlags : uint32_t {
    tfMPTUnauthorize = 0x00000001,
};

enum MPTokenIssuanceSetFlags : uint32_t {
    tfMPTLock = 0x00000001,
    tfMPTUnlock = 0x00000002,
};

enum NFTokenCreateOfferFlags : uint32_t {
    tfSellNFToken = 0x00000001,
};

enum ClaimRewardFlags : uint32_t {
    tfOptOut = 0x00000001,
};

enum CronSetFlags : uint32_t {
    tfCronUnset = 0x00000001,
};

enum AMMClawbackFlags : uint32_t {
    tfClawTwoAssets = 0x00000001,
};

enum BridgeModifyFlags : uint32_t {
    tfClearAccountCreateAmount = 0x00010000,
};
