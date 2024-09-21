//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2021 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/rdb/Wallet.h>
#include <boost/format.hpp>

namespace ripple {

std::unique_ptr<DatabaseCon>
makeWalletDB(DatabaseCon::Setup const& setup)
{
    // wallet database
    if (setup.sqlite3)
    {
        return std::make_unique<DatabaseCon>(
            setup, WalletDBName, std::array<char const*, 0>(), WalletDBInit);
    }
    return std::make_unique<DatabaseCon>(setup, LMDBWalletDBName);
}

std::unique_ptr<DatabaseCon>
makeTestWalletDB(DatabaseCon::Setup const& setup, std::string const& dbname)
{
    // wallet database
    if (setup.sqlite3)
    {
        return std::make_unique<DatabaseCon>(
            setup, dbname.data(), std::array<char const*, 0>(), WalletDBInit);
    }
    return std::make_unique<DatabaseCon>(setup, dbname);
}

void
getManifests(
    MDB_env* env,
    std::string const& dbTable,
    ManifestCache& mCache,
    beast::Journal j)
{
    MDB_txn* txn;
    MDB_dbi dbi;
    MDB_val key, data;
    int rc;

    rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn);
    if (rc != 0)
    {
        // Handle error
        return;
    }

    rc = mdb_dbi_open(txn, dbTable.c_str(), 0, &dbi);
    if (rc != 0)
    {
        mdb_txn_abort(txn);
        // Handle error
        return;
    }

    MDB_cursor* cursor;
    rc = mdb_cursor_open(txn, dbi, &cursor);
    if (rc != 0)
    {
        mdb_txn_abort(txn);
        // Handle error
        return;
    }

    while ((rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT)) == 0)
    {
        std::string serialized(static_cast<char*>(data.mv_data), data.mv_size);
        if (auto mo = deserializeManifest(serialized))
        {
            if (!mo->verify())
            {
                JLOG(j.warn()) << "Unverifiable manifest in db";
                continue;
            }

            mCache.applyManifest(std::move(*mo));
        }
        else
        {
            JLOG(j.warn()) << "Malformed manifest in database";
        }
    }

    mdb_cursor_close(cursor);
    mdb_txn_commit(txn);
    mdb_dbi_close(env, dbi);
}

void
getManifests(
    soci::session& session,
    std::string const& dbTable,
    ManifestCache& mCache,
    beast::Journal j)
{
    // Load manifests stored in database
    std::string const sql = "SELECT RawData FROM " + dbTable + ";";
    soci::blob sociRawData(session);
    soci::statement st = (session.prepare << sql, soci::into(sociRawData));
    st.execute();
    while (st.fetch())
    {
        std::string serialized;
        convert(sociRawData, serialized);
        if (auto mo = deserializeManifest(serialized))
        {
            if (!mo->verify())
            {
                JLOG(j.warn()) << "Unverifiable manifest in db";
                continue;
            }

            mCache.applyManifest(std::move(*mo));
        }
        else
        {
            JLOG(j.warn()) << "Malformed manifest in database";
        }
    }
}

static void
saveManifest(MDB_txn* txn, MDB_dbi dbi, std::string const& serialized)
{
    MDB_val key, data;
    key.mv_size = serialized.size();
    key.mv_data = const_cast<char*>(serialized.data());
    data.mv_size = serialized.size();
    data.mv_data = const_cast<char*>(serialized.data());

    mdb_put(txn, dbi, &key, &data, 0);
}

static void
saveManifest(
    soci::session& session,
    std::string const& dbTable,
    std::string const& serialized)
{
    // soci does not support bulk insertion of blob data
    // Do not reuse blob because manifest ecdsa signatures vary in length
    // but blob write length is expected to be >= the last write
    soci::blob rawData(session);
    convert(serialized, rawData);
    session << "INSERT INTO " << dbTable << " (RawData) VALUES (:rawData);",
        soci::use(rawData);
}

void
saveManifests(
    MDB_env* env,
    std::string const& dbTable,
    std::function<bool(PublicKey const&)> const& isTrusted,
    hash_map<PublicKey, Manifest> const& map,
    beast::Journal j)
{
    MDB_txn* txn;
    MDB_dbi dbi;
    int rc;

    rc = mdb_txn_begin(env, nullptr, 0, &txn);
    if (rc != 0)
    {
        // Handle error
        return;
    }

    rc = mdb_dbi_open(txn, dbTable.c_str(), 0, &dbi);
    if (rc != 0)
    {
        mdb_txn_abort(txn);
        // Handle error
        return;
    }

    rc = mdb_drop(txn, dbi, 0);
    if (rc != 0)
    {
        mdb_txn_abort(txn);
        // Handle error
        return;
    }

    for (auto const& v : map)
    {
        if (!v.second.revoked() && !isTrusted(v.second.masterKey))
        {
            JLOG(j.info()) << "Untrusted manifest in cache not saved to db";
            continue;
        }

        saveManifest(txn, dbi, v.second.serialized);
    }

    mdb_txn_commit(txn);
    mdb_dbi_close(env, dbi);
}

void
saveManifests(
    soci::session& session,
    std::string const& dbTable,
    std::function<bool(PublicKey const&)> const& isTrusted,
    hash_map<PublicKey, Manifest> const& map,
    beast::Journal j)
{
    soci::transaction tr(session);
    session << "DELETE FROM " << dbTable;
    for (auto const& v : map)
    {
        // Save all revocation manifests,
        // but only save trusted non-revocation manifests.
        if (!v.second.revoked() && !isTrusted(v.second.masterKey))
        {
            JLOG(j.info()) << "Untrusted manifest in cache not saved to db";
            continue;
        }

        saveManifest(session, dbTable, v.second.serialized);
    }
    tr.commit();
}

void
addValidatorManifest(MDB_env* env, std::string const& serialized)
{
    MDB_txn* txn;
    MDB_dbi dbi;
    int rc;

    rc = mdb_txn_begin(env, nullptr, 0, &txn);
    if (rc != 0)
    {
        // Handle error
        return;
    }

    rc = mdb_dbi_open(txn, "ValidatorManifests", 0, &dbi);
    if (rc != 0)
    {
        mdb_txn_abort(txn);
        // Handle error
        return;
    }

    saveManifest(txn, dbi, serialized);

    mdb_txn_commit(txn);
    mdb_dbi_close(env, dbi);
}

void
addValidatorManifest(soci::session& session, std::string const& serialized)
{
    soci::transaction tr(session);
    saveManifest(session, "ValidatorManifests", serialized);
    tr.commit();
}

void
clearNodeIdentity(MDB_env* env)
{
    MDB_txn* txn;
    MDB_dbi dbi;
    mdb_txn_begin(env, nullptr, 0, &txn);
    mdb_dbi_open(txn, "NodeIdentity", 0, &dbi);
    mdb_drop(txn, dbi, 0);
    mdb_txn_commit(txn);
    mdb_dbi_close(env, dbi);
}

void
clearNodeIdentity(soci::session& session)
{
    session << "DELETE FROM NodeIdentity;";
}

std::pair<PublicKey, SecretKey>
getNodeIdentity(MDB_env* env)
{
    MDB_txn* txn;
    MDB_dbi dbi;
    MDB_val key, data;
    mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn);
    mdb_dbi_open(txn, "NodeIdentity", 0, &dbi);

    key.mv_size = sizeof("PublicKey");
    key.mv_data = const_cast<char*>("PublicKey");

    std::string pubKO, priKO;
    if (mdb_get(txn, dbi, &key, &data) == MDB_SUCCESS)
    {
        pubKO.assign(static_cast<char*>(data.mv_data), data.mv_size);

        key.mv_size = sizeof("PrivateKey");
        key.mv_data = const_cast<char*>("PrivateKey");

        if (mdb_get(txn, dbi, &key, &data) == MDB_SUCCESS)
        {
            priKO.assign(static_cast<char*>(data.mv_data), data.mv_size);

            auto const sk =
                parseBase58<SecretKey>(TokenType::NodePrivate, priKO);
            auto const pk =
                parseBase58<PublicKey>(TokenType::NodePublic, pubKO);

            if (sk && pk && (*pk == derivePublicKey(KeyType::secp256k1, *sk)))
            {
                mdb_txn_commit(txn);
                mdb_dbi_close(env, dbi);
                return {*pk, *sk};
            }
        }
    }

    mdb_txn_abort(txn);
    mdb_dbi_close(env, dbi);

    auto [newpublicKey, newsecretKey] = randomKeyPair(KeyType::secp256k1);

    mdb_txn_begin(env, nullptr, 0, &txn);
    mdb_dbi_open(txn, "NodeIdentity", MDB_CREATE, &dbi);

    std::string newPubKeyStr = toBase58(TokenType::NodePublic, newpublicKey);
    std::string newPriKeyStr = toBase58(TokenType::NodePrivate, newsecretKey);

    key.mv_size = sizeof("PublicKey");
    key.mv_data = const_cast<char*>("PublicKey");
    data.mv_size = newPubKeyStr.size();
    data.mv_data = const_cast<char*>(newPubKeyStr.data());
    mdb_put(txn, dbi, &key, &data, 0);

    key.mv_size = sizeof("PrivateKey");
    key.mv_data = const_cast<char*>("PrivateKey");
    data.mv_size = newPriKeyStr.size();
    data.mv_data = const_cast<char*>(newPriKeyStr.data());
    mdb_put(txn, dbi, &key, &data, 0);

    mdb_txn_commit(txn);
    mdb_dbi_close(env, dbi);

    return {newpublicKey, newsecretKey};
}

std::pair<PublicKey, SecretKey>
getNodeIdentity(soci::session& session)
{
    {
        // SOCI requires boost::optional (not std::optional) as the parameter.
        boost::optional<std::string> pubKO, priKO;
        soci::statement st =
            (session.prepare
                 << "SELECT PublicKey, PrivateKey FROM NodeIdentity;",
             soci::into(pubKO),
             soci::into(priKO));
        st.execute();
        while (st.fetch())
        {
            auto const sk = parseBase58<SecretKey>(
                TokenType::NodePrivate, priKO.value_or(""));
            auto const pk = parseBase58<PublicKey>(
                TokenType::NodePublic, pubKO.value_or(""));

            // Only use if the public and secret keys are a pair
            if (sk && pk && (*pk == derivePublicKey(KeyType::secp256k1, *sk)))
                return {*pk, *sk};
        }
    }

    // If a valid identity wasn't found, we randomly generate a new one:
    auto [newpublicKey, newsecretKey] = randomKeyPair(KeyType::secp256k1);

    session << str(
        boost::format("INSERT INTO NodeIdentity (PublicKey,PrivateKey) "
                      "VALUES ('%s','%s');") %
        toBase58(TokenType::NodePublic, newpublicKey) %
        toBase58(TokenType::NodePrivate, newsecretKey));

    return {newpublicKey, newsecretKey};
}

std::unordered_set<PeerReservation, beast::uhash<>, KeyEqual>
getPeerReservationTable(MDB_env* env, beast::Journal j)
{
    std::unordered_set<PeerReservation, beast::uhash<>, KeyEqual> table;
    MDB_txn* txn;
    MDB_dbi dbi;
    MDB_val key, data;
    MDB_cursor* cursor;

    // Start a read-only transaction
    mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn);
    mdb_dbi_open(txn, "PeerReservations", 0, &dbi);
    mdb_cursor_open(txn, dbi, &cursor);

    while (mdb_cursor_get(cursor, &key, &data, MDB_NEXT) == 0)
    {
        std::string valPubKey(static_cast<char*>(key.mv_data), key.mv_size);
        std::string valDesc(static_cast<char*>(data.mv_data), data.mv_size);

        auto const optNodeId =
            parseBase58<PublicKey>(TokenType::NodePublic, valPubKey);
        if (!optNodeId)
        {
            JLOG(j.warn()) << "load: not a public key: " << valPubKey;
            continue;
        }
        table.insert(PeerReservation{*optNodeId, valDesc});
    }

    mdb_cursor_close(cursor);
    mdb_txn_commit(txn);
    mdb_dbi_close(env, dbi);

    return table;
}

std::unordered_set<PeerReservation, beast::uhash<>, KeyEqual>
getPeerReservationTable(soci::session& session, beast::Journal j)
{
    std::unordered_set<PeerReservation, beast::uhash<>, KeyEqual> table;
    // These values must be boost::optionals (not std) because SOCI expects
    // boost::optionals.
    boost::optional<std::string> valPubKey, valDesc;
    // We should really abstract the table and column names into constants,
    // but no one else does. Because it is too tedious? It would be easy if we
    // had a jOOQ for C++.
    soci::statement st =
        (session.prepare
             << "SELECT PublicKey, Description FROM PeerReservations;",
         soci::into(valPubKey),
         soci::into(valDesc));
    st.execute();
    while (st.fetch())
    {
        if (!valPubKey || !valDesc)
        {
            // This represents a `NULL` in a `NOT NULL` column. It should be
            // unreachable.
            continue;
        }
        auto const optNodeId =
            parseBase58<PublicKey>(TokenType::NodePublic, *valPubKey);
        if (!optNodeId)
        {
            JLOG(j.warn()) << "load: not a public key: " << valPubKey;
            continue;
        }
        table.insert(PeerReservation{*optNodeId, *valDesc});
    }

    return table;
}

void
insertPeerReservation(
    MDB_env* env,
    PublicKey const& nodeId,
    std::string const& description)
{
    MDB_txn* txn;
    MDB_dbi dbi;
    MDB_val key, data;

    auto const sNodeId = toBase58(TokenType::NodePublic, nodeId);
    key.mv_size = sNodeId.size();
    key.mv_data = const_cast<char*>(sNodeId.data());
    data.mv_size = description.size();
    data.mv_data = const_cast<char*>(description.data());

    // Start a write transaction
    mdb_txn_begin(env, nullptr, 0, &txn);
    mdb_dbi_open(txn, "PeerReservations", MDB_CREATE, &dbi);
    mdb_put(txn, dbi, &key, &data, 0);
    mdb_txn_commit(txn);
    mdb_dbi_close(env, dbi);
}

void
insertPeerReservation(
    soci::session& session,
    PublicKey const& nodeId,
    std::string const& description)
{
    auto const sNodeId = toBase58(TokenType::NodePublic, nodeId);
    session << "INSERT INTO PeerReservations (PublicKey, Description) "
               "VALUES (:nodeId, :desc) "
               "ON CONFLICT (PublicKey) DO UPDATE SET "
               "Description=excluded.Description",
        soci::use(sNodeId), soci::use(description);
}

void
deletePeerReservation(MDB_env* env, PublicKey const& nodeId)
{
    MDB_txn* txn;
    MDB_dbi dbi;
    MDB_val key;

    auto const sNodeId = toBase58(TokenType::NodePublic, nodeId);
    key.mv_size = sNodeId.size();
    key.mv_data = const_cast<char*>(sNodeId.data());

    // Start a write transaction
    mdb_txn_begin(env, nullptr, 0, &txn);
    mdb_dbi_open(txn, "PeerReservations", 0, &dbi);
    mdb_del(txn, dbi, &key, nullptr);
    mdb_txn_commit(txn);
    mdb_dbi_close(env, dbi);
}

void
deletePeerReservation(soci::session& session, PublicKey const& nodeId)
{
    auto const sNodeId = toBase58(TokenType::NodePublic, nodeId);
    session << "DELETE FROM PeerReservations WHERE PublicKey = :nodeId",
        soci::use(sNodeId);
}

bool
createFeatureVotes(MDB_env* env)
{
    // soci::transaction tr(session);
    // std::string sql =
    //     "SELECT count(*) FROM sqlite_master "
    //     "WHERE type='table' AND name='FeatureVotes'";
    // // SOCI requires boost::optional (not std::optional) as the parameter.
    // boost::optional<int> featureVotesCount;
    // session << sql, soci::into(featureVotesCount);
    // bool exists = static_cast<bool>(*featureVotesCount);

    // // Create FeatureVotes table in WalletDB if it doesn't exist
    // if (!exists)
    // {
    //     session << "CREATE TABLE  FeatureVotes ( "
    //                "AmendmentHash      CHARACTER(64) NOT NULL, "
    //                "AmendmentName      TEXT, "
    //                "Veto               INTEGER NOT NULL );";
    //     tr.commit();
    // }
    // return exists;
    return false;
}

bool
createFeatureVotes(soci::session& session)
{
    soci::transaction tr(session);
    std::string sql =
        "SELECT count(*) FROM sqlite_master "
        "WHERE type='table' AND name='FeatureVotes'";
    // SOCI requires boost::optional (not std::optional) as the parameter.
    boost::optional<int> featureVotesCount;
    session << sql, soci::into(featureVotesCount);
    bool exists = static_cast<bool>(*featureVotesCount);

    // Create FeatureVotes table in WalletDB if it doesn't exist
    if (!exists)
    {
        session << "CREATE TABLE  FeatureVotes ( "
                   "AmendmentHash      CHARACTER(64) NOT NULL, "
                   "AmendmentName      TEXT, "
                   "Veto               INTEGER NOT NULL );";
        tr.commit();
    }
    return exists;
}

void
readAmendments(
    MDB_env* env,
    std::function<void(
        boost::optional<std::string> amendment_hash,
        boost::optional<std::string> amendment_name,
        boost::optional<AmendmentVote> vote)> const& callback)
{
    // lambda that converts the internally stored int to an AmendmentVote.
    // auto intToVote = [](boost::optional<int> const& dbVote)
    //     -> boost::optional<AmendmentVote> {
    //     return safe_cast<AmendmentVote>(dbVote.value_or(1));
    // };

    // soci::transaction tr(session);
    // std::string sql =
    //     "SELECT AmendmentHash, AmendmentName, Veto FROM "
    //     "( SELECT AmendmentHash, AmendmentName, Veto, RANK() OVER "
    //     "(  PARTITION BY AmendmentHash ORDER BY ROWID DESC ) "
    //     "as rnk FROM FeatureVotes ) WHERE rnk = 1";
    // // SOCI requires boost::optional (not std::optional) as parameters.
    // boost::optional<std::string> amendment_hash;
    // boost::optional<std::string> amendment_name;
    // boost::optional<int> vote_to_veto;
    // soci::statement st =
    //     (session.prepare << sql,
    //      soci::into(amendment_hash),
    //      soci::into(amendment_name),
    //      soci::into(vote_to_veto));
    // st.execute();
    // while (st.fetch())
    // {
    //     callback(amendment_hash, amendment_name, intToVote(vote_to_veto));
    // }
}

void
readAmendments(
    soci::session& session,
    std::function<void(
        boost::optional<std::string> amendment_hash,
        boost::optional<std::string> amendment_name,
        boost::optional<AmendmentVote> vote)> const& callback)
{
    // lambda that converts the internally stored int to an AmendmentVote.
    auto intToVote = [](boost::optional<int> const& dbVote)
        -> boost::optional<AmendmentVote> {
        return safe_cast<AmendmentVote>(dbVote.value_or(1));
    };

    soci::transaction tr(session);
    std::string sql =
        "SELECT AmendmentHash, AmendmentName, Veto FROM "
        "( SELECT AmendmentHash, AmendmentName, Veto, RANK() OVER "
        "(  PARTITION BY AmendmentHash ORDER BY ROWID DESC ) "
        "as rnk FROM FeatureVotes ) WHERE rnk = 1";
    // SOCI requires boost::optional (not std::optional) as parameters.
    boost::optional<std::string> amendment_hash;
    boost::optional<std::string> amendment_name;
    boost::optional<int> vote_to_veto;
    soci::statement st =
        (session.prepare << sql,
         soci::into(amendment_hash),
         soci::into(amendment_name),
         soci::into(vote_to_veto));
    st.execute();
    while (st.fetch())
    {
        callback(amendment_hash, amendment_name, intToVote(vote_to_veto));
    }
}

void
voteAmendment(
    MDB_env* env,
    uint256 const& amendment,
    std::string const& name,
    AmendmentVote vote)
{
    // soci::transaction tr(session);
    // std::string sql =
    //     "INSERT INTO FeatureVotes (AmendmentHash, AmendmentName, Veto) VALUES
    //     "
    //     "('";
    // sql += to_string(amendment);
    // sql += "', '" + name;
    // sql += "', '" + std::to_string(safe_cast<int>(vote)) + "');";
    // session << sql;
    // tr.commit();
}

void
voteAmendment(
    soci::session& session,
    uint256 const& amendment,
    std::string const& name,
    AmendmentVote vote)
{
    soci::transaction tr(session);
    std::string sql =
        "INSERT INTO FeatureVotes (AmendmentHash, AmendmentName, Veto) VALUES "
        "('";
    sql += to_string(amendment);
    sql += "', '" + name;
    sql += "', '" + std::to_string(safe_cast<int>(vote)) + "');";
    session << sql;
    tr.commit();
}

}  // namespace ripple
