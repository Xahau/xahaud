#ifndef RIPPLE_TEST_JTX_EXPORTPUBLICATION_H_INCLUDED
#define RIPPLE_TEST_JTX_EXPORTPUBLICATION_H_INCLUDED

#include <xrpl/protocol/STLedgerEntry.h>

namespace ripple::test {

// Shared by the fault-scenario outcome classifiers. A retained latch alone
// does not prove expiry; equality is the last live publication ledger.
inline bool
isExportPublicationExpired(SLE const& latch, LedgerIndex acceptedSeq)
{
    return latch.isFieldPresent(sfLastLedgerSequence) &&
        acceptedSeq > latch.getFieldU32(sfLastLedgerSequence);
}

}  // namespace ripple::test

#endif
