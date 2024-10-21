#ifndef RIPPLE_PEERFINDER_INMEMORYSTORE_H_INCLUDED
#define RIPPLE_PEERFINDER_INMEMORYSTORE_H_INCLUDED

#include <ripple/peerfinder/impl/Store.h>
#include <vector>

namespace ripple {
namespace PeerFinder {

class InMemoryStore : public Store
{
private:
    std::vector<Entry> entries;

public:
    std::size_t
    load(load_callback const& cb) override
    {
        for (auto const& entry : entries)
        {
            cb(entry.endpoint, entry.valence);
        }
        return entries.size();
    }

    void
    save(std::vector<Entry> const& v) override
    {
        entries = v;
    }
};

}  // namespace PeerFinder
}  // namespace ripple

#endif
