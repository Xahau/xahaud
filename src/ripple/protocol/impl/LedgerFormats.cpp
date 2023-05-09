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

#include <ripple/protocol/LedgerFormats.h>
#include <ripple/protocol/jss.h>
#include <utility>

namespace ripple {

LedgerFormats::LedgerFormats()
{
    // clang-format off
    // Fields shared by all ledger formats:
    static const std::initializer_list<SOElement> commonFields{
        {sfLedgerIndex,              soeOPTIONAL},
        {sfLedgerEntryType,          soeREQUIRED},
        {sfFlags,                    soeREQUIRED},
    };

    add(jss::AccountRoot,
        ltACCOUNT_ROOT,
        {
            {sfAccount,              soeREQUIRED},
            {sfSequence,             soeREQUIRED},
            {sfBalance,              soeREQUIRED},
            {sfOwnerCount,           soeREQUIRED},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED},
            {sfAccountTxnID,         soeOPTIONAL},
            {sfRegularKey,           soeOPTIONAL},
            {sfEmailHash,            soeOPTIONAL},
            {sfWalletLocator,        soeOPTIONAL},
            {sfWalletSize,           soeOPTIONAL},
            {sfMessageKey,           soeOPTIONAL},
            {sfTransferRate,         soeOPTIONAL},
            {sfDomain,               soeOPTIONAL},
            {sfTickSize,             soeOPTIONAL},
            {sfTicketCount,          soeOPTIONAL},
            {sfNFTokenMinter,        soeOPTIONAL},
            {sfMintedNFTokens,       soeDEFAULT},
            {sfBurnedNFTokens,       soeDEFAULT},
            {sfHookStateCount,       soeOPTIONAL},
            {sfHookNamespaces,       soeOPTIONAL},
            {sfRewardLgrFirst,       soeOPTIONAL},
            {sfRewardLgrLast,        soeOPTIONAL},
            {sfRewardTime,           soeOPTIONAL},
            {sfRewardAccumulator,    soeOPTIONAL},
            {sfFirstNFTokenSequence, soeOPTIONAL},
            {sfImportSequence,       soeOPTIONAL},
        },
        commonFields);

    add(jss::DirectoryNode,
        ltDIR_NODE,
        {
            {sfOwner,                soeOPTIONAL},  // for owner directories
            {sfTakerPaysCurrency,    soeOPTIONAL},  // order book directories
            {sfTakerPaysIssuer,      soeOPTIONAL},  // order book directories
            {sfTakerGetsCurrency,    soeOPTIONAL},  // order book directories
            {sfTakerGetsIssuer,      soeOPTIONAL},  // order book directories
            {sfExchangeRate,         soeOPTIONAL},  // order book directories
            {sfReferenceCount,       soeOPTIONAL},  // for hook state directories
            {sfIndexes,              soeREQUIRED},
            {sfRootIndex,            soeREQUIRED},
            {sfIndexNext,            soeOPTIONAL},
            {sfIndexPrevious,        soeOPTIONAL},
            {sfNFTokenID,            soeOPTIONAL},
        },
        commonFields);

    add(jss::Offer,
        ltOFFER,
        {
            {sfAccount,              soeREQUIRED},
            {sfSequence,             soeREQUIRED},
            {sfTakerPays,            soeREQUIRED},
            {sfTakerGets,            soeREQUIRED},
            {sfBookDirectory,        soeREQUIRED},
            {sfBookNode,             soeREQUIRED},
            {sfOwnerNode,            soeREQUIRED},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED},
            {sfExpiration,           soeOPTIONAL},
        },
        commonFields);

    add(jss::RippleState,
        ltRIPPLE_STATE,
        {
            {sfBalance,              soeREQUIRED},
            {sfLowLimit,             soeREQUIRED},
            {sfHighLimit,            soeREQUIRED},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED},
            {sfLowNode,              soeOPTIONAL},
            {sfLowQualityIn,         soeOPTIONAL},
            {sfLowQualityOut,        soeOPTIONAL},
            {sfHighNode,             soeOPTIONAL},
            {sfHighQualityIn,        soeOPTIONAL},
            {sfHighQualityOut,       soeOPTIONAL},
            {sfLockedBalance,        soeOPTIONAL},
            {sfLockCount,            soeOPTIONAL},
        },
        commonFields);

    add(jss::Escrow,
        ltESCROW,
        {
            {sfAccount,              soeREQUIRED},
            {sfDestination,          soeREQUIRED},
            {sfAmount,               soeREQUIRED},
            {sfTransferRate,         soeREQUIRED},
            {sfCondition,            soeOPTIONAL},
            {sfCancelAfter,          soeOPTIONAL},
            {sfFinishAfter,          soeOPTIONAL},
            {sfSourceTag,            soeOPTIONAL},
            {sfDestinationTag,       soeOPTIONAL},
            {sfDestinationNode,      soeOPTIONAL},
            {sfOwnerNode,            soeREQUIRED},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED},
        },
        commonFields);

    add(jss::LedgerHashes,
        ltLEDGER_HASHES,
        {
            {sfFirstLedgerSequence,  soeOPTIONAL},
            {sfLastLedgerSequence,   soeOPTIONAL},
            {sfHashes,               soeREQUIRED},
        },
        commonFields);

    add(jss::Amendments,
        ltAMENDMENTS,
        {
            {sfAmendments,           soeOPTIONAL},  // Enabled
            {sfMajorities,           soeOPTIONAL},
        },
        commonFields);

    add(jss::FeeSettings,
        ltFEE_SETTINGS,
        {
            // Old version uses raw numbers
            {sfBaseFee,              soeOPTIONAL},
            {sfReferenceFeeUnits,    soeOPTIONAL},
            {sfReserveBase,          soeOPTIONAL},
            {sfReserveIncrement,     soeOPTIONAL},
            // New version uses Amounts
            {sfBaseFeeDrops,           soeOPTIONAL},
            {sfReserveBaseDrops,       soeOPTIONAL},
            {sfReserveIncrementDrops,  soeOPTIONAL},
        },
        commonFields);

    add(jss::Ticket,
        ltTICKET,
        {
            {sfAccount,              soeREQUIRED},
            {sfOwnerNode,            soeREQUIRED},
            {sfTicketSequence,       soeREQUIRED},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED},
        },
        commonFields);

    // All fields are soeREQUIRED because there is always a
    // SignerEntries.  If there are no SignerEntries the node is deleted.
    add(jss::SignerList,
        ltSIGNER_LIST,
        {
            {sfOwnerNode,            soeREQUIRED},
            {sfSignerQuorum,         soeREQUIRED},
            {sfSignerEntries,        soeREQUIRED},
            {sfSignerListID,         soeREQUIRED},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED},
        },
        commonFields);

    add(jss::Hook,
        ltHOOK,
        {
            {sfAccount, soeOPTIONAL},
            {sfOwnerNode, soeREQUIRED},
            {sfPreviousTxnID, soeREQUIRED},
            {sfPreviousTxnLgrSeq, soeREQUIRED},
            {sfHooks, soeREQUIRED} 
        },
        commonFields);

    add(jss::HookDefinition,
        ltHOOK_DEFINITION,
        {
            {sfHookHash, soeREQUIRED},
            {sfHookOn, soeREQUIRED},  
            {sfHookNamespace, soeREQUIRED},
            {sfHookParameters, soeREQUIRED},
            {sfHookApiVersion, soeREQUIRED},  
            {sfCreateCode, soeREQUIRED},
            {sfHookSetTxnID, soeREQUIRED},
            {sfReferenceCount, soeREQUIRED},
            {sfFee, soeREQUIRED},
            {sfHookCallbackFee, soeOPTIONAL}
        },
        commonFields);

    add(jss::HookState,
        ltHOOK_STATE,
        {
            {sfOwnerNode, soeREQUIRED},   
            {sfHookStateKey, soeREQUIRED},
            {sfHookStateData, soeREQUIRED},
        },
        commonFields);

    add(jss::PayChannel,
        ltPAYCHAN,
        {
            {sfAccount,              soeREQUIRED},
            {sfDestination,          soeREQUIRED},
            {sfAmount,               soeREQUIRED},
            {sfBalance,              soeREQUIRED},
            {sfPublicKey,            soeREQUIRED},
            {sfSettleDelay,          soeREQUIRED},
            {sfTransferRate,         soeREQUIRED},
            {sfExpiration,           soeOPTIONAL},
            {sfCancelAfter,          soeOPTIONAL},
            {sfSourceTag,            soeOPTIONAL},
            {sfDestinationTag,       soeOPTIONAL},
            {sfDestinationNode,      soeOPTIONAL},
            {sfOwnerNode,            soeREQUIRED},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED},
        },
        commonFields);

    add(jss::Check,
        ltCHECK,
        {
            {sfAccount,              soeREQUIRED},
            {sfDestination,          soeREQUIRED},
            {sfSendMax,              soeREQUIRED},
            {sfSequence,             soeREQUIRED},
            {sfOwnerNode,            soeREQUIRED},
            {sfDestinationNode,      soeREQUIRED},
            {sfExpiration,           soeOPTIONAL},
            {sfInvoiceID,            soeOPTIONAL},
            {sfSourceTag,            soeOPTIONAL},
            {sfDestinationTag,       soeOPTIONAL},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED},
        },
        commonFields);

    add(jss::DepositPreauth,
        ltDEPOSIT_PREAUTH,
        {
            {sfAccount,              soeREQUIRED},
            {sfAuthorize,            soeREQUIRED},
            {sfOwnerNode,            soeREQUIRED},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED},
        },
        commonFields);

    add(jss::NegativeUNL,
        ltNEGATIVE_UNL,
        {
            {sfDisabledValidators,   soeOPTIONAL},
            {sfValidatorToDisable,   soeOPTIONAL},
            {sfValidatorToReEnable,  soeOPTIONAL},
        },
        commonFields);

    add(jss::EmittedTxn,
        ltEMITTED_TXN,
        {
            {sfEmittedTxn, soeOPTIONAL},
            {sfOwnerNode, soeREQUIRED},
        },
        commonFields);
    
    add(jss::NFTokenPage,
        ltNFTOKEN_PAGE,
        {
            {sfPreviousPageMin,      soeOPTIONAL},
            {sfNextPageMin,          soeOPTIONAL},
            {sfNFTokens,             soeREQUIRED},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED}
        },
        commonFields);

    add(jss::NFTokenOffer,
        ltNFTOKEN_OFFER,
        {
            {sfOwner,                soeREQUIRED},
            {sfNFTokenID,            soeREQUIRED},
            {sfAmount,               soeREQUIRED},
            {sfOwnerNode,            soeREQUIRED},
            {sfNFTokenOfferNode,     soeREQUIRED},
            {sfDestination,          soeOPTIONAL},
            {sfExpiration,           soeOPTIONAL},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED}
        },
        commonFields);

    add(jss::URIToken,
        ltURI_TOKEN,
        {
            {sfOwner,                soeREQUIRED},
            {sfOwnerNode,            soeREQUIRED},
            {sfIssuer,               soeREQUIRED},
            {sfURI,                  soeREQUIRED},
            {sfDigest,               soeOPTIONAL},
            {sfAmount,               soeOPTIONAL},
            {sfDestination,          soeOPTIONAL},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED}
        },
        commonFields);

    // clang-format on
}

LedgerFormats const&
LedgerFormats::getInstance()
{
    static LedgerFormats instance;
    return instance;
}

}  // namespace ripple
