//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

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

#ifndef RIPPLE_APP_CONSENSUS_RCLENTROPYSET_H_INCLUDED
#define RIPPLE_APP_CONSENSUS_RCLENTROPYSET_H_INCLUDED

#include <xrpld/shamap/SHAMap.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ripple {

/** featureRNG: pushing the ttENTROPY transactions of a position.

    With featureRNG every validator's position carries its own ttENTROPY
    pseudo-transaction, so no two initial positions are the same set and a
    server can no longer build the sets its peers propose from the
    transactions it already holds. It has to request each of them.

    An entropy set is everything a server is missing in the common case: the
    ttENTROPY transactions of one position, sent by the validator that took
    the position (TMEntropySet). A receiver adds them to the other
    transactions of its own position and, if the result hashes to the set the
    validator named, has that set without asking anybody for it. If it does
    not, nothing is lost: the set is acquired from peers as before.

    These helpers are shared by the sender (RCLConsensus), the receiver
    (PeerImp, RCLConsensus) and the tests.
*/
namespace entropy_set {

using Txns = std::vector<boost::intrusive_ptr<SHAMapItem const>>;

/** The most transactions an entropy set may carry. There is one per
    contributing validator, so this is far above any real UNL.
*/
constexpr std::size_t maxTxns = 256;

/** The largest transaction an entropy set may carry. The transaction
    featureRNG builds is under 150 bytes.
*/
constexpr std::size_t maxTxnBytes = 1024;

/** The ID of a serialized transaction. */
uint256
txID(Slice const& tx);

/** Whether a serialized transaction starts like a ttENTROPY.

    Cheap enough to run over every transaction of a large set. Only
    transactions that pass are deserialized.
*/
bool
looksLikeEntropyTx(Slice const& tx);

/** The transaction set item for a serialized ttENTROPY.

    @return The item, keyed by the transaction ID, or nullptr unless `tx` is
            the canonical serialization of a ttENTROPY of at most maxTxnBytes.
*/
boost::intrusive_ptr<SHAMapItem const>
makeItem(Slice const& tx);

/** The ledger a ttENTROPY is for: its sfLedgerSequence.

    @param item An item makeItem made.
*/
std::uint32_t
ledgerSeq(SHAMapItem const& item);

/** Whether a transaction set item is one makeItem would have made.

    The sender uses this to choose what goes into an entropy set and the
    receiver uses it to choose what to drop from its own position before
    adding them, so both sides always agree on what "the ttENTROPY
    transactions of a set" are.
*/
bool
isEntropyItem(SHAMapItem const& item);

/** The ttENTROPY transactions of a transaction set, ordered by ID. */
Txns
entropyTxns(SHAMap const& set);

/** An immutable copy of a transaction set without its ttENTROPY
    transactions.
*/
std::shared_ptr<SHAMap>
withoutEntropy(SHAMap const& set);

/** Build the transaction set a validator named from an entropy set.

    @param base    Our transactions other than ttENTROPY (see withoutEntropy).
    @param txns    The transactions of the entropy set.
    @param setHash The transaction set the entropy set is for.
    @return The immutable set, or nullptr if `base` plus `txns` is not the
            set with that hash.
*/
std::shared_ptr<SHAMap>
rebuild(SHAMap const& base, Txns const& txns, uint256 const& setHash);

/** What the validator signs: the previous ledger, the transaction set, and
    the IDs of its ttENTROPY transactions in ascending order.

    The digest does not depend on the signer, so it also identifies an entropy
    set's content: every validator that reaches the same position produces
    the same digest.
*/
uint256
digest(
    uint256 const& prevLedger,
    uint256 const& setHash,
    std::vector<uint256> const& txIDs);

/** digest() for a list of transaction set items. */
uint256
digest(uint256 const& prevLedger, uint256 const& setHash, Txns const& txns);

/** Identifies one signed entropy set message, for duplicate suppression.

    It covers the signature, so a copy with a bad signature cannot suppress
    the real message.
*/
uint256
uniqueId(uint256 const& digest, Slice const& publicKey, Slice const& signature);

}  // namespace entropy_set
}  // namespace ripple

#endif
