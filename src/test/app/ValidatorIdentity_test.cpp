//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.
*/
//==============================================================================

#include <test/app/ValidatorIdentity_test_certs.h>
#include <test/jtx.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/tx/detail/ValidatorIdentity.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/digest.h>

#include <openssl/evp.h>

#include <array>
#include <charconv>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace ripple {
namespace test {
namespace {

using namespace validator_identity_fixtures;

// Mid-validity NetClock time inside the fixture cert window (2020-01-01Z).
constexpr NetClock::time_point kLedgerTime{NetClock::duration{631152000}};

// Claimed attestation window fully inside the fixture certificate validity.
constexpr std::uint64_t kNotBefore = certNotBeforeNet;
constexpr std::uint64_t kNotAfter = certNotAfterNet;

using EVPKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using EVPMdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

template <std::size_t N>
Slice
asSlice(std::array<std::uint8_t, N> const& a)
{
    return {a.data(), a.size()};
}

Blob
lengthPrefixedChain(std::initializer_list<Slice> certificates)
{
    Blob out;
    for (auto const cert : certificates)
    {
        out.push_back(static_cast<std::uint8_t>((cert.size() >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(cert.size() & 0xff));
        out.insert(out.end(), cert.begin(), cert.end());
    }
    return out;
}

Blob
lengthPrefixedChain(Slice leafDer)
{
    return lengthPrefixedChain({leafDer});
}

/** UINT64 fields without sMD_BaseTen are hex-encoded in JSON. */
std::string
hexU64(std::uint64_t value)
{
    std::string out(16, '\0');
    auto const [ptr, ec] =
        std::to_chars(out.data(), out.data() + out.size(), value, 16);
    if (ec != std::errc())
        Throw<std::runtime_error>("hexU64 failed");
    out.resize(static_cast<std::size_t>(ptr - out.data()));
    return out;
}

Blob
signWithLeafKey(Slice pkcs8Der, Slice message)
{
    auto const* cursor =
        reinterpret_cast<unsigned char const*>(pkcs8Der.data());
    EVPKeyPtr key{
        d2i_AutoPrivateKey(nullptr, &cursor, pkcs8Der.size()), EVP_PKEY_free};
    if (!key || cursor != pkcs8Der.data() + pkcs8Der.size())
        Throw<std::runtime_error>("failed to parse test leaf key");

    EVPMdCtxPtr ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!ctx ||
        EVP_DigestSignInit(
            ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1)
        Throw<std::runtime_error>("EVP_DigestSignInit failed");

    std::size_t sigLen = 0;
    if (EVP_DigestSign(
            ctx.get(),
            nullptr,
            &sigLen,
            reinterpret_cast<unsigned char const*>(message.data()),
            message.size()) != 1)
        Throw<std::runtime_error>("EVP_DigestSign size failed");

    Blob sig(sigLen);
    if (EVP_DigestSign(
            ctx.get(),
            sig.data(),
            &sigLen,
            reinterpret_cast<unsigned char const*>(message.data()),
            message.size()) != 1)
        Throw<std::runtime_error>("EVP_DigestSign failed");
    sig.resize(sigLen);
    return sig;
}

DomainTrustAnchor
testTrustAnchor()
{
    static uint256 const id = sha512Half(asSlice(testRootDer));
    return {asSlice(testRootDer), id};
}

}  // namespace

class ValidatorIdentity_test : public beast::unit_test::suite
{
    static std::string
    makeManifest(
        PublicKey const& master,
        SecretKey const& masterSecret,
        std::optional<PublicKey> const& signing,
        std::optional<SecretKey> const& signingSecret,
        std::uint32_t sequence)
    {
        STObject st{sfGeneric};
        st[sfSequence] = sequence;
        st[sfPublicKey] = master;
        if (signing)
        {
            st[sfSigningPubKey] = *signing;
            sign(
                st,
                HashPrefix::manifest,
                *publicKeyType(*signing),
                *signingSecret);
        }
        sign(
            st,
            HashPrefix::manifest,
            *publicKeyType(master),
            masterSecret,
            sfMasterSignature);

        Serializer s;
        st.add(s);
        return {static_cast<char const*>(s.data()), s.size()};
    }

    static Json::Value
    manifestSet(jtx::Account const& carrier, std::string const& manifest)
    {
        Json::Value tx;
        tx[jss::TransactionType] = "ValidatorManifestSet";
        tx[jss::Account] = carrier.human();
        tx[sfManifest.jsonName] = strHex(makeSlice(manifest));
        return tx;
    }

    Json::Value
    domainSetWithNetwork(
        jtx::Account const& carrier,
        PublicKey const& master,
        SecretKey const& masterSecret,
        std::string const& domain,
        Slice leafDer,
        Slice leafKeyDer,
        uint256 const& expectedDomainHash,
        uint256 const& expectedValidatorDomainHash,
        std::uint64_t notBefore,
        std::uint64_t notAfter,
        std::uint32_t networkID,
        uint256 const& rootID,
        Blob const& chain,
        bool corruptDomainSig,
        bool corruptMasterSig)
    {
        STTx unsignedTx{
            ttVALIDATOR_DOMAIN_SET, [&](STObject& obj) {
                obj[sfAccount] = carrier.id();
                obj[sfFee] = STAmount{1};
                obj[sfSequence] = 1;
                obj.setFieldVL(sfDomain, makeSlice(domain));
                obj.setFieldVL(sfValidatorPublicKey, master.slice());
                obj.setFieldVL(sfCertificateChain, makeSlice(chain));
                // Placeholders; replaced after signing below is done via JSON.
                obj.setFieldVL(sfDomainSignature, Blob{0x00});
                obj.setFieldVL(sfValidatorMasterSignature, Blob{0x00});
                obj.setFieldH256(sfRootSetID, rootID);
                obj.setFieldU64(sfNotBefore, notBefore);
                obj.setFieldU64(sfNotAfter, notAfter);
                obj.setFieldH256(sfExpectedDomainHash, expectedDomainHash);
                obj.setFieldH256(
                    sfExpectedValidatorDomainHash, expectedValidatorDomainHash);
            }};

        auto const statement = domainBindingStatement(unsignedTx, networkID);
        auto domainSig = signWithLeafKey(leafKeyDer, statement.slice());
        auto const masterSigBuf = sign(master, masterSecret, statement.slice());
        Blob masterSig{
            masterSigBuf.data(), masterSigBuf.data() + masterSigBuf.size()};
        if (corruptDomainSig && !domainSig.empty())
            domainSig[0] ^= 0x01;
        if (corruptMasterSig && !masterSig.empty())
            masterSig[0] ^= 0x01;

        Json::Value tx;
        tx[jss::TransactionType] = "ValidatorDomainSet";
        tx[jss::Account] = carrier.human();
        tx[sfDomain.jsonName] = strHex(makeSlice(domain));
        tx[sfValidatorPublicKey.jsonName] = strHex(master.slice());
        tx[sfCertificateChain.jsonName] = strHex(makeSlice(chain));
        tx[sfDomainSignature.jsonName] = strHex(makeSlice(domainSig));
        tx[sfValidatorMasterSignature.jsonName] = strHex(makeSlice(masterSig));
        tx[sfRootSetID.jsonName] = to_string(rootID);
        tx[sfNotBefore.jsonName] = hexU64(notBefore);
        tx[sfNotAfter.jsonName] = hexU64(notAfter);
        tx[sfExpectedDomainHash.jsonName] = to_string(expectedDomainHash);
        tx[sfExpectedValidatorDomainHash.jsonName] =
            to_string(expectedValidatorDomainHash);
        return tx;
    }

    Json::Value
    domainSetFor(
        jtx::Env& env,
        jtx::Account const& carrier,
        PublicKey const& master,
        SecretKey const& masterSecret,
        std::string const& domain,
        Slice leafDer,
        Slice leafKeyDer,
        uint256 const& expectedDomainHash,
        uint256 const& expectedValidatorDomainHash,
        std::uint64_t notBefore = kNotBefore,
        std::uint64_t notAfter = kNotAfter,
        std::optional<uint256> rootSetID = std::nullopt,
        std::optional<Blob> chainOverride = std::nullopt,
        bool corruptDomainSig = false,
        bool corruptMasterSig = false)
    {
        auto const chain =
            chainOverride ? *chainOverride : lengthPrefixedChain(leafDer);
        auto const rootID =
            rootSetID.value_or(env.app().domainTrustAnchor().rootSetID);
        return domainSetWithNetwork(
            carrier,
            master,
            masterSecret,
            domain,
            leafDer,
            leafKeyDer,
            expectedDomainHash,
            expectedValidatorDomainHash,
            notBefore,
            notAfter,
            env.app().config().NETWORK_ID,
            rootID,
            chain,
            corruptDomainSig,
            corruptMasterSig);
    }

    jtx::Env
    makeDomainEnv(Slice rootDer = asSlice(testRootDer))
    {
        using namespace jtx;
        return Env{
            *this,
            envconfig(),
            supported_amendments() | featureOnlineValidatorIdentity,
            nullptr,
            beast::severities::kError,
            rootDer};
    }

    void
    advanceToCertValidity(jtx::Env& env)
    {
        env.close(kLedgerTime);
    }

    static std::pair<PublicKey, SecretKey>
    makeMaster()
    {
        auto const secret = randomSecretKey();
        auto const master = derivePublicKey(KeyType::ed25519, secret);
        return {master, secret};
    }

    void
    publishManifest(
        jtx::Env& env,
        jtx::Account const& carrier,
        PublicKey const& master,
        SecretKey const& masterSecret,
        std::uint32_t sequence = 1)
    {
        using namespace jtx;
        auto const signingSecret = randomSecretKey();
        auto const signing = derivePublicKey(KeyType::secp256k1, signingSecret);
        env(manifestSet(
                carrier,
                makeManifest(
                    master, masterSecret, signing, signingSecret, sequence)),
            fee(XRP(101)),
            ter(tesSUCCESS));
    }

    void
    testManifestLifecycle()
    {
        testcase("manifest lifecycle");
        using namespace jtx;

        Env env{*this, supported_amendments() | featureOnlineValidatorIdentity};
        Account const carrier{"carrier"};
        env.fund(XRP(1000), carrier);
        env.close();

        auto const masterSecret = randomSecretKey();
        auto const master = derivePublicKey(KeyType::ed25519, masterSecret);
        auto const signingSecret1 = randomSecretKey();
        auto const signing1 =
            derivePublicKey(KeyType::secp256k1, signingSecret1);
        auto const manifest1 =
            makeManifest(master, masterSecret, signing1, signingSecret1, 1);

        env(manifestSet(carrier, manifest1), fee(XRP(101)), ter(tesSUCCESS));
        auto validator = env.current()->read(keylet::validator(master));
        auto reverse = env.current()->read(keylet::validatorManifest(signing1));
        BEAST_EXPECT(validator);
        BEAST_EXPECT(reverse);
        if (!validator || !reverse)
            return;
        BEAST_EXPECT(validator->getFieldU32(sfSequence) == 1);
        BEAST_EXPECT(
            PublicKey{makeSlice(reverse->getFieldVL(sfValidatorPublicKey))} ==
            master);
        BEAST_EXPECT(
            env.app().validatorManifests().getMasterKey(signing1) == master);

        // Stale sequence: fee is consumed but live cache must stay on seq 1.
        env(manifestSet(carrier, manifest1), fee(XRP(1)), ter(tecDUPLICATE));
        BEAST_EXPECT(
            env.app().validatorManifests().getMasterKey(signing1) == master);
        BEAST_EXPECT(
            env.app().validatorManifests().getSigningKey(master) == signing1);

        auto const signingSecret2 = randomSecretKey();
        auto const signing2 =
            derivePublicKey(KeyType::secp256k1, signingSecret2);
        auto const manifest2 =
            makeManifest(master, masterSecret, signing2, signingSecret2, 2);
        env(manifestSet(carrier, manifest2), fee(XRP(1)), ter(tesSUCCESS));
        BEAST_EXPECT(
            !env.current()->exists(keylet::validatorManifest(signing1)));
        BEAST_EXPECT(
            env.current()->exists(keylet::validatorManifest(signing2)));
        BEAST_EXPECT(
            env.app().validatorManifests().getMasterKey(signing1) == signing1);
        BEAST_EXPECT(
            env.app().validatorManifests().getMasterKey(signing2) == master);

        auto const revocation = makeManifest(
            master,
            masterSecret,
            std::nullopt,
            std::nullopt,
            std::numeric_limits<std::uint32_t>::max());
        env(manifestSet(carrier, revocation), fee(XRP(1)), ter(tesSUCCESS));
        validator = env.current()->read(keylet::validator(master));
        BEAST_EXPECT(validator);
        BEAST_EXPECT(
            validator->getFieldU32(sfSequence) ==
            std::numeric_limits<std::uint32_t>::max());
        BEAST_EXPECT(!validator->isFieldPresent(sfSigningPubKey));
        BEAST_EXPECT(
            !env.current()->exists(keylet::validatorManifest(signing2)));
        BEAST_EXPECT(
            env.app().validatorManifests().getSigningKey(master) == master);
    }

    void
    testManifestCollision()
    {
        testcase("manifest current-role collision");
        using namespace jtx;

        Env env{*this, supported_amendments() | featureOnlineValidatorIdentity};
        Account const carrier{"carrier"};
        env.fund(XRP(1000), carrier);
        env.close();

        auto const signingSecret = randomSecretKey();
        auto const signing = derivePublicKey(KeyType::secp256k1, signingSecret);
        auto const masterSecret1 = randomSecretKey();
        auto const master1 = derivePublicKey(KeyType::ed25519, masterSecret1);
        env(manifestSet(
                carrier,
                makeManifest(
                    master1, masterSecret1, signing, signingSecret, 1)),
            fee(XRP(101)),
            ter(tesSUCCESS));
        BEAST_EXPECT(
            env.app().validatorManifests().getMasterKey(signing) == master1);

        auto const masterSecret2 = randomSecretKey();
        auto const master2 = derivePublicKey(KeyType::ed25519, masterSecret2);
        env(manifestSet(
                carrier,
                makeManifest(
                    master2, masterSecret2, signing, signingSecret, 1)),
            fee(XRP(101)),
            ter(tecDUPLICATE));
        BEAST_EXPECT(!env.current()->exists(keylet::validator(master2)));
        // Rejected collision must not re-attribute the live signing key.
        BEAST_EXPECT(
            env.app().validatorManifests().getMasterKey(signing) == master1);
        BEAST_EXPECT(
            env.app().validatorManifests().getSigningKey(master2) == master2);
    }

    void
    testDisabled()
    {
        testcase("amendment disabled");
        using namespace jtx;

        Env env{*this, supported_amendments() - featureOnlineValidatorIdentity};
        Account const carrier{"carrier"};
        env.fund(XRP(1000), carrier);
        env.close();

        auto const masterSecret = randomSecretKey();
        auto const signingSecret = randomSecretKey();
        env(manifestSet(
                carrier,
                makeManifest(
                    derivePublicKey(KeyType::ed25519, masterSecret),
                    masterSecret,
                    derivePublicKey(KeyType::secp256k1, signingSecret),
                    signingSecret,
                    1)),
            fee(XRP(10)),
            ter(temDISABLED));
    }

    void
    testProductionAnchorDefault()
    {
        testcase("production application uses ISRG trust anchor");
        using namespace jtx;

        // Default Env (no injected root) must equal
        // productionDomainTrustAnchor.
        Env env{*this, supported_amendments() | featureOnlineValidatorIdentity};
        auto const appAnchor = env.app().domainTrustAnchor();
        auto const prod = productionDomainTrustAnchor();
        BEAST_EXPECT(appAnchor.rootSetID == prod.rootSetID);
        BEAST_EXPECT(appAnchor.der.size() == prod.der.size());
        BEAST_EXPECT(std::equal(
            appAnchor.der.begin(), appAnchor.der.end(), prod.der.begin()));
    }

    void
    testDomainInitialBind()
    {
        testcase("domain initial bind (e2e Env)");
        using namespace jtx;

        auto env = makeDomainEnv();
        Account const carrier{"carrier"};
        env.fund(XRP(10000), carrier);
        advanceToCertValidity(env);

        auto const [master, masterSecret] = makeMaster();
        publishManifest(env, carrier, master, masterSecret);

        auto const domain = std::string{"a.example"};
        auto const domainKey = keylet::validatorDomain(makeSlice(domain));

        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                beast::zero,
                beast::zero),
            fee(XRP(101)),
            ter(tesSUCCESS));

        auto const domainObj = env.current()->read(domainKey);
        auto const validator = env.current()->read(keylet::validator(master));
        BEAST_EXPECT(domainObj);
        BEAST_EXPECT(validator);
        if (!domainObj || !validator)
            return;

        BEAST_EXPECT(
            makeSlice(domainObj->getFieldVL(sfDomain)) == makeSlice(domain));
        BEAST_EXPECT(
            PublicKey{makeSlice(domainObj->getFieldVL(sfValidatorPublicKey))} ==
            master);
        BEAST_EXPECT(domainObj->isFieldPresent(sfCertificateChain));
        BEAST_EXPECT(domainObj->isFieldPresent(sfDomainSignature));
        BEAST_EXPECT(domainObj->isFieldPresent(sfValidatorMasterSignature));
        BEAST_EXPECT(
            domainObj->getFieldH256(sfRootSetID) ==
            env.app().domainTrustAnchor().rootSetID);
        BEAST_EXPECT(domainObj->getFieldU64(sfNotBefore) == kNotBefore);
        BEAST_EXPECT(domainObj->getFieldU64(sfNotAfter) == kNotAfter);
        BEAST_EXPECT(domainObj->isFieldPresent(sfDigest));
        BEAST_EXPECT(validator->isFieldPresent(sfDomainID));
        BEAST_EXPECT(validator->getFieldH256(sfDomainID) == domainKey.key);
        BEAST_EXPECT(isDomainBindingActive(
            *env.current(), *domainObj, env.current()->info().parentCloseTime));
        // Carrier is absent from identity state.
        BEAST_EXPECT(!domainObj->isFieldPresent(sfAccount));
    }

    void
    testDomainStaleCAS()
    {
        testcase("domain stale CAS rejection (e2e Env)");
        using namespace jtx;

        auto env = makeDomainEnv();
        Account const carrier{"carrier"};
        env.fund(XRP(10000), carrier);
        advanceToCertValidity(env);

        auto const [master, masterSecret] = makeMaster();
        publishManifest(env, carrier, master, masterSecret);

        auto const domain = std::string{"a.example"};
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                beast::zero,
                beast::zero),
            fee(XRP(101)),
            ter(tesSUCCESS));

        auto const domainObj =
            env.current()->read(keylet::validatorDomain(makeSlice(domain)));
        auto const validator = env.current()->read(keylet::validator(master));
        BEAST_EXPECT(domainObj && validator);
        if (!domainObj || !validator)
            return;

        auto const digest = domainObj->getFieldH256(sfDigest);
        auto const domainID = validator->getFieldH256(sfDomainID);

        // Stale expected domain hash (zero after bind).
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                beast::zero,
                domainID),
            fee(XRP(101)),
            ter(tecDUPLICATE));

        // Stale expected validator domain pointer.
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                digest,
                beast::zero),
            fee(XRP(101)),
            ter(tecDUPLICATE));

        auto const after =
            env.current()->read(keylet::validatorDomain(makeSlice(domain)));
        auto const afterV = env.current()->read(keylet::validator(master));
        BEAST_EXPECT(after && afterV);
        if (after && afterV)
        {
            BEAST_EXPECT(after->getFieldH256(sfDigest) == digest);
            BEAST_EXPECT(afterV->getFieldH256(sfDomainID) == domainID);
        }
    }

    void
    testDomainReplace()
    {
        testcase("domain replace a.example -> b.example (e2e Env)");
        using namespace jtx;

        auto env = makeDomainEnv();
        Account const carrier{"carrier"};
        env.fund(XRP(10000), carrier);
        advanceToCertValidity(env);

        auto const [master, masterSecret] = makeMaster();
        publishManifest(env, carrier, master, masterSecret);

        auto const domainA = std::string{"a.example"};
        auto const domainB = std::string{"b.example"};
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domainA,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                beast::zero,
                beast::zero),
            fee(XRP(101)),
            ter(tesSUCCESS));

        auto const validator = env.current()->read(keylet::validator(master));
        BEAST_EXPECT(validator && validator->isFieldPresent(sfDomainID));
        if (!validator)
            return;
        auto const oldDomainID = validator->getFieldH256(sfDomainID);

        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domainB,
                asSlice(leafBDer),
                asSlice(leafBKeyDer),
                beast::zero,
                oldDomainID),
            fee(XRP(101)),
            ter(tesSUCCESS));

        BEAST_EXPECT(!env.current()->exists(
            keylet::validatorDomain(makeSlice(domainA))));
        auto const bObj =
            env.current()->read(keylet::validatorDomain(makeSlice(domainB)));
        auto const v2 = env.current()->read(keylet::validator(master));
        BEAST_EXPECT(bObj && v2);
        if (bObj && v2)
        {
            BEAST_EXPECT(
                PublicKey{makeSlice(bObj->getFieldVL(sfValidatorPublicKey))} ==
                master);
            BEAST_EXPECT(
                v2->getFieldH256(sfDomainID) ==
                keylet::validatorDomain(makeSlice(domainB)).key);
        }
    }

    void
    testDomainTransfer()
    {
        testcase("domain transfer A -> B (e2e Env)");
        using namespace jtx;

        auto env = makeDomainEnv();
        Account const carrier{"carrier"};
        env.fund(XRP(10000), carrier);
        advanceToCertValidity(env);

        auto const [masterA, secretA] = makeMaster();
        auto const [masterB, secretB] = makeMaster();
        publishManifest(env, carrier, masterA, secretA);
        publishManifest(env, carrier, masterB, secretB);

        auto const domain = std::string{"a.example"};
        env(domainSetFor(
                env,
                carrier,
                masterA,
                secretA,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                beast::zero,
                beast::zero),
            fee(XRP(101)),
            ter(tesSUCCESS));

        auto const domainObj =
            env.current()->read(keylet::validatorDomain(makeSlice(domain)));
        BEAST_EXPECT(domainObj);
        if (!domainObj)
            return;
        auto const digest = domainObj->getFieldH256(sfDigest);

        // Transfer a.example from A to B.
        env(domainSetFor(
                env,
                carrier,
                masterB,
                secretB,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                digest,
                beast::zero),
            fee(XRP(101)),
            ter(tesSUCCESS));

        auto const after =
            env.current()->read(keylet::validatorDomain(makeSlice(domain)));
        auto const va = env.current()->read(keylet::validator(masterA));
        auto const vb = env.current()->read(keylet::validator(masterB));
        BEAST_EXPECT(after && va && vb);
        if (!after || !va || !vb)
            return;

        BEAST_EXPECT(
            PublicKey{makeSlice(after->getFieldVL(sfValidatorPublicKey))} ==
            masterB);
        BEAST_EXPECT(!va->isFieldPresent(sfDomainID));
        BEAST_EXPECT(
            vb->getFieldH256(sfDomainID) ==
            keylet::validatorDomain(makeSlice(domain)).key);

        // Stale transfer cannot clear an unrelated/newer backlink: re-bind A
        // to a different domain, then replay transfer CAS against old state.
        env(domainSetFor(
                env,
                carrier,
                masterA,
                secretA,
                std::string{"b.example"},
                asSlice(leafBDer),
                asSlice(leafBKeyDer),
                beast::zero,
                beast::zero),
            fee(XRP(101)),
            ter(tesSUCCESS));
        BEAST_EXPECT(env.current()
                         ->read(keylet::validator(masterA))
                         ->isFieldPresent(sfDomainID));

        // Replay the transfer inputs that would have cleared A — but A no
        // longer points at a.example, so even a stale wrapper that still
        // named A cannot clear A's newer b.example backlink. Use stale
        // domain CAS (old digest) so the tx fails before mutation.
        env(domainSetFor(
                env,
                carrier,
                masterB,
                secretB,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                digest,
                keylet::validatorDomain(makeSlice(domain)).key),
            fee(XRP(101)),
            ter(tecDUPLICATE));

        auto const va2 = env.current()->read(keylet::validator(masterA));
        BEAST_EXPECT(va2 && va2->isFieldPresent(sfDomainID));
        if (va2)
        {
            BEAST_EXPECT(
                va2->getFieldH256(sfDomainID) ==
                keylet::validatorDomain(makeSlice(std::string{"b.example"}))
                    .key);
        }
    }

    void
    testDomainExpiry()
    {
        testcase("domain expiry semantics (e2e Env)");
        using namespace jtx;

        auto env = makeDomainEnv();
        Account const carrier{"carrier"};
        env.fund(XRP(10000), carrier);
        advanceToCertValidity(env);

        auto const [master, masterSecret] = makeMaster();
        publishManifest(env, carrier, master, masterSecret);

        auto const domain = std::string{"a.example"};
        // Narrow claimed window that still contains the current parent time.
        auto const parent =
            env.current()->info().parentCloseTime.time_since_epoch().count();
        auto const shortAfter = static_cast<std::uint64_t>(parent) + 20;

        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                beast::zero,
                beast::zero,
                kNotBefore,
                shortAfter),
            fee(XRP(101)),
            ter(tesSUCCESS));

        auto domainObj =
            env.current()->read(keylet::validatorDomain(makeSlice(domain)));
        BEAST_EXPECT(domainObj);
        if (!domainObj)
            return;
        auto const digest = domainObj->getFieldH256(sfDigest);
        auto const domainID = env.current()
                                  ->read(keylet::validator(master))
                                  ->getFieldH256(sfDomainID);

        // Advance past NotAfter: object remains; inactive for lookup.
        env.close(NetClock::time_point{NetClock::duration{shortAfter + 5}});
        domainObj =
            env.current()->read(keylet::validatorDomain(makeSlice(domain)));
        BEAST_EXPECT(domainObj);
        if (!domainObj)
            return;
        BEAST_EXPECT(!isDomainBindingActive(
            *env.current(), *domainObj, env.current()->info().parentCloseTime));
        BEAST_EXPECT(domainObj->getFieldH256(sfDigest) == digest);

        // Expired claimed proof cannot establish a new binding (zero CAS
        // is wrong anyway once the object exists; use correct CAS with a
        // claimed window that does not contain parent time).
        auto const now =
            env.current()->info().parentCloseTime.time_since_epoch().count();
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                digest,
                domainID,
                kNotBefore,
                static_cast<std::uint64_t>(
                    now)),  // notAfter == now => inactive
            fee(XRP(101)),
            ter(tecNO_PERMISSION));

        // Valid replacement after expiry must still name the predecessor.
        auto const newAfter = static_cast<std::uint64_t>(now) + 100000;
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                digest,
                domainID,
                static_cast<std::uint64_t>(now) - 10,
                newAfter),
            fee(XRP(101)),
            ter(tesSUCCESS));

        domainObj =
            env.current()->read(keylet::validatorDomain(makeSlice(domain)));
        BEAST_EXPECT(domainObj);
        if (domainObj)
        {
            BEAST_EXPECT(domainObj->getFieldH256(sfDigest) != digest);
            BEAST_EXPECT(isDomainBindingActive(
                *env.current(),
                *domainObj,
                env.current()->info().parentCloseTime));
        }
    }

    void
    testDomainNegativeVerifier()
    {
        testcase("domain negative verifier cases (e2e Env)");
        using namespace jtx;

        auto env = makeDomainEnv();
        Account const carrier{"carrier"};
        env.fund(XRP(10000), carrier);
        advanceToCertValidity(env);

        auto const [master, masterSecret] = makeMaster();
        publishManifest(env, carrier, master, masterSecret);

        auto const domain = std::string{"a.example"};
        auto const expectUnchanged = [&] {
            BEAST_EXPECT(!env.current()->exists(
                keylet::validatorDomain(makeSlice(domain))));
            auto const v = env.current()->read(keylet::validator(master));
            BEAST_EXPECT(v && !v->isFieldPresent(sfDomainID));
        };

        // Wrong RootSetID.
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                beast::zero,
                beast::zero,
                kNotBefore,
                kNotAfter,
                uint256{beast::zero}),
            fee(XRP(101)),
            ter(tecNO_PERMISSION));
        expectUnchanged();

        // Wrong DNS SAN / domain vs leaf.
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafWrongDer),
                asSlice(leafWrongKeyDer),
                beast::zero,
                beast::zero),
            fee(XRP(101)),
            ter(tecNO_PERMISSION));
        expectUnchanged();

        // Invalid domain-key signature.
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                beast::zero,
                beast::zero,
                kNotBefore,
                kNotAfter,
                std::nullopt,
                std::nullopt,
                true,
                false),
            fee(XRP(101)),
            ter(tecNO_PERMISSION));
        expectUnchanged();

        // Invalid master signature.
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                beast::zero,
                beast::zero,
                kNotBefore,
                kNotAfter,
                std::nullopt,
                std::nullopt,
                false,
                true),
            fee(XRP(101)),
            ter(tecNO_PERMISSION));
        expectUnchanged();

        // Proof outside claimed validity (notAfter <= now).
        auto const now =
            env.current()->info().parentCloseTime.time_since_epoch().count();
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                beast::zero,
                beast::zero,
                kNotBefore,
                static_cast<std::uint64_t>(now)),
            fee(XRP(101)),
            ter(tecNO_PERMISSION));
        expectUnchanged();

        // Malformed / truncated length-prefixed chain.
        Blob trunc{0x00, 0x10, 0xff};
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                beast::zero,
                beast::zero,
                kNotBefore,
                kNotAfter,
                std::nullopt,
                trunc),
            fee(XRP(101)),
            ter(tecNO_PERMISSION));
        expectUnchanged();

        // Zero-length cert prefix is malformed.
        Blob emptyPrefixed{0x00, 0x00};
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                beast::zero,
                beast::zero,
                kNotBefore,
                kNotAfter,
                std::nullopt,
                emptyPrefixed),
            fee(XRP(101)),
            ter(tecNO_PERMISSION));
        expectUnchanged();

        // Extra root in the presented chain is rejected (root is the trust
        // anchor, not a transaction certificate).
        {
            auto chain = lengthPrefixedChain(asSlice(leafADer));
            auto const root = asSlice(testRootDer);
            chain.push_back(
                static_cast<std::uint8_t>((root.size() >> 8) & 0xff));
            chain.push_back(static_cast<std::uint8_t>(root.size() & 0xff));
            chain.insert(chain.end(), root.begin(), root.end());
            env(domainSetFor(
                    env,
                    carrier,
                    master,
                    masterSecret,
                    domain,
                    asSlice(leafADer),
                    asSlice(leafAKeyDer),
                    beast::zero,
                    beast::zero,
                    kNotBefore,
                    kNotAfter,
                    std::nullopt,
                    chain),
                fee(XRP(101)),
                ter(tecNO_PERMISSION));
            expectUnchanged();
        }
    }

    void
    testCarrierSignatureCoversProof()
    {
        testcase("carrier signature covers domain proof signatures");
        using namespace jtx;

        auto env = makeDomainEnv();
        Account const carrier{"carrier"};
        env.fund(XRP(10000), carrier);
        advanceToCertValidity(env);

        auto const [master, masterSecret] = makeMaster();
        publishManifest(env, carrier, master, masterSecret);

        auto const jv = domainSetFor(
            env,
            carrier,
            master,
            masterSecret,
            std::string{"a.example"},
            asSlice(leafADer),
            asSlice(leafAKeyDer),
            beast::zero,
            beast::zero);

        STTx tx{
            ttVALIDATOR_DOMAIN_SET, [&](STObject& obj) {
                obj[sfAccount] = carrier.id();
                obj[sfFee] = STAmount{XRPAmount{1}};
                obj[sfSequence] = env.seq(carrier);
                obj.setFieldVL(sfSigningPubKey, carrier.pk().slice());
                obj.setFieldVL(
                    sfDomain,
                    strUnHex(jv[sfDomain.jsonName].asString()).value());
                obj.setFieldVL(
                    sfValidatorPublicKey,
                    strUnHex(jv[sfValidatorPublicKey.jsonName].asString())
                        .value());
                obj.setFieldVL(
                    sfCertificateChain,
                    strUnHex(jv[sfCertificateChain.jsonName].asString())
                        .value());
                obj.setFieldVL(
                    sfDomainSignature,
                    strUnHex(jv[sfDomainSignature.jsonName].asString())
                        .value());
                obj.setFieldVL(
                    sfValidatorMasterSignature,
                    strUnHex(jv[sfValidatorMasterSignature.jsonName].asString())
                        .value());
                uint256 rootSet;
                BEAST_EXPECT(
                    rootSet.parseHex(jv[sfRootSetID.jsonName].asString()));
                obj.setFieldH256(sfRootSetID, rootSet);
                obj.setFieldU64(sfNotBefore, kNotBefore);
                obj.setFieldU64(sfNotAfter, kNotAfter);
                obj.setFieldH256(sfExpectedDomainHash, beast::zero);
                obj.setFieldH256(sfExpectedValidatorDomainHash, beast::zero);
            }};
        tx.sign(carrier.pk(), carrier.sk());
        BEAST_EXPECT(tx.checkSign(
            STTx::RequireFullyCanonicalSig::yes, env.current()->rules()));

        auto mutated = tx.getFieldVL(sfDomainSignature);
        BEAST_EXPECT(!mutated.empty());
        if (!mutated.empty())
        {
            mutated[0] ^= 0x01;
            tx.setFieldVL(sfDomainSignature, mutated);
            BEAST_EXPECT(!tx.checkSign(
                STTx::RequireFullyCanonicalSig::yes, env.current()->rules()));
        }
    }

    void
    testDomainProofRejectedWithoutTestRoot()
    {
        testcase("domain proof rejected under production root");
        using namespace jtx;

        // Default Env pins ISRG; test-CA material must fail preclaim.
        Env env{*this, supported_amendments() | featureOnlineValidatorIdentity};
        Account const carrier{"carrier"};
        env.fund(XRP(1000), carrier);
        env.close(kLedgerTime);

        auto const [master, masterSecret] = makeMaster();
        publishManifest(env, carrier, master, masterSecret);

        // Build against the test root set id, but the app uses ISRG.
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                std::string{"a.example"},
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                beast::zero,
                beast::zero,
                kNotBefore,
                kNotAfter,
                testTrustAnchor().rootSetID),
            fee(XRP(101)),
            ter(tecNO_PERMISSION));
        BEAST_EXPECT(!env.current()->exists(
            keylet::validatorDomain(makeSlice(std::string{"a.example"}))));
    }

    void
    testDomainRejectedWhenRevoked()
    {
        testcase("domain bind rejected for terminally revoked master");
        using namespace jtx;

        auto env = makeDomainEnv();
        Account const carrier{"carrier"};
        env.fund(XRP(10000), carrier);
        advanceToCertValidity(env);

        auto const [master, masterSecret] = makeMaster();
        publishManifest(env, carrier, master, masterSecret);

        auto const domain = std::string{"a.example"};
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                beast::zero,
                beast::zero),
            fee(XRP(101)),
            ter(tesSUCCESS));

        auto domainObj =
            env.current()->read(keylet::validatorDomain(makeSlice(domain)));
        auto validator = env.current()->read(keylet::validator(master));
        BEAST_EXPECT(domainObj && validator);
        if (!domainObj || !validator)
            return;
        auto const digest = domainObj->getFieldH256(sfDigest);
        auto const domainID = validator->getFieldH256(sfDomainID);
        BEAST_EXPECT(isDomainBindingActive(
            *env.current(), *domainObj, env.current()->info().parentCloseTime));

        // Terminal revocation clears the ephemeral reverse index but keeps
        // the master registry object (and any domain predecessor).
        auto const revocation = makeManifest(
            master,
            masterSecret,
            std::nullopt,
            std::nullopt,
            std::numeric_limits<std::uint32_t>::max());
        env(manifestSet(carrier, revocation), fee(XRP(1)), ter(tesSUCCESS));
        validator = env.current()->read(keylet::validator(master));
        domainObj =
            env.current()->read(keylet::validatorDomain(makeSlice(domain)));
        BEAST_EXPECT(validator && domainObj);
        if (!validator || !domainObj)
            return;
        BEAST_EXPECT(!isDomainBindingActive(
            *env.current(), *domainObj, env.current()->info().parentCloseTime));

        // Revoked masters cannot create or replace domain bindings.
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafADer),
                asSlice(leafAKeyDer),
                digest,
                domainID),
            fee(XRP(101)),
            ter(tecNO_PERMISSION));
        BEAST_EXPECT(
            env.current()
                ->read(keylet::validatorDomain(makeSlice(domain)))
                ->getFieldH256(sfDigest) == digest);
    }

    void
    testDomainIntermediateConstraints()
    {
        testcase("domain intermediate CA constraints");
        using namespace jtx;

        auto env = makeDomainEnv(asSlice(icaRootDer));
        Account const carrier{"carrier"};
        env.fund(XRP(10000), carrier);
        advanceToCertValidity(env);

        auto const [master, masterSecret] = makeMaster();
        publishManifest(env, carrier, master, masterSecret);

        auto const domain = std::string{"a.example"};
        auto const domainKey = keylet::validatorDomain(makeSlice(domain));

        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafViaIcaDer),
                asSlice(leafViaIcaKeyDer),
                beast::zero,
                beast::zero,
                kNotBefore,
                kNotAfter,
                std::nullopt,
                lengthPrefixedChain(
                    {asSlice(leafViaIcaDer), asSlice(icaGoodDer)})),
            fee(XRP(101)),
            ter(tesSUCCESS));
        BEAST_EXPECT(env.current()->exists(domainKey));

        // SAN DNS:A.EXAMPLE matches the normalized claimed domain.
        env(domainSetFor(
                env,
                carrier,
                master,
                masterSecret,
                domain,
                asSlice(leafUpperSanDer),
                asSlice(leafUpperSanKeyDer),
                env.current()->read(domainKey)->getFieldH256(sfDigest),
                domainKey.key,
                kNotBefore,
                kNotAfter,
                std::nullopt,
                lengthPrefixedChain(
                    {asSlice(leafUpperSanDer), asSlice(icaGoodDer)})),
            fee(XRP(101)),
            ter(tesSUCCESS));

        auto const expectNo = [&](Slice leaf, Slice key, Blob const& chain) {
            env(domainSetFor(
                    env,
                    carrier,
                    master,
                    masterSecret,
                    domain,
                    leaf,
                    key,
                    env.current()->read(domainKey)->getFieldH256(sfDigest),
                    domainKey.key,
                    kNotBefore,
                    kNotAfter,
                    std::nullopt,
                    chain),
                fee(XRP(101)),
                ter(tecNO_PERMISSION));
            BEAST_EXPECT(
                PublicKey{makeSlice(env.current()->read(domainKey)->getFieldVL(
                    sfValidatorPublicKey))} == master);
        };

        expectNo(
            asSlice(leafNcDer),
            asSlice(leafNcKeyDer),
            lengthPrefixedChain({asSlice(leafNcDer), asSlice(icaNcDenyDer)}));
        expectNo(
            asSlice(leafEkuDer),
            asSlice(leafEkuKeyDer),
            lengthPrefixedChain({asSlice(leafEkuDer), asSlice(icaBadEkuDer)}));
        expectNo(
            asSlice(leafCritDer),
            asSlice(leafCritKeyDer),
            lengthPrefixedChain({asSlice(leafCritDer), asSlice(icaCritDer)}));
        expectNo(
            asSlice(leafDeepDer),
            asSlice(leafDeepKeyDer),
            lengthPrefixedChain(
                {asSlice(leafDeepDer),
                 asSlice(icaChildDer),
                 asSlice(icaPath0Der)}));
        // Extra SAN is excluded by permitted-only NC; claimed domain is
        // allowed, so this only fails once NAME_CONSTRAINTS_check sees SAN.
        expectNo(
            asSlice(leafExtraSanDer),
            asSlice(leafExtraSanKeyDer),
            lengthPrefixedChain(
                {asSlice(leafExtraSanDer), asSlice(icaNcPermitADer)}));
        // Excluded DNS is encoded A.EXAMPLE against claimed a.example.
        expectNo(
            asSlice(leafExcludeADer),
            asSlice(leafExcludeAKeyDer),
            lengthPrefixedChain(
                {asSlice(leafExcludeADer), asSlice(icaNcExcludeADer)}));
        // Leading-dot excluded .example matches a.example.
        expectNo(
            asSlice(leafDotDer),
            asSlice(leafDotKeyDer),
            lengthPrefixedChain({asSlice(leafDotDer), asSlice(icaNcDotDer)}));
        // keyCertSign without basicConstraints is not a CA.
        expectNo(
            asSlice(leafNoBcDer),
            asSlice(leafNoBcKeyDer),
            lengthPrefixedChain({asSlice(leafNoBcDer), asSlice(icaNoBcDer)}));
        // nameConstraints is a CA extension; reject it on the leaf.
        expectNo(
            asSlice(leafNcLeafDer),
            asSlice(leafNcLeafKeyDer),
            lengthPrefixedChain({asSlice(leafNcLeafDer), asSlice(icaGoodDer)}));
    }

public:
    void
    run() override
    {
        testManifestLifecycle();
        testManifestCollision();
        testDisabled();
        testProductionAnchorDefault();
        testDomainInitialBind();
        testDomainStaleCAS();
        testDomainReplace();
        testDomainTransfer();
        testDomainExpiry();
        testDomainNegativeVerifier();
        testCarrierSignatureCoversProof();
        testDomainProofRejectedWithoutTestRoot();
        testDomainRejectedWhenRevoked();
        testDomainIntermediateConstraints();
    }
};

BEAST_DEFINE_TESTSUITE(ValidatorIdentity, app, ripple);

}  // namespace test
}  // namespace ripple
