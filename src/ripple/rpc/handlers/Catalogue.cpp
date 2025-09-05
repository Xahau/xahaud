//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

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

#include <ripple/rpc/handlers/Catalogue.h>
#include <iomanip>
#include <sstream>

namespace ripple {

// Global status for catalogue operations
std::shared_mutex catalogueStatusMutex;
CatalogueRunStatus catalogueRunStatus;

std::string
formatBytesIEC(uint64_t bytes, int precision)
{
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    int unit_index = 0;
    auto size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_index < 5)
    {
        size /= 1024.0;
        unit_index++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << size << " "
        << units[unit_index];
    return oss.str();
}

std::string
toHexString(unsigned char const* data, size_t len)
{
    static char const* hexDigits = "0123456789ABCDEF";
    std::string result;
    result.reserve(2 * len);
    for (size_t i = 0; i < len; ++i)
    {
        unsigned char c = data[i];
        result.push_back(hexDigits[c >> 4]);
        result.push_back(hexDigits[c & 15]);
    }
    return result;
}

// Helper function to generate status JSON
// IMPORTANT: Caller must hold at least a shared (read) lock on
// catalogueStatusMutex before calling this function
Json::Value
generateStatusJson(bool includeErrorInfo)
{
    Json::Value jvResult;

    if (catalogueRunStatus.isRunning)
    {
        jvResult[jss::job_status] = "job_in_progress";
        jvResult[jss::min_ledger] = catalogueRunStatus.minLedger;
        jvResult[jss::max_ledger] = catalogueRunStatus.maxLedger;
        jvResult[jss::current_ledger] = catalogueRunStatus.ledgerUpto;

        // Calculate percentage complete
        uint32_t total_ledgers =
            catalogueRunStatus.maxLedger - catalogueRunStatus.minLedger + 1;

        uint32_t processed_ledgers = (catalogueRunStatus.ledgerUpto == 0)
            ? 0
            : catalogueRunStatus.ledgerUpto - catalogueRunStatus.minLedger + 1;

        if (processed_ledgers > total_ledgers)
            processed_ledgers = total_ledgers;

        int percentage = (total_ledgers > 0)
            ? static_cast<int>((processed_ledgers * 100) / total_ledgers)
            : 0;
        jvResult[jss::percent_complete] = percentage;

        // Calculate elapsed time
        auto now = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                           now - catalogueRunStatus.started)
                           .count();
        jvResult[jss::elapsed_seconds] = static_cast<Json::UInt>(elapsed);

        // Calculate estimated time remaining
        if (processed_ledgers > 0 && total_ledgers > processed_ledgers)
        {
            double ledgers_per_second =
                static_cast<double>(processed_ledgers) / elapsed;

            if (ledgers_per_second > 0)
            {
                uint32_t remaining_ledgers = total_ledgers - processed_ledgers;
                uint64_t estimated_seconds_remaining = static_cast<uint64_t>(
                    remaining_ledgers / ledgers_per_second);

                // Format the time remaining in human-readable form
                std::string time_remaining;
                if (estimated_seconds_remaining > 3600)
                {
                    uint64_t hours = estimated_seconds_remaining / 3600;
                    uint64_t minutes =
                        (estimated_seconds_remaining % 3600) / 60;
                    time_remaining = std::to_string(hours) + " hour" +
                        (hours > 1 ? "s" : "") + " " + std::to_string(minutes) +
                        " minute" + (minutes > 1 ? "s" : "");
                }
                else if (estimated_seconds_remaining > 60)
                {
                    uint64_t minutes = estimated_seconds_remaining / 60;
                    uint64_t seconds = estimated_seconds_remaining % 60;
                    time_remaining = std::to_string(minutes) + " minute" +
                        (minutes > 1 ? "s" : "") + " " +
                        std::to_string(seconds) + " second" +
                        (seconds > 1 ? "s" : "");
                }
                else
                {
                    time_remaining =
                        std::to_string(estimated_seconds_remaining) +
                        " second" +
                        (estimated_seconds_remaining > 1 ? "s" : "");
                }
                jvResult[jss::estimated_time_remaining] = time_remaining;
            }
            else
            {
                jvResult[jss::estimated_time_remaining] = "unknown";
            }
        }
        else
        {
            jvResult[jss::estimated_time_remaining] = "unknown";
        }

        // Add start time as ISO 8601 string
        auto time_t_started =
            std::chrono::system_clock::to_time_t(catalogueRunStatus.started);
        std::tm* tm_started = std::gmtime(&time_t_started);
        char time_buffer[30];
        std::strftime(
            time_buffer, sizeof(time_buffer), "%Y-%m-%dT%H:%M:%SZ", tm_started);
        jvResult[jss::start_time] = time_buffer;

        // Add job type
        jvResult[jss::job_type] =
            (catalogueRunStatus.jobType == CatalogueJobType::CREATE)
            ? "catalogue_create"
            : "catalogue_load";

        // Add filename
        jvResult[jss::file] = catalogueRunStatus.filename;

        // Add compression level if applicable
        if (catalogueRunStatus.compressionLevel > 0)
        {
            jvResult[jss::compression_level] =
                catalogueRunStatus.compressionLevel;
        }

        // Add hash if available
        if (!catalogueRunStatus.hash.empty())
        {
            jvResult[jss::hash] = catalogueRunStatus.hash;
        }

        // Add filesize if available
        if (catalogueRunStatus.filesize > 0)
        {
            jvResult[jss::file_size_human] =
                formatBytesIEC(catalogueRunStatus.filesize);
            jvResult[jss::file_size] =
                std::to_string(catalogueRunStatus.filesize);
        }

        // Add estimated filesize
        jvResult[jss::file_size_estimated_human] =
            catalogueRunStatus.fileSizeEstimated;

        // Add thread count for loading
        if (catalogueRunStatus.loadThreads > 0)
        {
            jvResult["load_threads"] = catalogueRunStatus.loadThreads;
        }

        // Add bytes processed if tracking
        auto bytesProcessed = catalogueRunStatus.fileBytesProcessed.load();
        if (bytesProcessed > 0)
        {
            jvResult["bytes_processed_human"] = formatBytesIEC(bytesProcessed);
            jvResult["bytes_processed"] = std::to_string(bytesProcessed);
        }

        if (includeErrorInfo)
        {
            jvResult[jss::error] = "busy";
            jvResult[jss::error_message] =
                "Another catalogue operation is in progress";
        }
    }
    else
    {
        jvResult[jss::job_status] = "no_job_running";
    }

    return jvResult;
}

// CatalogueSizePredictor implementation
CatalogueSizePredictor::CatalogueSizePredictor(
    uint32_t minLedger,
    uint32_t maxLedger,
    uint64_t headerSize)
    : minLedger_(minLedger)
    , maxLedger_(maxLedger)
    , headerSize_(headerSize)
    , totalBytesWritten_(headerSize)
    , firstLedgerSize_(0)
    , processedLedgers_(0)
{
}

void
CatalogueSizePredictor::addLedger(uint32_t seq, uint64_t bytes)
{
    totalBytesWritten_ += bytes;
    processedLedgers_++;

    if (seq == minLedger_)
    {
        firstLedgerSize_ = bytes;
    }
    else
    {
        recentDeltas_.push_back(bytes);
        if (recentDeltas_.size() > MAX_DELTAS)
            recentDeltas_.pop_front();
    }
}

uint64_t
CatalogueSizePredictor::getEstimate() const
{
    if (recentDeltas_.empty())
    {
        return totalBytesWritten_;
    }

    uint64_t totalDeltaSize = 0;
    for (auto size : recentDeltas_)
        totalDeltaSize += size;

    uint64_t avgDelta = totalDeltaSize / recentDeltas_.size();

    uint32_t totalLedgers = maxLedger_ - minLedger_ + 1;
    uint32_t remainingLedgers = (totalLedgers >= processedLedgers_)
        ? (totalLedgers - processedLedgers_)
        : 0;

    return totalBytesWritten_ + avgDelta * remainingLedgers;
}

std::string
CatalogueSizePredictor::getEstimateHuman() const
{
    auto bytes = getEstimate();
    if (bytes == totalBytesWritten_)
        return totalBytesWritten_ == 0
            ? "unknown"
            : formatBytesIEC(totalBytesWritten_) + "+";
    return formatBytesIEC(bytes);
}

}  // namespace ripple