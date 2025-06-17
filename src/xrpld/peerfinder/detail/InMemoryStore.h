#ifndef RIPPLE_PEERFINDER_INMEMORYSTORE_H_INCLUDED
#define RIPPLE_PEERFINDER_INMEMORYSTORE_H_INCLUDED

#include <ripple/beast/net/IPEndpoint.h>
#include <ripple/peerfinder/impl/Store.h>
#include <boost/functional/hash.hpp>
#include <boost/unordered/concurrent_flat_map.hpp>

namespace ripple {
namespace PeerFinder {

struct EndpointHasher
{
    std::size_t
    operator()(beast::IP::Endpoint const& endpoint) const
    {
        std::size_t seed = 0;
        boost::hash_combine(seed, endpoint.address().to_string());
        boost::hash_combine(seed, endpoint.port());
        return seed;
    }
};

class InMemoryStore : public Store
{
private:
    boost::concurrent_flat_map<beast::IP::Endpoint, int, EndpointHasher>
        entries;

public:
    std::size_t
    load(load_callback const& cb) override
    {
        std::size_t count = 0;
        entries.visit_all([&](auto const& entry) {
            cb(entry.first, entry.second);
            ++count;
        });
        return count;
    }

    void
    save(std::vector<Entry> const& v) override
    {
        entries.clear();
        for (auto const& entry : v)
            entries.emplace(entry.endpoint, entry.valence);
    }
};

}  // namespace PeerFinder
}  // namespace ripple

#endif
