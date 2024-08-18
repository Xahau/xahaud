//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <ripple/basics/contract.h>
#include <ripple/nodestore/Factory.h>
#include <ripple/nodestore/Manager.h>
#include <ripple/nodestore/impl/DecodedBlob.h>
#include <ripple/nodestore/impl/EncodedBlob.h>
#include <ripple/nodestore/impl/codec.h>
#include <boost/filesystem.hpp>
#include "snug.hpp"
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>

namespace ripple {
namespace NodeStore {

class SnugDBBackend : public Backend
{
private:
    static constexpr uint64_t BUFFER_SIZE =
        256ULL * 1024ULL * 1024ULL;  // 256 Mib read buffer per thread
public:
    beast::Journal const j_;
    std::string const name_;
    std::unique_ptr<snug::SnugDB> db_;
    Scheduler& scheduler_;

    SnugDBBackend(
        Section const& keyValues,
        Scheduler& scheduler,
        beast::Journal journal)
        : j_(journal), name_(get(keyValues, "path")), scheduler_(scheduler)
    {
        if (name_.empty())
            throw std::runtime_error(
                "nodestore: Missing path in SnugDB backend");
    }

    ~SnugDBBackend() override
    {
        try
        {
            // close can throw and we don't want the destructor to throw.
            db_ = nullptr;
        }
        catch (std::exception const& e)
        {
            JLOG(j_.warn()) << "SnugDB threw on destruction: " << e.what();
            // Don't allow exceptions to propagate out of destructors.
        }
    }

    std::string
    getName() override
    {
        return name_;
    }

    void
    open(bool createIfMissing, uint64_t appType, uint64_t uid, uint64_t salt)
        override
    {
        if (db_)
        {
            assert(false);
            JLOG(j_.error()) << "database is already open";
            return;
        }

        std::string path = name_ + "/" + std::to_string(uid) + "-" +
            std::to_string(appType) + "-" + std::to_string(salt);

        boost::filesystem::create_directories(path);
        db_ = std::make_unique<snug::SnugDB>(path);
    }

    bool
    isOpen() override
    {
        return db_ != nullptr;
    }

    void
    open(bool createIfMissing) override
    {
        open(createIfMissing, 0, 0, 0);
    }

    void
    close() override
    {
        db_ = nullptr;
    }

    Status
    fetch(void const* key, std::shared_ptr<NodeObject>* pno) override
    {
        if (!db_)
            return backendError;

        pno->reset();

        static thread_local std::unique_ptr<uint8_t[]> thread_buffer =
            std::make_unique<uint8_t[]>(BUFFER_SIZE);

        uint8_t* ptr = &(thread_buffer[0]);

        uint64_t len = BUFFER_SIZE;
        int result = db_->read_entry(
            static_cast<uint8_t*>(const_cast<void*>(key)), ptr, &len);

        if (0)
        {
            std::stringstream ss;
            const unsigned char* bytes = static_cast<const unsigned char*>(key);
            for (int i = 0; i < 32; ++i)
            {
                ss << std::setfill('0') << std::setw(2) << std::hex
                   << static_cast<int>(bytes[i]);
            }
            std::string key_hex = ss.str();

            // Print the result using printf
            printf(
                "snug fetch: len=%zu result=%zu key=%s\n",
                len,
                result,
                key_hex.c_str());
        }

        if (result == 1)
            return notFound;

        if (result == 0)
        {
            DecodedBlob decoded(key, ptr, len);
            if (!decoded.wasOk())
                return dataCorrupt;

            *pno = decoded.createObject();
            return ok;
        }

        return backendError;
    }

    std::pair<std::vector<std::shared_ptr<NodeObject>>, Status>
    fetchBatch(std::vector<uint256 const*> const& hashes) override
    {
        std::vector<std::shared_ptr<NodeObject>> results;
        results.reserve(hashes.size());
        for (auto const& h : hashes)
        {
            std::shared_ptr<NodeObject> nObj;
            Status status = fetch(h->begin(), &nObj);
            if (status != ok)
                results.push_back({});
            else
                results.push_back(nObj);
        }

        return {results, ok};
    }

    void
    do_insert(std::shared_ptr<NodeObject> const& no)
    {
        EncodedBlob e(no);

        if (0)
        {
            std::stringstream ss;
            const unsigned char* bytes = static_cast<const unsigned char*>(
                const_cast<void*>(e.getKey()));
            for (int i = 0; i < 32; ++i)
                ss << std::setfill('0') << std::setw(2) << std::hex
                   << static_cast<int>(bytes[i]);
            std::string key_hex = ss.str();

            std::cout << "snugdb write: len=" << e.getSize()
                      << ", key=" << key_hex << "\n";
        }
        int out = db_->write_entry(
            static_cast<uint8_t*>(const_cast<void*>(e.getKey())),
            static_cast<uint8_t*>(const_cast<void*>(e.getData())),
            e.getSize());
        if (out != 0)
            throw std::runtime_error(
                "SnugDB could not write entry. Disk full? error" +
                std::to_string(out));
    }

    void
    store(std::shared_ptr<NodeObject> const& no) override
    {
        BatchWriteReport report;
        report.writeCount = 1;
        auto const start = std::chrono::steady_clock::now();
        do_insert(no);
        report.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        scheduler_.onBatchWrite(report);
    }

    void
    storeBatch(Batch const& batch) override
    {
        BatchWriteReport report;
        report.writeCount = batch.size();
        auto const start = std::chrono::steady_clock::now();
        for (auto const& e : batch)
            do_insert(e);
        report.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        scheduler_.onBatchWrite(report);
    }

    void
    sync() override
    {
    }

    void
    for_each(std::function<void(std::shared_ptr<NodeObject>)> f) override
    {
        db_->visit_all(
            [](uint8_t* key, uint8_t* data, uint64_t len, void* fp) -> void {
                DecodedBlob decoded(key, data, len);
                if (!decoded.wasOk())
                {
                    throw std::runtime_error(
                        "Missing or corrupted data in snugdb");
                    return;
                }

                std::function<void(std::shared_ptr<NodeObject>)> f =
                    *(reinterpret_cast<
                        std::function<void(std::shared_ptr<NodeObject>)>*>(fp));
                f(decoded.createObject());
            },
            reinterpret_cast<void*>(&f));
    }

    int
    getWriteLoad() override
    {
        return 0;
    }

    void
    setDeletePath() override
    {
    }

    void
    verify() override
    {
    }

    int
    fdRequired() const override
    {
        return 3;
    }
};

//------------------------------------------------------------------------------

class SnugDBFactory : public Factory
{
public:
    SnugDBFactory()
    {
        Manager::instance().insert(*this);
    }

    ~SnugDBFactory() override
    {
        Manager::instance().erase(*this);
    }

    std::string
    getName() const override
    {
        return "SnugDB";
    }

    std::unique_ptr<Backend>
    createInstance(
        size_t keyBytes,
        Section const& keyValues,
        std::size_t burstSize,
        Scheduler& scheduler,
        beast::Journal journal) override
    {
        return std::make_unique<SnugDBBackend>(keyValues, scheduler, journal);
    }

    std::unique_ptr<Backend>
    createInstance(
        size_t keyBytes,
        Section const& keyValues,
        std::size_t burstSize,
        Scheduler& scheduler,
        nudb::context& context,
        beast::Journal journal) override
    {
        return std::make_unique<SnugDBBackend>(keyValues, scheduler, journal);
    }
};

static SnugDBFactory snugDBFactory;

}  // namespace NodeStore
}  // namespace ripple
