//------------------------------------------------------------------------------
/*
    This file is part of xahaud: https://github.com/Xahau/xahaud
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

#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/AccountIndexer.h>
#include <xrpld/core/Config.h>
#include <xrpld/core/DatabaseCon.h>
#include <xrpld/core/SociDB.h>
#include <xrpl/basics/Log.h>
#include <xrpl/basics/contract.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/beast/core/CurrentThreadName.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STObject.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ripple {

namespace {

constexpr char const* AccountIndexDBName{"accountindex.db"};

inline constexpr std::array<char const*, 4> AccountIndexDBInit{
    {"BEGIN TRANSACTION;",

     // Idx is the AccountIndex; Account is the 40-hex-char AccountID.
     // Idx is dense and never reused, so it is the natural primary key.
     "CREATE TABLE IF NOT EXISTS AccountIndex (     \
        Idx     INTEGER PRIMARY KEY,                \
        Account CHARACTER(40) NOT NULL              \
    );",

     // IndexedSeq: every account in validated ledgers <= this is present.
     // Built:      1 once a full state walk has completed.
     "CREATE TABLE IF NOT EXISTS Meta (             \
        Key     TEXT PRIMARY KEY,                   \
        Value   INTEGER NOT NULL                    \
    );",

     "END TRANSACTION;"}};

// A catch-up gap wider than this is cheaper to close by rescanning the
// state of the newest validated ledger than by loading each ledger.
constexpr LedgerIndex MaxCatchUpLedgers{2000};

// Rows committed per transaction during a full scan. Lookups interleave
// between batches, so this bounds how long an RPC can be held off.
constexpr std::size_t ScanBatchRows{10000};

}  // namespace

class AccountIndexerImp : public AccountIndexer
{
    Application& app_;
    beast::Journal const j_;

    std::unique_ptr<DatabaseCon> db_;

    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    std::thread thread_;

    // Guarded by mutex_
    bool shouldExit_ = false;
    bool built_ = false;
    bool needRescan_ = false;
    // Set with needRescan_ when existing rows are suspect and must go.
    bool wipeOnRescan_ = false;
    LedgerIndex indexedSeq_ = 0;
    LedgerIndex targetSeq_ = 0;
    std::uint64_t count_ = 0;

public:
    AccountIndexerImp(Application& app, beast::Journal journal)
        : app_(app), j_(journal)
    {
    }

    ~AccountIndexerImp() override
    {
        if (thread_.joinable())
            LogicError("AccountIndexerImp::stop not called.");
    }

    void
    start() override
    {
        if (!app_.config().ACCOUNT_INDEX)
            return;

        try
        {
            auto setup = setup_DatabaseCon(app_.config(), j_);
            setup.useGlobalPragma = false;
            db_ = std::make_unique<DatabaseCon>(
                setup,
                AccountIndexDBName,
                std::array<std::string, 0>(),
                AccountIndexDBInit,
                j_);
            loadMeta();
        }
        catch (std::exception const& e)
        {
            JLOG(j_.fatal())
                << "Failed to open account index database: " << e.what();
            db_.reset();
            return;
        }

        JLOG(j_.info()) << "Account index: " << count_ << " accounts, "
                        << (built_ ? "built" : "not built")
                        << ", indexed through ledger " << indexedSeq_;

        thread_ = std::thread{&AccountIndexerImp::run, this};
    }

    void
    stop() override
    {
        if (!thread_.joinable())
            return;
        JLOG(j_.info()) << "Stopping";
        {
            std::lock_guard lock(mutex_);
            shouldExit_ = true;
            wakeup_.notify_one();
        }
        thread_.join();
    }

    void
    onValidatedLedger(LedgerIndex seq) override
    {
        if (!db_)
            return;
        std::lock_guard lock(mutex_);
        if (seq > targetSeq_)
        {
            targetSeq_ = seq;
            wakeup_.notify_one();
        }
    }

    bool
    enabled() const override
    {
        return db_ != nullptr;
    }

    Status
    status() const override
    {
        Status s;
        s.enabled = db_ != nullptr;
        if (!s.enabled)
            return s;
        std::lock_guard lock(mutex_);
        s.built = built_ && !needRescan_;
        s.indexedSeq = indexedSeq_;
        s.targetSeq = targetSeq_;
        s.count = count_;
        return s;
    }

    std::optional<AccountID>
    lookup(std::uint64_t index) const override
    {
        if (!db_)
            return std::nullopt;
        if (index >
            static_cast<std::uint64_t>(std::numeric_limits<long long>::max()))
            return std::nullopt;

        try
        {
            auto db = db_->checkoutDb();
            long long const idx = static_cast<long long>(index);
            std::string hex;
            soci::indicator ind = soci::i_null;
            *db << "SELECT Account FROM AccountIndex WHERE Idx = :i;",
                soci::into(hex, ind), soci::use(idx);
            if (!db->got_data() || ind != soci::i_ok || hex.size() != 40)
                return std::nullopt;
            AccountID id;
            if (!id.parseHex(hex))
                return std::nullopt;
            return id;
        }
        catch (std::exception const& e)
        {
            JLOG(j_.warn()) << "Account index lookup failed: " << e.what();
            return std::nullopt;
        }
    }

    //--------------------------------------------------------------------------
    //
    // PropertyStream
    //
    //--------------------------------------------------------------------------

    void
    onWrite(beast::PropertyStream::Map& map) override
    {
        auto const s = status();
        map["enabled"] = s.enabled;
        map["built"] = s.built;
        map["indexed_seq"] = s.indexedSeq;
        map["target_seq"] = s.targetSeq;
        map["count"] = s.count;
    }

private:
    //--------------------------------------------------------------------------
    //
    // Persistence helpers (call without mutex_ held)
    //
    //--------------------------------------------------------------------------

    void
    loadMeta()
    {
        auto db = db_->checkoutDb();

        auto metaValue =
            [&](std::string const& key) -> std::optional<long long> {
            long long v = 0;
            soci::indicator ind = soci::i_null;
            *db << "SELECT Value FROM Meta WHERE Key = :k;", soci::into(v, ind),
                soci::use(key);
            if (!db->got_data() || ind != soci::i_ok)
                return std::nullopt;
            return v;
        };

        LedgerIndex const indexedSeq =
            static_cast<LedgerIndex>(metaValue("IndexedSeq").value_or(0));
        bool const built = metaValue("Built").value_or(0) != 0;

        long long rows = 0;
        *db << "SELECT COUNT(*) FROM AccountIndex;", soci::into(rows);

        long long maxIdx = 0;
        soci::indicator ind = soci::i_null;
        *db << "SELECT MAX(Idx) FROM AccountIndex;", soci::into(maxIdx, ind);
        bool const haveMax = db->got_data() && ind == soci::i_ok;

        // The index must be dense: rows == max + 1. Anything else means the
        // database was damaged or produced by a buggy build; rebuild it.
        bool const dense =
            (rows == 0 && !haveMax) || (haveMax && rows == maxIdx + 1);

        std::lock_guard lock(mutex_);
        indexedSeq_ = indexedSeq;
        built_ = built;
        count_ = static_cast<std::uint64_t>(rows);
        if (built && !dense)
        {
            JLOG(j_.warn())
                << "Account index is not dense (" << rows
                << " rows, highest index " << maxIdx << "); scheduling rebuild";
            needRescan_ = true;
            wipeOnRescan_ = true;
        }
    }

    void
    saveMeta(soci::session& session, LedgerIndex indexedSeq, bool built)
    {
        long long const seq = indexedSeq;
        long long const b = built ? 1 : 0;
        session << "INSERT OR REPLACE INTO Meta (Key, Value) VALUES "
                   "('IndexedSeq', :s);",
            soci::use(seq);
        session << "INSERT OR REPLACE INTO Meta (Key, Value) VALUES "
                   "('Built', :b);",
            soci::use(b);
    }

    /** Insert a batch. Returns how many rows were new. */
    std::uint64_t
    insertBatch(
        soci::session& session,
        std::vector<std::pair<std::uint64_t, AccountID>> const& batch)
    {
        std::uint64_t added = 0;
        long long idx = 0;
        std::string hex;
        soci::statement st =
            (session.prepare << "INSERT OR IGNORE INTO AccountIndex "
                                "(Idx, Account) VALUES (:i, :a);",
             soci::use(idx),
             soci::use(hex));
        for (auto const& [i, acct] : batch)
        {
            if (i > static_cast<std::uint64_t>(
                        std::numeric_limits<long long>::max()))
                continue;
            idx = static_cast<long long>(i);
            hex = strHex(acct);
            st.execute(true);
            added += st.get_affected_rows();
        }
        return added;
    }

    //--------------------------------------------------------------------------
    //
    // Worker thread
    //
    //--------------------------------------------------------------------------

    void
    run()
    {
        beast::setCurrentThreadName("AccountIndexer");

        // Set when a scan or catch-up failed and should be retried later
        // rather than immediately.
        bool backoff = false;

        while (true)
        {
            bool rescan = false;
            {
                std::unique_lock lock(mutex_);
                auto const pending = [this] {
                    return shouldExit_ || needRescan_ ||
                        (targetSeq_ > 0 && !built_) ||
                        (built_ && targetSeq_ > indexedSeq_);
                };
                if (backoff)
                    wakeup_.wait_for(lock, std::chrono::seconds(30), [&] {
                        return shouldExit_;
                    });
                else
                    wakeup_.wait(lock, pending);

                if (shouldExit_)
                    break;
                if (!pending())
                    continue;
                rescan = needRescan_ || !built_;
            }

            backoff = rescan ? !fullScan() : !catchUp();
        }
    }

    /** Walk the state of the newest validated ledger and index every
        AccountRoot that carries an AccountIndex. Returns false to retry
        after a backoff. */
    bool
    fullScan()
    {
        auto const ledger = app_.getLedgerMaster().getValidatedLedger();
        if (!ledger)
        {
            JLOG(j_.debug()) << "No validated ledger yet; deferring scan";
            return false;
        }
        auto const seq = ledger->info().seq;
        JLOG(j_.info()) << "Account index: full scan of ledger " << seq
                        << " starting";

        bool wipe = false;
        {
            std::lock_guard lock(mutex_);
            needRescan_ = false;
            built_ = false;
            wipe = std::exchange(wipeOnRescan_, false);
        }

        std::uint64_t added = 0;
        std::uint64_t seen = 0;
        auto const started = std::chrono::steady_clock::now();

        try
        {
            if (wipe)
            {
                JLOG(j_.warn()) << "Account index: discarding existing rows";
                auto db = db_->checkoutDb();
                soci::transaction tr(*db);
                *db << "DELETE FROM AccountIndex;";
                saveMeta(*db, 0, false);
                tr.commit();
                std::lock_guard lock(mutex_);
                count_ = 0;
            }

            std::vector<std::pair<std::uint64_t, AccountID>> batch;
            batch.reserve(ScanBatchRows);

            auto flush = [&] {
                if (batch.empty())
                    return;
                auto db = db_->checkoutDb();
                soci::transaction tr(*db);
                added += insertBatch(*db, batch);
                tr.commit();
                batch.clear();
            };

            for (auto const& sle : ledger->sles)
            {
                if (sle->getType() != ltACCOUNT_ROOT ||
                    !sle->isFieldPresent(sfAccountIndex))
                    continue;
                ++seen;
                batch.emplace_back(
                    sle->getFieldU64(sfAccountIndex),
                    sle->getAccountID(sfAccount));
                if (batch.size() >= ScanBatchRows)
                {
                    flush();
                    {
                        std::lock_guard lock(mutex_);
                        if (shouldExit_)
                            return true;
                    }
                }
            }
            flush();

            {
                auto db = db_->checkoutDb();
                soci::transaction tr(*db);
                saveMeta(*db, seq, true);
                tr.commit();
            }
        }
        catch (std::exception const& e)
        {
            // Typically SHAMapMissingNode: this node does not hold the
            // full state of that ledger (yet). Try again later.
            JLOG(j_.warn())
                << "Account index: full scan of ledger " << seq
                << " failed after " << seen << " accounts: " << e.what();
            return false;
        }

        auto const elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started);

        {
            std::lock_guard lock(mutex_);
            built_ = true;
            indexedSeq_ = seq;
            count_ += added;
        }
        JLOG(j_.info()) << "Account index: full scan of ledger " << seq
                        << " done in " << elapsed.count() << "s; " << seen
                        << " accounts seen, " << added << " new";

        checkDense(*ledger);
        return true;
    }

    /** Index the accounts created by ledgers indexedSeq_+1 .. targetSeq_
        from their transaction metadata. Returns false to retry after a
        backoff (only when a rescan was scheduled instead). */
    bool
    catchUp()
    {
        while (true)
        {
            LedgerIndex seq = 0;
            {
                std::lock_guard lock(mutex_);
                if (shouldExit_ || needRescan_ || indexedSeq_ >= targetSeq_)
                    return true;
                if (targetSeq_ - indexedSeq_ > MaxCatchUpLedgers)
                {
                    JLOG(j_.info())
                        << "Account index: " << (targetSeq_ - indexedSeq_)
                        << " ledgers behind; rescanning instead";
                    needRescan_ = true;
                    return true;
                }
                seq = indexedSeq_ + 1;
            }

            auto const ledger = app_.getLedgerMaster().getLedgerBySeq(seq);
            if (!ledger)
            {
                JLOG(j_.warn()) << "Account index: ledger " << seq
                                << " unavailable; rescanning";
                std::lock_guard lock(mutex_);
                needRescan_ = true;
                return true;
            }

            std::uint64_t added = 0;
            try
            {
                std::vector<std::pair<std::uint64_t, AccountID>> batch;
                for (auto const& [tx, meta] : ledger->txs)
                {
                    if (!meta || !meta->isFieldPresent(sfAffectedNodes))
                        continue;
                    for (auto const& node :
                         meta->getFieldArray(sfAffectedNodes))
                    {
                        if (!node.isFieldPresent(sfLedgerEntryType) ||
                            node.getFieldU16(sfLedgerEntryType) !=
                                ltACCOUNT_ROOT)
                            continue;
                        SField const& kind = node.getFName();
                        // Created nodes carry NewFields; a Modified node's
                        // FinalFields are inserted too (INSERT OR IGNORE)
                        // in case an index was ever added post-creation.
                        SField const* fieldsName = nullptr;
                        if (kind == sfCreatedNode)
                            fieldsName = &sfNewFields;
                        else if (kind == sfModifiedNode)
                            fieldsName = &sfFinalFields;
                        else
                            continue;
                        if (!node.isFieldPresent(*fieldsName))
                            continue;
                        auto const& fields =
                            node.peekAtField(*fieldsName).downcast<STObject>();
                        if (!fields.isFieldPresent(sfAccountIndex) ||
                            !fields.isFieldPresent(sfAccount))
                            continue;
                        batch.emplace_back(
                            fields.getFieldU64(sfAccountIndex),
                            fields.getAccountID(sfAccount));
                    }
                }

                auto db = db_->checkoutDb();
                soci::transaction tr(*db);
                added = insertBatch(*db, batch);
                saveMeta(*db, seq, true);
                tr.commit();
            }
            catch (std::exception const& e)
            {
                JLOG(j_.warn()) << "Account index: processing ledger " << seq
                                << " failed: " << e.what() << "; rescanning";
                std::lock_guard lock(mutex_);
                needRescan_ = true;
                return true;
            }

            {
                std::lock_guard lock(mutex_);
                indexedSeq_ = seq;
                count_ += added;
            }
            if (added)
            {
                JLOG(j_.debug()) << "Account index: ledger " << seq << " added "
                                 << added << " accounts";
            }

            // Cheap self-check every flag-ledger interval so a missed
            // account cannot go unnoticed for long.
            if (seq % 256 == 0)
                checkDense(*ledger);
        }
    }

    /** Compare the number of indexed accounts with sfAccountCount on the
        ledger's FeeSettings. Any disagreement schedules a rebuild. */
    void
    checkDense(ReadView const& ledger)
    {
        auto const fees = ledger.read(keylet::fees());
        if (!fees || !fees->isFieldPresent(sfAccountCount))
            return;
        auto const expected = fees->getFieldU64(sfAccountCount);

        std::lock_guard lock(mutex_);
        if (count_ != expected)
        {
            JLOG(j_.warn())
                << "Account index: have " << count_ << " accounts but ledger "
                << ledger.info().seq << " reports AccountCount " << expected
                << "; scheduling rebuild";
            needRescan_ = true;
            // More rows than accounts means rows that should not exist.
            if (count_ > expected)
                wipeOnRescan_ = true;
        }
    }
};

std::unique_ptr<AccountIndexer>
make_AccountIndexer(Application& app, beast::Journal journal)
{
    return std::make_unique<AccountIndexerImp>(app, journal);
}

}  // namespace ripple
