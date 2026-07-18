#ifndef RIPPLE_APP_PATHS_PAYGRAPHDELTA_H_INCLUDED
#define RIPPLE_APP_PATHS_PAYGRAPHDELTA_H_INCLUDED

//------------------------------------------------------------------------------
/*
    PayGraphDelta — Extract changed order-book pairs from ledger metadata.

    Ported from rippled PR #7392 and adapted to xahaud (Issue-based,
    namespace ripple, no permissioned domains).

    Called at each ledger close to build the changedBooks list passed to
    PayGraph::applyLedgerDelta().

    For each transaction in the just-closed ledger, any ltOFFER node that
    was created, modified, or deleted tells us that a specific order book
    (takerPays.issue, takerGets.issue) may have changed its top-of-book
    quality.  We collect those Book pairs and deduplicate them.

    The parsing logic mirrors OrderBookDB::processTxn() which is the
    canonical example of reading offer changes from transaction metadata.
*/
//------------------------------------------------------------------------------

#include <xrpl/protocol/Book.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/TxMeta.h>
#include <xrpl/protocol/UintTypes.h>

#include <vector>

namespace ripple {

/// Extract the set of order-book pairs that were touched by any transaction
/// metadata node array.  The result is deduplicated.
///
/// Matches the logic in OrderBookDB::processTxn().
inline std::vector<Book>
extractChangedBooks(STArray const& nodes)
{
    std::vector<Book> result;

    for (STObject const& node : nodes)
    {
        try
        {
            if (node.getFieldU16(sfLedgerEntryType) != ltOFFER)
                continue;

            // Determine which sub-field holds the TakerPays / TakerGets.
            // sfCreatedNode  -> sfNewFields
            // sfModifiedNode -> sfFinalFields
            // sfDeletedNode  -> sfFinalFields
            // We want the *resulting* state to determine the new top-of-book
            // after this ledger closes.
            SField const* subField = nullptr;
            SField const& nodeName = node.getFName();
            if (nodeName == sfCreatedNode)
            {
                subField = &sfNewFields;
            }
            else if (nodeName == sfModifiedNode || nodeName == sfDeletedNode)
            {
                subField = &sfFinalFields;
            }

            if (subField == nullptr)
                continue;

            auto const* data =
                dynamic_cast<STObject const*>(node.peekAtPField(*subField));
            if (data == nullptr)
                continue;
            if (!data->isFieldPresent(sfTakerPays) ||
                !data->isFieldPresent(sfTakerGets))
                continue;

            Book const book{
                data->getFieldAmount(sfTakerPays).issue(),
                data->getFieldAmount(sfTakerGets).issue()};

            // Deduplicate.
            bool dup = false;
            for (auto const& b : result)
            {
                if (b.in == book.in && b.out == book.out)
                {
                    dup = true;
                    break;
                }
            }
            if (!dup)
                result.push_back(book);
        }
        catch (std::exception const&)
        {
            // Malformed metadata node — skip safely.
        }
    }

    return result;
}

/// Convenience overload that accepts a TxMeta directly.
inline std::vector<Book>
extractChangedBooks(TxMeta const& meta)
{
    return extractChangedBooks(meta.getNodes());
}

/// Merge src into dest, deduplicating across both.
inline void
mergeBooks(std::vector<Book>& dest, std::vector<Book> const& src)
{
    for (auto const& b : src)
    {
        bool dup = false;
        for (auto const& d : dest)
        {
            if (d.in == b.in && d.out == b.out)
            {
                dup = true;
                break;
            }
        }
        if (!dup)
            dest.push_back(b);
    }
}

}  // namespace ripple

#endif
