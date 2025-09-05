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

#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/main/Application.h>
#include <ripple/basics/Log.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/digest.h>
#include <ripple/rpc/handlers/Catalogue.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <fstream>
#include <sys/stat.h>

namespace ripple {

Json::Value
doCatalogueCreate(RPC::JsonContext& context)
{
    auto j = context.app.logs().journal("CatalogueTools");

    // Try to acquire write lock to check if an operation is running
    {
        std::unique_lock<std::shared_mutex> writeLock(
            catalogueStatusMutex, std::try_to_lock);
        if (!writeLock.owns_lock())
        {
            // Couldn't get the lock, so another thread is accessing the status
            // Try a shared lock to get the status
            std::shared_lock<std::shared_mutex> readLock(catalogueStatusMutex);
            return generateStatusJson(true);
        }

        // We have the write lock, check if an operation is already running
        if (catalogueRunStatus.isRunning)
        {
            return generateStatusJson(true);
        }

        // No operation running, set up our operation
        catalogueRunStatus.isRunning = true;
    }
    // Write lock is released here, allowing status checks while operation runs

    // Ensure we reset the running flag when we're done
    struct OpCleanup
    {
        ~OpCleanup()
        {
            std::unique_lock<std::shared_mutex> writeLock(catalogueStatusMutex);
            catalogueRunStatus.isRunning = false;
        }
    } opCleanup;

    if (!context.params.isMember(jss::min_ledger) ||
        !context.params.isMember(jss::max_ledger))
        return rpcError(
            rpcINVALID_PARAMS, "expected min_ledger and max_ledger");

    std::string filepath;
    struct stat st;
    uint64_t file_size = 0;

    if (!context.params.isMember(jss::output_file) ||
        (filepath = context.params[jss::output_file].asString()).empty() ||
        filepath.front() != '/')
        return rpcError(
            rpcINVALID_PARAMS,
            "expected output_file: <absolute writeable filepath>");

    uint8_t compressionLevel = 0;  // Default: no compression

    if (context.params.isMember(jss::compression_level))
    {
        if (context.params[jss::compression_level].isInt() ||
            context.params[jss::compression_level].isUInt())
        {
            // Handle numeric value between 0 and 9
            compressionLevel = context.params[jss::compression_level].asUInt();
            if (compressionLevel > 9)
                compressionLevel = 9;
        }
        else if (context.params[jss::compression_level].isBool())
        {
            // Handle boolean: true means 6, false means 0
            compressionLevel =
                context.params[jss::compression_level].asBool() ? 6 : 0;
        }
    }

    // Check output file isn't already populated and can be written to
    {
        struct stat st;
        if (stat(filepath.c_str(), &st) == 0)
        {  // file exists
            if (st.st_size > 0)
                return rpcError(
                    rpcINVALID_PARAMS,
                    "output_file already exists and is non-empty");
        }
        else if (errno != ENOENT)
            return rpcError(
                rpcINTERNAL,
                "cannot stat output_file: " + std::string(strerror(errno)));

        std::ofstream testWrite(filepath.c_str(), std::ios::out);
        if (testWrite.fail())
            return rpcError(
                rpcINTERNAL,
                "output_file location is not writeable: " +
                    std::string(strerror(errno)));
        testWrite.close();
    }

    std::ofstream outfile(filepath.c_str(), std::ios::out | std::ios::binary);
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to open output_file: " + std::string(strerror(errno)));

    uint32_t min_ledger = context.params[jss::min_ledger].asUInt();
    uint32_t max_ledger = context.params[jss::max_ledger].asUInt();

    if (min_ledger > max_ledger)
        return rpcError(rpcINVALID_PARAMS, "min_ledger must be <= max_ledger");

    // Initialize status tracking
    {
        std::unique_lock<std::shared_mutex> writeLock(catalogueStatusMutex);
        catalogueRunStatus.isRunning = true;
        catalogueRunStatus.started = std::chrono::system_clock::now();
        catalogueRunStatus.minLedger = min_ledger;
        catalogueRunStatus.maxLedger = max_ledger;
        catalogueRunStatus.ledgerUpto =
            0;  // Initialize to 0 to indicate no progress yet
        catalogueRunStatus.jobType = CatalogueStatusJobType::CREATE;
        catalogueRunStatus.filename = filepath;
        catalogueRunStatus.compressionLevel = compressionLevel;
        catalogueRunStatus.hash.clear();  // No hash yet
    }

    // Create and write header with zero hash
    CATLHeader header;
    header.min_ledger = min_ledger;
    header.max_ledger = max_ledger;
    header.version =
        makeCatalogueVersionField(CATALOGUE_VERSION, compressionLevel);
    header.network_id = context.app.config().NETWORK_ID;
    // hash is already zero-initialized

    outfile.write(reinterpret_cast<const char*>(&header), sizeof(CATLHeader));
    if (outfile.fail())
        return rpcError(
            rpcINTERNAL,
            "failed to write header: " + std::string(strerror(errno)));

    auto compStream = std::make_unique<boost::iostreams::filtering_ostream>();
    if (compressionLevel > 0)
    {
        JLOG(j.info()) << "Setting up compression with level "
                       << (int)compressionLevel;

        boost::iostreams::zlib_params params((int)compressionLevel);
        params.window_bits = 15;
        params.noheader = false;
        compStream->push(boost::iostreams::zlib_compressor(params));
    }
    else
    {
        JLOG(j.info()) << "No compression (level 0), using direct output";
    }

    ByteCounterFilter byteCounter;
    compStream->push(boost::ref(byteCounter));

    compStream->push(boost::ref(outfile));

    // Process ledgers with local processor implementation
    auto writeToFile = [&j, &compStream](const void* data, size_t size) {
        compStream->write(reinterpret_cast<const char*>(data), size);
        if (compStream->fail())
        {
            JLOG(j.error())
                << "Failed to write to output file: " << std::strerror(errno);
            return false;
        }
        return true;
    };

    CatalogueSizePredictor predictor(
        header.min_ledger, header.max_ledger, sizeof(CATLHeader));

    // Modified outputLedger to work with individual ledgers instead of a vector
    auto outputLedger =
        [&j, &writeToFile, &compStream, &predictor, &byteCounter](
            const std::shared_ptr<Ledger const>& ledger,
            std::optional<std::reference_wrapper<const SHAMap>> prevStateMap =
                std::nullopt) -> bool {
        try
        {
            byteCounter.resetCounter();
            auto const& info = ledger->info();

            uint64_t closeTime = info.closeTime.time_since_epoch().count();
            uint64_t parentCloseTime =
                info.parentCloseTime.time_since_epoch().count();
            uint32_t closeTimeResolution = info.closeTimeResolution.count();
            uint64_t drops = info.drops.drops();

            // Write ledger header information
            if (!writeToFile(&info.seq, sizeof(info.seq)) ||
                !writeToFile(info.hash.data(), 32) ||
                !writeToFile(info.txHash.data(), 32) ||
                !writeToFile(info.accountHash.data(), 32) ||
                !writeToFile(info.parentHash.data(), 32) ||
                !writeToFile(&drops, sizeof(drops)) ||
                !writeToFile(&info.closeFlags, sizeof(info.closeFlags)) ||
                !writeToFile(
                    &closeTimeResolution, sizeof(closeTimeResolution)) ||
                !writeToFile(&closeTime, sizeof(closeTime)) ||
                !writeToFile(&parentCloseTime, sizeof(parentCloseTime)))
            {
                return false;
            }

            size_t stateNodesWritten =
                ledger->stateMap().serializeToStream(*compStream, prevStateMap);
            size_t txNodesWritten =
                ledger->txMap().serializeToStream(*compStream);

            predictor.addLedger(info.seq, byteCounter.getBytesWritten());

            JLOG(j.info()) << "Ledger " << info.seq << ": Wrote "
                           << stateNodesWritten << " state nodes, "
                           << "and " << txNodesWritten << " tx nodes";

            return true;
        }
        catch (std::exception const& e)
        {
            JLOG(j.error()) << "Error processing ledger " << ledger->info().seq
                            << ": " << e.what();
            return false;
        }
    };

    // Instead of loading all ledgers at once, process them in a sliding window
    // of two
    std::shared_ptr<Ledger const> prevLedger = nullptr;
    std::shared_ptr<Ledger const> currLedger = nullptr;
    uint32_t ledgers_written = 0;

    JLOG(j.info()) << "Starting to stream ledgers from " << min_ledger << " to "
                   << max_ledger;

    // Process the first ledger completely
    {
        UPDATE_CATALOGUE_STATUS(ledgerUpto, min_ledger);

        // Load the first ledger
        if (auto error = RPC::getLedger(currLedger, min_ledger, context))
            return rpcError(error.toErrorCode(), error.message());
        if (!currLedger)
            return rpcError(rpcLEDGER_MISSING);

        if (!outputLedger(currLedger))
            return rpcError(
                rpcINTERNAL, "Error occurred while processing first ledger");

        ledgers_written++;
        prevLedger = currLedger;
    }

    // Process remaining ledgers with diffs
    for (uint32_t ledger_seq = min_ledger + 1; ledger_seq <= max_ledger;
         ++ledger_seq)
    {
        if (context.app.isStopping())
            return {};

        // Update current ledger in status
        UPDATE_CATALOGUE_STATUS(ledgerUpto, ledger_seq);

        // Load the next ledger
        currLedger = nullptr;  // Release any previous current ledger
        if (auto error = RPC::getLedger(currLedger, ledger_seq, context))
            return rpcError(error.toErrorCode(), error.message());
        if (!currLedger)
            return rpcError(rpcLEDGER_MISSING);

        // Process with diff against previous ledger
        if (!outputLedger(currLedger, prevLedger->stateMap()))
            return rpcError(
                rpcINTERNAL, "Error occurred while processing ledgers");

        UPDATE_CATALOGUE_STATUS(
            fileSizeEstimated, predictor.getEstimateHuman());

        ledgers_written++;

        // Cycle the ledgers: current becomes previous, we'll load a new current
        // next iteration
        prevLedger = currLedger;
    }

    // flush and finish
    compStream->flush();
    compStream->reset();
    outfile.flush();
    outfile.close();

    // Clear ledger references to release memory
    prevLedger = nullptr;
    currLedger = nullptr;

    // Get the file size and update it in the header
    if (stat(filepath.c_str(), &st) != 0)
    {
        JLOG(j.warn()) << "Could not get file size: " << std::strerror(errno);
        return rpcError(
            rpcINTERNAL, "failed to get file size for header update");
    }

    file_size = st.st_size;

    // Update header with filesize
    JLOG(j.info()) << "Updating file size in header: "
                   << std::to_string(file_size) << " bytes";

    header.filesize = file_size;
    std::fstream updateFileSizeFile(
        filepath.c_str(), std::ios::in | std::ios::out | std::ios::binary);
    if (updateFileSizeFile.fail())
        return rpcError(
            rpcINTERNAL,
            "cannot open file for updating filesize: " +
                std::string(strerror(errno)));

    updateFileSizeFile.seekp(0, std::ios::beg);
    updateFileSizeFile.write(
        reinterpret_cast<const char*>(&header), sizeof(CATLHeader));
    updateFileSizeFile.close();

    // Now compute the hash over the entire file
    JLOG(j.info()) << "Computing catalogue hash...";

    std::ifstream hashFile(filepath.c_str(), std::ios::in | std::ios::binary);
    if (hashFile.fail())
        return rpcError(
            rpcINTERNAL,
            "cannot open file for hashing: " + std::string(strerror(errno)));

    // Initialize hasher
    sha512_hasher hasher;

    // Create a buffer for reading
    std::vector<char> buffer(64 * 1024);  // 64K buffer

    // Read and process the header portion
    hashFile.read(buffer.data(), sizeof(CATLHeader));
    if (hashFile.gcount() != sizeof(CATLHeader))
        return rpcError(rpcINTERNAL, "failed to read header for hashing");

    // Zero out the hash portion in the buffer for hash calculation
    std::fill(
        buffer.data() + offsetof(CATLHeader, hash),
        buffer.data() + offsetof(CATLHeader, hash) + sizeof(header.hash),
        0);

    // Add the modified header to the hash
    hasher(buffer.data(), sizeof(CATLHeader));

    // Read and hash the rest of the file
    while (hashFile)
    {
        hashFile.read(buffer.data(), buffer.size());
        std::streamsize bytes_read = hashFile.gcount();
        if (bytes_read > 0)
            hasher(buffer.data(), bytes_read);
    }
    hashFile.close();

    // Get the hash result
    auto hash_result = static_cast<sha512_hasher::result_type>(hasher);

    // Update the hash in the file
    std::fstream updateFile(
        filepath.c_str(), std::ios::in | std::ios::out | std::ios::binary);
    if (updateFile.fail())
        return rpcError(
            rpcINTERNAL,
            "cannot open file for updating hash: " +
                std::string(strerror(errno)));

    updateFile.seekp(offsetof(CATLHeader, hash), std::ios::beg);
    updateFile.write(
        reinterpret_cast<const char*>(hash_result.data()), hash_result.size());
    updateFile.close();

    // Convert hash to hex string
    std::string hash_hex = toHexString(hash_result.data(), hash_result.size());

    // Update status with hash and filesize
    UPDATE_CATALOGUE_STATUS(hash, hash_hex);
    UPDATE_CATALOGUE_STATUS(filesize, file_size);

    Json::Value jvResult;
    jvResult[jss::min_ledger] = min_ledger;
    jvResult[jss::max_ledger] = max_ledger;
    jvResult[jss::output_file] = filepath;
    jvResult[jss::file_size_human] = formatBytesIEC(file_size);
    jvResult[jss::file_size] = std::to_string(file_size);
    jvResult[jss::ledgers_written] = static_cast<Json::UInt>(ledgers_written);
    jvResult[jss::status] = jss::success;
    jvResult[jss::compression_level] = compressionLevel;
    jvResult[jss::hash] = hash_hex;

    return jvResult;
}

}  // namespace ripple