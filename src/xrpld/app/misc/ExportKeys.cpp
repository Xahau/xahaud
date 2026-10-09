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

#include <xrpld/app/misc/ExportKeys.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/core/Config.h>
#include <xrpld/core/ConfigSections.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpl/basics/Log.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STLedgerEntry.h>

#include <boost/algorithm/string/trim.hpp>

#include <algorithm>
#include <fstream>

namespace ripple {

ExportKeys::ExportKeys(
    Config const& config,
    ValidatorKeys const& validatorKeys,
    beast::Journal j)
    : j_(j), networkID_(config.NETWORK_ID)
{
    if (!validatorKeys.keys)
        return;

    master_ = validatorKeys.keys->masterPublicKey;

    if (auto const& v = config.section(SECTION_EXPORT_KEY_FILE).values();
        !v.empty())
        file_ = boost::filesystem::path(v.front());
    else if (auto const db = config.legacy("database_path"); !db.empty())
        file_ = boost::filesystem::path(db) / "export_keys.txt";
    else
        JLOG(j_.warn()) << "ExportKeys: no [export_key_file] or "
                           "database_path, export keys will not persist";

    load();
}

ExportKeys::Key
ExportKeys::make(SecretKey const& sk) const
{
    auto const pk = derivePublicKey(KeyType::ed25519, sk);
    auto const proof =
        sign(pk, sk, exportKeyProofData(*master_, pk, networkID_).slice());
    return Key{pk, sk, calcAccountID(pk), Blob(proof.begin(), proof.end())};
}

void
ExportKeys::load()
{
    boost::system::error_code ec;
    if (!file_ || !boost::filesystem::exists(*file_, ec))
        return;

    std::ifstream in(file_->string());
    std::string line;
    while (std::getline(in, line))
    {
        boost::algorithm::trim(line);
        if (line.empty())
            continue;
        auto const raw = strUnHex(line);
        if (!raw || raw->size() != 32)
            Throw<std::runtime_error>(
                "ExportKeys: malformed key in " + file_->string());
        keys_.push_back(make(SecretKey(makeSlice(*raw))));
    }

    JLOG(j_.info()) << "ExportKeys: loaded " << keys_.size() << " from "
                    << file_->string();
}

void
ExportKeys::save() const
{
    if (!file_)
        return;

    // write then rename, so a crash never leaves a truncated file
    auto const tmp = boost::filesystem::path(file_->string() + ".tmp");
    {
        std::ofstream out(tmp.string(), std::ios::trunc);
        boost::system::error_code ec;
        boost::filesystem::permissions(
            tmp,
            boost::filesystem::owner_read | boost::filesystem::owner_write,
            ec);
        for (auto const& k : keys_)
            out << strHex(
                       k.secretKey.data(),
                       k.secretKey.data() + k.secretKey.size())
                << "\n";
        out.flush();
        if (!out)
        {
            JLOG(j_.error()) << "ExportKeys: could not write " << tmp.string();
            return;
        }
    }
    boost::system::error_code ec;
    boost::filesystem::rename(tmp, *file_, ec);
    if (ec)
        JLOG(j_.error()) << "ExportKeys: could not replace " << file_->string()
                         << ": " << ec.message();
}

std::optional<std::pair<PublicKey, Blob>>
ExportKeys::nominate(ReadView const& ledger)
{
    if (!master_)
        return std::nullopt;

    std::lock_guard lock(mutex_);

    // the account's keys, newest first, and when the newest was recorded
    auto const sle = ledger.read(keylet::account(calcAccountID(*master_)));
    std::vector<Blob> onLedger;
    std::uint32_t headTime = 0;
    if (sle && sle->isFieldPresent(sfExportKeys))
    {
        auto const& a = sle->getFieldArray(sfExportKeys);
        for (auto const& e : a)
            onLedger.push_back(e.getFieldVL(sfExportKey));
        if (!a.empty())
            headTime = a[0].getFieldU32(sfCloseTime);
    }

    auto const blob = [](PublicKey const& pk) {
        return Blob(pk.begin(), pk.end());
    };

    bool rotate = keys_.empty();
    if (!rotate && !onLedger.empty())
    {
        auto const it = std::find(
            onLedger.begin(), onLedger.end(), blob(keys_.back().publicKey));
        auto const now = static_cast<std::uint32_t>(
            ledger.info().closeTime.time_since_epoch().count());
        if (it != onLedger.end() && it != onLedger.begin())
            rotate = true;  // the ledger moved past ours: an older backup
        else if (
            it == onLedger.begin() &&
            Export::rotationEpoch(now) > Export::rotationEpoch(headTime))
            rotate = true;  // a new epoch
    }

    bool changed = false;
    if (rotate)
    {
        keys_.push_back(make(randomKeyPair(KeyType::ed25519).second));
        changed = true;
        JLOG(j_.info()) << "ExportKeys: new export key "
                        << strHex(keys_.back().publicKey);
    }

    // forget keys the account no longer lists, except the nominee
    if (sle)
    {
        auto const n = keys_.size();
        keys_.erase(
            std::remove_if(
                keys_.begin(),
                std::prev(keys_.end()),
                [&](Key const& k) {
                    return std::find(
                               onLedger.begin(),
                               onLedger.end(),
                               blob(k.publicKey)) == onLedger.end();
                }),
            std::prev(keys_.end()));
        changed |= keys_.size() != n;
    }

    if (changed)
        save();

    return std::make_pair(keys_.back().publicKey, keys_.back().proof);
}

std::vector<std::pair<PublicKey, SecretKey>>
ExportKeys::signersFor(STArray const& entries) const
{
    std::vector<std::pair<PublicKey, SecretKey>> ret;
    if (!master_)
        return ret;

    std::lock_guard lock(mutex_);
    for (auto const& k : keys_)
        if (std::any_of(entries.begin(), entries.end(), [&](STObject const& e) {
                return e[sfAccount] == k.account;
            }))
            ret.emplace_back(k.publicKey, k.secretKey);
    return ret;
}

}  // namespace ripple
