#ifndef RIPPLE_APP_PROOF_PROOFBUILDER_H_INCLUDED
#define RIPPLE_APP_PROOF_PROOFBUILDER_H_INCLUDED

#include <xrpld/shamap/SHAMap.h>
#include <xrpl/basics/base_uint.h>
#include <array>
#include <optional>
#include <vector>

namespace ripple {
namespace proof {

/// A single node in a binary trie merkle proof.
/// Each inner node has 16 branches (SHAMap is a 16-way radix trie).
/// The target branch contains either a child ProofNode or is the leaf.
/// Non-target branches store their hash (for reconstruction).
struct ProofNode
{
    /// Branch hashes. All 16 branches are present.
    /// The target branch is identified by `targetBranch`.
    std::array<uint256, 16> branches;

    /// Which branch (0-15) leads deeper toward the target leaf.
    /// Only meaningful for inner nodes in the path.
    std::uint8_t targetBranch{0};

    /// True if this is the deepest inner node (branches[targetBranch]
    /// is the leaf hash, not another inner node).
    bool isLeafParent{false};
};

/// A merkle proof: path from root to a specific leaf in a SHAMap.
/// Verifiable by hashing from leaf up through each ProofNode.
struct MerkleProof
{
    /// The key (hash) of the target item.
    uint256 key;

    /// The leaf hash (hash of the item's content).
    uint256 leafHash;

    /// Path of inner nodes from root to the leaf's parent.
    std::vector<ProofNode> path;

    /// Reconstruct the root hash from this proof.
    /// Returns nullopt if the proof is malformed.
    std::optional<uint256>
    computeRoot() const;

    /// Verify this proof against an expected root hash.
    bool
    verify(uint256 const& expectedRoot) const;

    /// Serialize to JSON (v1 compatibility — nested arrays).
    Json::Value
    toJsonV1() const;
};

/// Extract a merkle proof for a specific item from a SHAMap.
/// V1 leaf hash: SHA512Half(txNode prefix + item_data + item_key).
/// Returns nullopt if the item is not in the map.
std::optional<MerkleProof>
extractProofV1(SHAMap const& map, uint256 const& key);

}  // namespace proof
}  // namespace ripple

#endif
