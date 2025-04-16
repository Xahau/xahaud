//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#ifndef RIPPLE_PROTOCOL_TXFLAGS_H_INCLUDED
#define RIPPLE_PROTOCOL_TXFLAGS_H_INCLUDED

#include <cstdint>

namespace ripple {

/** Transaction flags.

    These flags are specified in a transaction's 'Flags' field and modify the
    behavior of that transaction.

    There are two types of flags:

        (1) Universal flags: these are flags which apply to, and are interpreted
                             the same way by, all transactions, except, perhaps,
                             to special pseudo-transactions.

        (2) Tx-Specific flags: these are flags which are interpreted according
                               to the type of the transaction being executed.
                               That is, the same numerical flag value may have
                               different effects, depending on the transaction
                               being executed.

    @note The universal transaction flags occupy the high-order 8 bits. The
          tx-specific flags occupy the remaining 24 bits.

    @warning Transaction flags form part of the protocol. **Changing them
             should be avoided because without special handling, this will
             result in a hard fork.**

    @ingroup protocol
*/

// Formatting equals sign aligned 4 spaces after longest prefix, except for
// wrapped lines
// clang-format off
// Universal Transaction flags:
enum UniversalFlags : uint32_t {
    tfFullyCanonicalSig = 0x80000000,
};
constexpr std::uint32_t tfUniversal                        = tfFullyCanonicalSig;
constexpr std::uint32_t tfUniversalMask                    = ~tfUniversal;

// AccountSet flags:
enum AccountSetFlags : uint32_t {
    tfRequireDestTag = 0x00010000,
    tfOptionalDestTag = 0x00020000,
    tfRequireAuth = 0x00040000,
    tfOptionalAuth = 0x00080000,
    tfDisallowXRP = 0x00100000,
    tfAllowXRP = 0x00200000,
};
constexpr std::uint32_t tfAccountSetMask =
    ~(tfUniversal | tfRequireDestTag | tfOptionalDestTag | tfRequireAuth |
      tfOptionalAuth | tfDisallowXRP | tfAllowXRP);

// AccountSet SetFlag/ClearFlag values
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
};

// OfferCreate flags:
enum OfferCreateFlags : uint32_t {
    tfPassive = 0x00010000,
    tfImmediateOrCancel = 0x00020000,
    tfFillOrKill = 0x00040000,
    tfSell = 0x00080000,
};
constexpr std::uint32_t tfOfferCreateMask =
    ~(tfUniversal | tfPassive | tfImmediateOrCancel | tfFillOrKill | tfSell);

// Payment flags:
enum PaymentFlags : uint32_t {
    tfNoRippleDirect = 0x00010000,
    tfPartialPayment = 0x00020000,
    tfLimitQuality = 0x00040000,
};
constexpr std::uint32_t tfPaymentMask =
    ~(tfUniversal | tfPartialPayment | tfLimitQuality | tfNoRippleDirect);

// TrustSet flags:
enum TrustSetFlags : uint32_t {
    tfSetfAuth = 0x00010000,
    tfSetNoRipple = 0x00020000,
    tfClearNoRipple = 0x00040000,
    tfSetFreeze = 0x00100000,
    tfClearFreeze = 0x00200000,
};
constexpr std::uint32_t tfTrustSetMask =
    ~(tfUniversal | tfSetfAuth | tfSetNoRipple | tfClearNoRipple | tfSetFreeze |
      tfClearFreeze);

// EnableAmendment flags:
enum EnableAmendmentFlags : std::uint32_t {
    tfGotMajority = 0x00010000,
    tfLostMajority = 0x00020000,
    tfTestSuite = 0x80000000,
};

// PaymentChannelClaim flags:
enum PaymentChannelClaimFlags : uint32_t {
    tfRenew = 0x00010000,
    tfClose = 0x00020000,
};
constexpr std::uint32_t tfPayChanClaimMask = ~(tfUniversal | tfRenew | tfClose);

// NFTokenMint flags:
enum NFTokenMintFlags : uint32_t {
    tfBurnable = 0x00000001,
    tfOnlyXRP = 0x00000002,
    tfTrustLine = 0x00000004,
    tfTransferable = 0x00000008,
    tfStrongTSH = 0x00008000,
};

constexpr std::uint32_t const tfNFTokenMintOldMask =
    ~(tfUniversal | tfBurnable | tfOnlyXRP | tfTrustLine | tfTransferable | tfStrongTSH);

// Prior to fixRemoveNFTokenAutoTrustLine, transfer of an NFToken between
// accounts allowed a TrustLine to be added to the issuer of that token
// without explicit permission from that issuer.  This was enabled by
// minting the NFToken with the tfTrustLine flag set.
//
// That capability could be used to attack the NFToken issuer.  It
// would be possible for two accounts to trade the NFToken back and forth
// building up any number of TrustLines on the issuer, increasing the
// issuer's reserve without bound.
//
// The fixRemoveNFTokenAutoTrustLine amendment disables minting with the
// tfTrustLine flag as a way to prevent the attack.  But until the
// amendment passes we still need to keep the old behavior available.

constexpr std::uint32_t const tfNFTokenMintMask =
    ~(tfUniversal | tfBurnable | tfOnlyXRP | tfTransferable | tfStrongTSH);

// NFTokenCreateOffer flags:
enum NFTokenCreateOfferFlags : uint32_t {
    tfSellNFToken = 0x00000001,
};
constexpr std::uint32_t const tfNFTokenCreateOfferMask =
    ~(tfUniversal | tfSellNFToken);

// NFTokenCancelOffer flags:
constexpr std::uint32_t const tfNFTokenCancelOfferMask     = ~(tfUniversal);

// NFTokenAcceptOffer flags:
constexpr std::uint32_t const tfNFTokenAcceptOfferMask     = ~tfUniversal;

// URIToken mask
constexpr std::uint32_t const tfURITokenMintMask = ~(tfUniversal | tfBurnable);
constexpr std::uint32_t const tfURITokenNonMintMask = ~tfUniversal;

// ClaimReward flags:
enum ClaimRewardFlags : uint32_t {
    tfOptOut = 0x00000001,
};
constexpr std::uint32_t const tfClaimRewardMask = ~(tfUniversal | tfOptOut);

// Remarks flags:
constexpr std::uint32_t const tfImmutable = 1;

// clang-format on

}  // namespace ripple

#endif
