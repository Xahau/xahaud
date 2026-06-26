#include <xrpld/app/proof/ProofBuilder.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/digest.h>

namespace ripple {
namespace proof {

// --- MerkleProof ---

std::optional<uint256>
MerkleProof::computeRoot() const
{
    if (path.empty())
        return std::nullopt;

    // Start with the leaf hash and work up through the path.
    uint256 currentHash = leafHash;

    // Path is root-to-leaf, so iterate in reverse.
    for (auto it = path.rbegin(); it != path.rend(); ++it)
    {
        sha512_half_hasher h;
        using beast::hash_append;
        hash_append(h, HashPrefix::innerNode);

        for (int i = 0; i < 16; ++i)
        {
            if (i == it->targetBranch)
                hash_append(h, currentHash);
            else
                hash_append(h, it->branches[i]);
        }

        currentHash = static_cast<uint256>(h);
    }

    return currentHash;
}

bool
MerkleProof::verify(uint256 const& expectedRoot) const
{
    auto const computed = computeRoot();
    return computed && *computed == expectedRoot;
}

Json::Value
MerkleProof::toJsonV1() const
{
    // Build the nested array format that Import.cpp expects.
    // Start from the deepest level and work up.
    // Each level is an array of 16 entries: 15 hex hashes + 1 nested
    // array (or the leaf hash at the bottom level).

    // Build bottom-up: the deepest ProofNode's target branch
    // contains the leaf hash directly.
    Json::Value current;

    for (auto it = path.rbegin(); it != path.rend(); ++it)
    {
        Json::Value level(Json::arrayValue);
        for (int i = 0; i < 16; ++i)
        {
            if (i == it->targetBranch)
            {
                if (it == path.rbegin())
                {
                    // Deepest level: the target branch IS the leaf hash
                    level.append(to_string(leafHash));
                }
                else
                {
                    // Inner level: the target branch is the nested proof
                    level.append(current);
                }
            }
            else
            {
                level.append(to_string(it->branches[i]));
            }
        }
        current = level;
    }

    return current;
}

// --- extractProof ---

namespace {

/// Simple in-memory radix trie node for proof construction.
struct TrieNode
{
    uint256 hash;
    uint256 key;
    std::array<std::unique_ptr<TrieNode>, 16> children;
    bool isLeaf{false};

    bool
    hasChildren() const
    {
        for (auto const& c : children)
            if (c)
                return true;
        return false;
    }

    int
    childCount() const
    {
        int count = 0;
        for (auto const& c : children)
            if (c)
                ++count;
        return count;
    }
};

/// Get the nibble at position `depth` in a 256-bit key.
int
getNibble(uint256 const& key, int depth)
{
    // Each byte has 2 nibbles. depth 0 = high nibble of byte 0.
    int byteIdx = depth / 2;
    int nibbleIdx = depth % 2;
    auto byte = key.data()[byteIdx];
    return nibbleIdx == 0 ? (byte >> 4) & 0x0F : byte & 0x0F;
}

/// Insert a leaf into the trie.
void
trieInsert(TrieNode& root, uint256 const& key, uint256 const& leafHash)
{
    TrieNode* node = &root;
    int depth = 0;

    while (depth < 64)  // 256 bits / 4 bits per nibble = 64 max depth
    {
        int nibble = getNibble(key, depth);

        if (!node->children[nibble])
        {
            // Empty slot — insert leaf here.
            node->children[nibble] = std::make_unique<TrieNode>();
            node->children[nibble]->hash = leafHash;
            node->children[nibble]->key = key;
            node->children[nibble]->isLeaf = true;
            return;
        }

        if (node->children[nibble]->isLeaf)
        {
            // Collision — need to split.
            auto existing = std::move(node->children[nibble]);
            node->children[nibble] = std::make_unique<TrieNode>();
            auto* newInner = node->children[nibble].get();

            // Re-insert the existing leaf deeper.
            int existingNibble = getNibble(existing->key, depth + 1);
            newInner->children[existingNibble] = std::move(existing);

            // Continue inserting the new leaf.
            node = newInner;
            ++depth;
            continue;
        }

        // Inner node — descend.
        node = node->children[nibble].get();
        ++depth;
    }
}

/// Compute hashes bottom-up for the trie.
uint256
trieComputeHash(TrieNode& node)
{
    if (node.isLeaf)
        return node.hash;

    sha512_half_hasher h;
    using beast::hash_append;
    hash_append(h, HashPrefix::innerNode);

    for (int i = 0; i < 16; ++i)
    {
        if (node.children[i])
            hash_append(h, trieComputeHash(*node.children[i]));
        else
            hash_append(h, uint256{});
    }

    node.hash = static_cast<uint256>(h);
    return node.hash;
}

/// Extract proof path from root to a specific key.
bool
trieExtractProof(
    TrieNode const& node,
    uint256 const& key,
    int depth,
    std::vector<ProofNode>& path,
    uint256& outLeafHash)
{
    if (node.isLeaf)
    {
        if (node.key == key)
        {
            outLeafHash = node.hash;
            return true;
        }
        return false;
    }

    int nibble = getNibble(key, depth);
    if (!node.children[nibble])
        return false;

    ProofNode pn;
    pn.targetBranch = static_cast<std::uint8_t>(nibble);

    for (int i = 0; i < 16; ++i)
    {
        if (node.children[i])
            pn.branches[i] = node.children[i]->hash;
        // else: already zero-initialized
    }

    if (node.children[nibble]->isLeaf)
    {
        if (node.children[nibble]->key != key)
            return false;
        pn.isLeafParent = true;
        path.push_back(pn);
        outLeafHash = node.children[nibble]->hash;
        return true;
    }

    path.push_back(pn);
    return trieExtractProof(
        *node.children[nibble], key, depth + 1, path, outLeafHash);
}

}  // namespace

std::optional<MerkleProof>
extractProofV1(SHAMap const& map, uint256 const& key)
{
    // Build an in-memory trie from all SHAMap leaves.
    // V1 leaf hash: SHA512Half(txNode prefix + item_data + item_key)
    TrieNode root;

    map.visitLeaves([&](boost::intrusive_ptr<SHAMapItem const> const& item) {
        auto const itemHash =
            sha512Half(HashPrefix::txNode, item->slice(), item->key());
        trieInsert(root, item->key(), itemHash);
    });

    trieComputeHash(root);

    MerkleProof proof;
    proof.key = key;

    if (!trieExtractProof(root, key, 0, proof.path, proof.leafHash))
        return std::nullopt;

    return proof;
}

}  // namespace proof
}  // namespace ripple
