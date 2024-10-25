#include <ripple/basics/contract.h>
#include <ripple/nodestore/Factory.h>
#include <ripple/nodestore/Manager.h>
#include <boost/beast/core/string.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/unordered/concurrent_flat_map.hpp>
#include <map>
#include <memory>
#include <mutex>

namespace ripple {

struct base_uint_hasher
{
    using result_type = std::size_t;

    result_type
    operator()(base_uint<256> const& value) const
    {
        return hardened_hash<>{}(value);
    }
};

namespace NodeStore {

struct MemoryDB
{
    explicit MemoryDB() = default;

    std::mutex mutex;
    bool open = false;
    boost::unordered::concurrent_flat_map<
        uint256,
        std::shared_ptr<NodeObject>,
        base_uint_hasher>
        table;
};

class MemoryFactory : public Factory
{
private:
    std::mutex mutex_;
    std::map<std::string, MemoryDB, boost::beast::iless> map_;

public:
    MemoryFactory();
    ~MemoryFactory() override;

    std::string
    getName() const override;

    std::unique_ptr<Backend>
    createInstance(
        size_t keyBytes,
        Section const& keyValues,
        std::size_t burstSize,
        Scheduler& scheduler,
        beast::Journal journal) override;

    MemoryDB&
    open(std::string const& path)
    {
        std::lock_guard _(mutex_);
        auto const result = map_.emplace(
            std::piecewise_construct, std::make_tuple(path), std::make_tuple());
        MemoryDB& db = result.first->second;
        if (db.open)
            Throw<std::runtime_error>("already open");
        return db;
    }
};

static MemoryFactory memoryFactory;

//------------------------------------------------------------------------------

class MemoryBackend : public Backend
{
private:
    using Map = boost::unordered::concurrent_flat_map<
        uint256,
        std::shared_ptr<NodeObject>,
        base_uint_hasher>;

    std::string name_;
    beast::Journal const journal_;
    MemoryDB* db_{nullptr};

public:
    MemoryBackend(
        size_t keyBytes,
        Section const& keyValues,
        beast::Journal journal)
        : name_(get(keyValues, "path")), journal_(journal)
    {
        boost::ignore_unused(journal_);  // Keep unused journal_ just in case.
        if (name_.empty())
            name_ = "node_db";
    }

    ~MemoryBackend() override
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
        db_ = &memoryFactory.open(name_);
    }

    bool
    isOpen() override
    {
        return static_cast<bool>(db_);
    }

    void
    close() override
    {
        db_ = nullptr;
    }

    //--------------------------------------------------------------------------

    Status
    fetch(void const* key, std::shared_ptr<NodeObject>* pObject) override
    {
        assert(db_);
        uint256 const hash(uint256::fromVoid(key));

        bool found = db_->table.visit(hash, [&](const auto& key_value_pair) {
            *pObject = key_value_pair.second;
        });

        return found ? ok : notFound;
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
        assert(db_);
        db_->table.insert_or_assign(object->getHash(), object);
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
        assert(db_);
        db_->table.visit_all([&f](const auto& entry) { f(entry.second); });
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

    int
    fdRequired() const override
    {
        return 0;
    }
};

//------------------------------------------------------------------------------

MemoryFactory::MemoryFactory()
{
    Manager::instance().insert(*this);
}

MemoryFactory::~MemoryFactory()
{
    Manager::instance().erase(*this);
}

std::string
MemoryFactory::getName() const
{
    return "Memory";
}

std::unique_ptr<Backend>
MemoryFactory::createInstance(
    size_t keyBytes,
    Section const& keyValues,
    std::size_t,
    Scheduler& scheduler,
    beast::Journal journal)
{
    return std::make_unique<MemoryBackend>(keyBytes, keyValues, journal);
}

}  // namespace NodeStore
}  // namespace ripple
