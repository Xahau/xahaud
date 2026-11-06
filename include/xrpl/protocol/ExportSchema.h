//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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

#ifndef RIPPLE_PROTOCOL_EXPORTSCHEMA_H_INCLUDED
#define RIPPLE_PROTOCOL_EXPORTSCHEMA_H_INCLUDED

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ripple {

class Rules;
class STObject;

/** What a hook may export (featureExport): the transactions both networks
    read the same way, and nothing else.

    The other network's `server_definitions` is checked in as
    src/libxrpl/protocol/xrpl_server_definitions.json and read at compile
    time (XrplDefinitions.h). From it and this network's own field
    and transaction tables (sfields.macro, transactions.macro) the build
    derives, for each transaction type an amendment has opened for export:

      - the type itself, which must have the same name and code on both
        networks (a type this network numbered differently is out, whatever
        it is called);
      - the fields it may carry: the type's own fields as the other network
        lists them, plus the fields every transaction there carries, each
        kept only if this network defines a field of the same name with the
        same type and code (a code the two networks assigned to different
        fields is out, so neither can misread the other's);
      - the fields the other network requires;
      - the flags the other network accepts on it.

    Anything nested (Memos, Paths, SignerEntries, Signers) must likewise be
    made only of fields both networks define identically.

    Types are opened by amendment, in the policy table in ExportSchema.cpp:
    adding a type there, gated on a new feature, lets developers try it on a
    test network as soon as both networks have it, while mainnet waits for
    the vote. The json is refreshed from a node there (see RippledCore.cmake)
    whenever the other network's definitions change; refreshing it never
    widens the set by itself, only a policy row does. */
struct ExportSchema
{
    /** The `hash` of the other network's definitions this build read */
    static std::string_view
    xrplDefinitionsHash();

    /** Why `tx`, a transaction built for the other network (with or without
        Signers), may not be exported under `rules`; nullopt if it may */
    static std::optional<std::string>
    reject(STObject const& tx, Rules const& rules);

    static bool
    allowed(STObject const& tx, Rules const& rules)
    {
        return !reject(tx, rules).has_value();
    }

    /** The transaction types exportable under `rules`, by name */
    static std::vector<std::string_view>
    types(Rules const& rules);
};

}  // namespace ripple

#endif
