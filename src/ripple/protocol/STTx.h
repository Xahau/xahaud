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

#ifndef RIPPLE_PROTOCOL_STTX_H_INCLUDED
#define RIPPLE_PROTOCOL_STTX_H_INCLUDED

#include <ripple/basics/Expected.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/Rules.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/SecretKey.h>
#include <ripple/protocol/SeqProxy.h>
#include <ripple/protocol/TxFormats.h>
#include <boost/container/flat_set.hpp>
#include <functional>

namespace ripple {

enum TxnSql : char {
    txnSqlNew = 'N',
    txnSqlConflict = 'C',
    txnSqlHeld = 'H',
    txnSqlValidated = 'V',
    txnSqlIncluded = 'I',
    txnSqlUnknown = 'U'
};

class STTx final : public STObject, public CountedObject<STTx>
{
    uint256 tid_;
    TxType tx_type_;

public:
    static std::size_t const minMultiSigners = 1;

    // if rules are not supplied then the largest possible value is returned
    static std::size_t
    maxMultiSigners(Rules const* rules = 0)
    {
        if (rules && !rules->enabled(featureExpandedSignerList))
            return 8;

        return 32;
    }

    STTx() = delete;
    STTx(STTx const& other) = default;
    STTx&
    operator=(STTx const& other) = delete;

    explicit STTx(SerialIter& sit);
    explicit STTx(SerialIter&& sit);
    explicit STTx(STObject&& object);

    /** Constructs a transaction.

        The returned transaction will have the specified type and
        any fields that the callback function adds to the object
        that's passed in.
    */
    STTx(TxType type, std::function<void(STObject&)> assembler);

    // STObject functions.
    SerializedTypeID
    getSType() const override;

    std::string
    getFullText() const override;

    // Outer transaction functions / signature functions.
    Blob
    getSignature() const;

    uint256
    getSigningHash() const;

    TxType
    getTxnType() const;

    Blob
    getSigningPubKey() const;

    SeqProxy
    getSeqProxy() const;

    boost::container::flat_set<AccountID>
    getMentionedAccounts() const;

    uint256
    getTransactionID() const;

    Json::Value
    getJson(JsonOptions options) const override;
    Json::Value
    getJson(JsonOptions options, bool binary) const;

    void
    sign(PublicKey const& publicKey, SecretKey const& secretKey);

    /** Check the signature.
        @return `true` if valid signature. If invalid, the error message string.
    */
    enum class RequireFullyCanonicalSig : bool { no, yes };
    Expected<void, std::string>
    checkSign(RequireFullyCanonicalSig requireCanonicalSig, Rules const& rules)
        const;

    // SQL Functions with metadata.
    static std::string const&
    getMetaSQLInsertReplaceHeader();

    std::string
    getMetaSQL(std::uint32_t inLedger, std::string const& escapedMetaData)
        const;

    std::string
    getMetaSQL(
        Serializer rawTxn,
        std::uint32_t inLedger,
        char status,
        std::string const& escapedMetaData) const;

private:
    Expected<void, std::string>
    checkSingleSign(RequireFullyCanonicalSig requireCanonicalSig) const;

    Expected<void, std::string>
    checkMultiSign(
        RequireFullyCanonicalSig requireCanonicalSig,
        Rules const& rules) const;

    STBase*
    copy(std::size_t n, void* buf) const override;
    STBase*
    move(std::size_t n, void* buf) override;

    friend class detail::STVar;
};

bool
passesLocalChecks(STObject const& st, std::string&);

/** Sterilize a transaction.

    The transaction is serialized and then deserialized,
    ensuring that all equivalent transactions are in canonical
    form. This also ensures that program metadata such as
    the transaction's digest, are all computed.
*/
std::shared_ptr<STTx const>
sterilize(STTx const& stx);

/** Check whether a transaction is a pseudo-transaction */
bool
isPseudoTx(STObject const& tx);

inline STTx::STTx(SerialIter&& sit) : STTx(sit)
{
}

inline TxType
STTx::getTxnType() const
{
    return tx_type_;
}

inline Blob
STTx::getSigningPubKey() const
{
    return getFieldVL(sfSigningPubKey);
}

inline uint256
STTx::getTransactionID() const
{
    return tid_;
}

//------------------------------------------------------------------------------
// Multi-sign depth limits
//------------------------------------------------------------------------------

/** Maximum nesting depth for nested multi-signing (featureNestedMultiSign). */
constexpr int nestedMultiSignMaxDepth = 4;

/** Maximum nesting depth when nested multi-signing is disabled (flat only). */
constexpr int legacyMultiSignMaxDepth = 1;

//------------------------------------------------------------------------------
// Multi-sign signer entry helpers
//------------------------------------------------------------------------------

/** Count the number of present (non-STI_NOTPRESENT) fields in an STObject.
    STObject::getCount() returns v_.size() which includes template slots for
    optional fields that aren't set. This helper counts only populated fields.
*/
inline std::size_t
countPresentFields(STObject const& obj)
{
    std::size_t count = 0;
    for (auto const& field : obj)
    {
        if (field.getSType() != STI_NOTPRESENT)
            ++count;
    }
    return count;
}

/** Check if a signer entry is a leaf signer (has signature fields).
    A leaf signer has exactly 3 present fields:
    Account + SigningPubKey + TxnSignature (no nested Signers).
*/
inline bool
isLeafSigner(STObject const& signer)
{
    return signer.isFieldPresent(sfAccount) &&
        signer.isFieldPresent(sfSigningPubKey) &&
        signer.isFieldPresent(sfTxnSignature) &&
        !signer.isFieldPresent(sfSigners) && countPresentFields(signer) == 3;
}

/** Check if a signer entry is a nested signer (delegates to sub-signers).
    A nested signer has exactly 2 present fields:
    Account + Signers (no SigningPubKey/TxnSignature).
*/
inline bool
isNestedSigner(STObject const& signer)
{
    return signer.isFieldPresent(sfAccount) &&
        signer.isFieldPresent(sfSigners) &&
        !signer.isFieldPresent(sfSigningPubKey) &&
        !signer.isFieldPresent(sfTxnSignature) &&
        countPresentFields(signer) == 2;
}

/** Check if a signer entry has valid structure for multi-signing.
    Returns true if the entry is either a valid leaf or nested signer.
*/
inline bool
isValidSignerEntry(STObject const& signer)
{
    return isLeafSigner(signer) || isNestedSigner(signer);
}

}  // namespace ripple

#endif
