#ifndef RIPPLE_APP_PROOF_XPOPV1_H_INCLUDED
#define RIPPLE_APP_PROOF_XPOPV1_H_INCLUDED

#include <xrpld/app/proof/LedgerProof.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>
#include <string>
#include <vector>

namespace ripple {
namespace proof {

/// Validator key pair for signing validations in an XPOP.
struct ValidatorKeys
{
    PublicKey masterPublic;
    SecretKey masterSecret;
    PublicKey signingPublic;  // ephemeral (manifest signing key)
    SecretKey signingSecret;
    std::string manifest;  // base64-encoded manifest
};

/// Validator List (VL) data for the XPOP.
struct VLData
{
    PublicKey masterPublic;  // VL master key
    SecretKey masterSecret;
    std::string manifest;   // base64-encoded VL manifest
    std::string blob;       // base64-encoded VL blob (validator list)
    std::string signature;  // hex signature of the blob
    std::uint32_t version{1};
};

/// Build an XPOP v1 JSON object from a LedgerProof.
///
/// The XPOP format matches what Import.cpp::syntaxCheckXPOP expects:
/// {
///   "ledger": { index, coins, phash, txroot, acroot, pclose, close, cres,
///   flags }, "transaction": { blob, meta, proof }, "validation": { data: {
///   pubkey: validation_hex, ... }, unl: { ... } }
/// }
///
/// @param proof       The ledger proof containing tx blob, meta, and merkle
/// proof
/// @param validators  Validator keys to sign the ledger hash
/// @param vl          Validator list data
/// @return            XPOP as a Json::Value
Json::Value
buildXPOPv1(
    LedgerProof const& proof,
    std::vector<ValidatorKeys> const& validators,
    VLData const& vl);

/// Convenience: build XPOP from a Ledger + tx hash + validator keys.
/// Combines buildLedgerProof + buildXPOPv1.
Json::Value
buildXPOPv1(
    Ledger const& ledger,
    uint256 const& txHash,
    std::vector<ValidatorKeys> const& validators,
    VLData const& vl);

/// Encode an XPOP Json::Value to the hex blob format that ttIMPORT expects
/// in sfBlob.
std::string
xpopToHex(Json::Value const& xpop);

}  // namespace proof
}  // namespace ripple

#endif
