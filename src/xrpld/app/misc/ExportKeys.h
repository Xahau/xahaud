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

#ifndef RIPPLE_APP_MISC_EXPORTKEYS_H_INCLUDED
#define RIPPLE_APP_MISC_EXPORTKEYS_H_INCLUDED

#include <xrpl/basics/Blob.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/SecretKey.h>

#include <boost/filesystem.hpp>

#include <mutex>
#include <optional>
#include <vector>

namespace ripple {

class Config;
class ReadView;
class ValidatorKeys;

/** A validator's export keys (see Export.h): independent random ed25519 keys,
    kept for as long as the validator's account lists them on ledger.

    They live in a file (`[export_key_file]`, by default export_keys.txt in
    the database path), one hex secret key per line, oldest first, the last
    being the key currently nominated. It is as sensitive as the validator
    token and should be backed up: without it the validator can no longer
    sign for exporters still listing its older keys.

    Keys are independent rather than derived from one seed so that rotation
    is worth something: a compromise that has ended does not reach keys
    generated after it, and exporters that keep current move onto them.
*/
class ExportKeys
{
public:
    ExportKeys(
        Config const& config,
        ValidatorKeys const& validatorKeys,
        beast::Journal j);

    /** The export key to nominate, and its proof of possession, in the
        validation of `ledger`, a flag ledger. Generates a key first if there
        is none, if `ledger` shows the current key's epoch has passed, or if
        `ledger` has moved past the current key (restored from an older
        backup). Forgets keys `ledger` no longer lists. Empty unless this
        server is a validator.
    */
    std::optional<std::pair<PublicKey, Blob>>
    nominate(ReadView const& ledger);

    /** The held keys whose accounts are listed in `entries`, an
        sfSignerEntries array. */
    std::vector<std::pair<PublicKey, SecretKey>>
    signersFor(STArray const& entries) const;

private:
    struct Key
    {
        PublicKey publicKey;
        SecretKey secretKey;
        AccountID account;
        Blob proof;
    };

    Key
    make(SecretKey const& sk) const;

    void
    load();

    void
    save() const;

    beast::Journal const j_;
    std::optional<PublicKey> master_;
    std::uint32_t const networkID_;
    std::optional<boost::filesystem::path> file_;

    mutable std::mutex mutex_;
    std::vector<Key> keys_;  // oldest first, back() is the nominee
};

}  // namespace ripple

#endif
