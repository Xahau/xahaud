#include <ripple/basics/contract.h>
#include <ripple/nodestore/Factory.h>
#include <ripple/nodestore/Manager.h>
#include <ripple/nodestore/impl/DecodedBlob.h>
#include <ripple/nodestore/impl/EncodedBlob.h>
#include <ripple/nodestore/impl/codec.h>
#include <boost/beast/core/string.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/unordered/concurrent_flat_map.hpp>
#include <memory>
#include <mutex>

namespace ripple {
namespace NodeStore {

class FlatmapBackend : public Backend
{
private:
    std::string name_;
    beast::Journal journal_;
    bool isOpen_{false};

    struct base_uint_hasher
    {
        using result_type = std::size_t;

        result_type
        operator()(base_uint<256> const& value) const
        {
            return hardened_hash<>{}(value);
        }
    };

    using DataStore = boost::unordered::concurrent_flat_map<
        uint256,
        std::vector<std::uint8_t>,  // Store compressed blob data
        base_uint_hasher>;

    DataStore table_;

public:
    FlatmapBackend(
        size_t keyBytes,
        Section const& keyValues,
        beast::Journal journal)
        : name_(get(keyValues, "path")), journal_(journal)
    {
        boost::ignore_unused(journal_);
        if (name_.empty())
            name_ = "node_db";
    }

    ~FlatmapBackend() override
    {
        close();
    }

    std::string
    getName() override
    {
        return name_;
    }

    void
    open(bool createIfMissing) override
    {
        if (isOpen_)
            Throw<std::runtime_error>("already open");
        isOpen_ = true;
    }

    bool
    isOpen() override
    {
        return isOpen_;
    }

    void
    close() override
    {
        table_.clear();
        isOpen_ = false;
    }

    Status
    fetch(void const* key, std::shared_ptr<NodeObject>* pObject) override
    {
        if (!isOpen_)
            return notFound;

        uint256 const hash(uint256::fromVoid(key));

        bool found = table_.visit(hash, [&](const auto& key_value_pair) {
            nudb::detail::buffer bf;
            auto const result = nodeobject_decompress(
                key_value_pair.second.data(), key_value_pair.second.size(), bf);
            DecodedBlob decoded(hash.data(), result.first, result.second);
            if (!decoded.wasOk())
            {
                *pObject = nullptr;
                return;
            }
            *pObject = decoded.createObject();
        });
        return found ? (*pObject ? ok : dataCorrupt) : notFound;
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
    store(std::shared_ptr<NodeObject> const& object) override
    {
        if (!isOpen_)
            return;

        if (!object)
            return;

        EncodedBlob encoded(object);
        nudb::detail::buffer bf;
        auto const result =
            nodeobject_compress(encoded.getData(), encoded.getSize(), bf);

        std::vector<std::uint8_t> compressed(
            static_cast<const std::uint8_t*>(result.first),
            static_cast<const std::uint8_t*>(result.first) + result.second);

        table_.insert_or_assign(object->getHash(), std::move(compressed));
    }

    void
    storeBatch(Batch const& batch) override
    {
        for (auto const& e : batch)
            store(e);
    }

    void
    sync() override
    {
    }

    void
    for_each(std::function<void(std::shared_ptr<NodeObject>)> f) override
    {
        if (!isOpen_)
            return;

        table_.visit_all([&f](const auto& entry) {
            nudb::detail::buffer bf;
            auto const result = nodeobject_decompress(
                entry.second.data(), entry.second.size(), bf);
            DecodedBlob decoded(
                entry.first.data(), result.first, result.second);
            if (decoded.wasOk())
                f(decoded.createObject());
        });
    }

    int
    getWriteLoad() override
    {
        return 0;
    }

    void
    setDeletePath() override
    {
        close();
    }

    int
    fdRequired() const override
    {
        return 0;
    }

private:
    size_t
    size() const
    {
        return table_.size();
    }
};

class FlatmapFactory : public Factory
{
public:
    FlatmapFactory()
    {
        Manager::instance().insert(*this);
    }

    ~FlatmapFactory() override
    {
        Manager::instance().erase(*this);
    }

    std::string
    getName() const override
    {
        return "Flatmap";
    }

    std::unique_ptr<Backend>
    createInstance(
        size_t keyBytes,
        Section const& keyValues,
        std::size_t burstSize,
        Scheduler& scheduler,
        beast::Journal journal) override
    {
        return std::make_unique<FlatmapBackend>(keyBytes, keyValues, journal);
    }
};

static FlatmapFactory flatmapFactory;

}  // namespace NodeStore
}  // namespace ripple
