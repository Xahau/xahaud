//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.
*/
//==============================================================================

#ifndef RIPPLE_APP_MISC_DOMAINTRUSTANCHOR_H_INCLUDED
#define RIPPLE_APP_MISC_DOMAINTRUSTANCHOR_H_INCLUDED

#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/UintTypes.h>

namespace ripple {

/** Immutable WebPKI trust anchor for validator-domain proof verification.
 *
 * Production Application construction always pins the CMake-embedded ISRG
 * Root X1. jtx may inject a private test root at Application construction.
 * The anchor cannot change for the lifetime of the Application, and no
 * config/RPC/env surface may select it.
 */
struct DomainTrustAnchor
{
    Slice der;
    uint256 rootSetID;
};

}  // namespace ripple

#endif
