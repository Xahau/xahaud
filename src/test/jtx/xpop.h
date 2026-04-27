#ifndef RIPPLE_TEST_JTX_XPOP_H_INCLUDED
#define RIPPLE_TEST_JTX_XPOP_H_INCLUDED

#include <test/jtx/Env.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/proof/LedgerProof.h>
#include <xrpld/app/proof/XPOPv1.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/digest.h>

namespace ripple {
namespace test {
namespace jtx {
namespace xpop {

/// Build a manifest string (binary, not base64).
inline std::string
makeManifestRaw(
    PublicKey const& masterPub,
    SecretKey const& masterSec,
    PublicKey const& signingPub,
    SecretKey const& signingSec,
    int seq = 1)
{
    STObject st(sfGeneric);
    st[sfSequence] = seq;
    st[sfPublicKey] = masterPub;
    st[sfSigningPubKey] = signingPub;

    sign(st, HashPrefix::manifest, *publicKeyType(signingPub), signingSec);
    sign(
        st,
        HashPrefix::manifest,
        *publicKeyType(masterPub),
        masterSec,
        sfMasterSignature);

    Serializer s;
    st.add(s);
    return std::string(static_cast<char const*>(s.data()), s.size());
}

/// A complete test validator with all keys and manifest.
struct TestValidator
{
    PublicKey masterPublic;
    SecretKey masterSecret;
    PublicKey signingPublic;
    SecretKey signingSecret;
    std::string manifestRaw;
    std::string manifestBase64;

    static TestValidator
    create()
    {
        auto const ms = randomSecretKey();
        auto const mp = derivePublicKey(KeyType::ed25519, ms);
        auto const [sp, ss] = randomKeyPair(KeyType::secp256k1);
        auto raw = makeManifestRaw(mp, ms, sp, ss, 1);
        return {mp, ms, sp, ss, raw, base64_encode(raw)};
    }

    proof::ValidatorKeys
    toValidatorKeys() const
    {
        return {
            masterPublic,
            masterSecret,
            signingPublic,
            signingSecret,
            manifestBase64};
    }
};

/// A complete test VL publisher with keys and manifest.
struct TestVLPublisher
{
    PublicKey masterPublic;
    SecretKey masterSecret;
    PublicKey signingPublic;
    SecretKey signingSecret;
    std::string manifestBase64;

    static TestVLPublisher
    create()
    {
        auto const ms = randomSecretKey();
        auto const mp = derivePublicKey(KeyType::ed25519, ms);
        auto const [sp, ss] = randomKeyPair(KeyType::secp256k1);
        return {
            mp, ms, sp, ss, base64_encode(makeManifestRaw(mp, ms, sp, ss, 1))};
    }

    /// Build VL data for these validators.
    proof::VLData
    buildVLData(
        std::vector<TestValidator> const& validators,
        std::uint32_t sequence = 1,
        std::uint32_t expiration = 767784645) const
    {
        // Build the JSON blob
        std::string data = "{\"sequence\":" + std::to_string(sequence) +
            ",\"expiration\":" + std::to_string(expiration) +
            ",\"validators\":[";

        for (std::size_t i = 0; i < validators.size(); ++i)
        {
            if (i > 0)
                data += ",";
            data += "{\"validation_public_key\":\"" +
                strHex(validators[i].masterPublic) + "\",\"manifest\":\"" +
                validators[i].manifestBase64 + "\"}";
        }
        data += "]}";

        auto const blob = base64_encode(data);
        auto const sig =
            strHex(sign(signingPublic, signingSecret, makeSlice(data)));

        return proof::VLData{
            masterPublic, masterSecret, manifestBase64, blob, sig, 1};
    }
};

/// Everything needed to build and import XPOPs in tests.
struct TestXPOPContext
{
    std::vector<TestValidator> validators;
    TestVLPublisher publisher;
    proof::VLData vlData;

    static TestXPOPContext
    create(int validatorCount = 5)
    {
        auto pub = TestVLPublisher::create();
        std::vector<TestValidator> vals;
        for (int i = 0; i < validatorCount; ++i)
            vals.push_back(TestValidator::create());
        auto vl = pub.buildVLData(vals);
        return {std::move(vals), std::move(pub), std::move(vl)};
    }

    /// Get the VL master public key hex for IMPORT_VL_KEYS config.
    std::string
    vlKeyHex() const
    {
        return strHex(publisher.masterPublic);
    }

    /// Build an Env config with NETWORK_ID and IMPORT_VL_KEYS set.
    std::unique_ptr<Config>
    makeEnvConfig(std::uint32_t networkID = 21337) const
    {
        auto cfg = envconfig(jtx::validator, "");
        cfg->NETWORK_ID = networkID;
        auto const keyHex = vlKeyHex();
        auto const pkHex = strUnHex(keyHex);
        if (pkHex)
            cfg->IMPORT_VL_KEYS.emplace(keyHex, makeSlice(*pkHex));
        return cfg;
    }

    /// Build XPOP from a closed ledger for a specific tx.
    Json::Value
    buildXPOP(Ledger const& ledger, uint256 const& txHash) const
    {
        std::vector<proof::ValidatorKeys> valKeys;
        for (auto const& v : validators)
            valKeys.push_back(v.toValidatorKeys());
        return proof::buildXPOPv1(ledger, txHash, valKeys, vlData);
    }

    /// Build XPOP from an Env's last closed ledger.
    Json::Value
    buildXPOP(Env& env, uint256 const& txHash) const
    {
        auto const lcl = env.app().getLedgerMaster().getClosedLedger();
        if (!lcl)
            return {};
        return buildXPOP(*lcl, txHash);
    }
};

/// Build a complete XPOP v1 JSON from an Env's last closed ledger.
/// Creates fresh validator keys and VL publisher for each call.
inline Json::Value
buildTestXPOP(Env& env, uint256 const& txHash, int validatorCount = 5)
{
    auto ctx = TestXPOPContext::create(validatorCount);
    return ctx.buildXPOP(env, txHash);
}

/// Get the hex-encoded XPOP blob suitable for sfBlob in ttIMPORT.
inline std::string
buildTestXPOPHex(Env& env, uint256 const& txHash, int validatorCount = 5)
{
    auto const xpop = buildTestXPOP(env, txHash, validatorCount);
    if (xpop.isNull())
        return {};
    return proof::xpopToHex(xpop);
}

}  // namespace xpop
}  // namespace jtx
}  // namespace test
}  // namespace ripple

#endif
