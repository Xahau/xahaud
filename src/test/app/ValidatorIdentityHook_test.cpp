//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.
*/
//==============================================================================

#include <test/app/ValidatorIdentityHook_test_hooks.h>
#include <test/app/ValidatorIdentity_test_certs.h>
#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <xrpld/app/tx/detail/ValidatorIdentity.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/digest.h>

#include <openssl/evp.h>

#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ripple {
namespace test {

using TestHook = std::vector<std::uint8_t> const&;

namespace {

using namespace validator_identity_fixtures;

constexpr NetClock::time_point kLedgerTime{NetClock::duration{631152000}};

using EVPKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using EVPMdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

template <std::size_t N>
Slice
asSlice(std::array<std::uint8_t, N> const& a)
{
    return {a.data(), a.size()};
}

Blob
lengthPrefixedChain(Slice leafDer)
{
    Blob out;
    out.reserve(2 + leafDer.size());
    out.push_back(static_cast<std::uint8_t>((leafDer.size() >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(leafDer.size() & 0xff));
    out.insert(out.end(), leafDer.begin(), leafDer.end());
    return out;
}

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

}  // namespace

class ValidatorIdentityHook_test : public beast::unit_test::suite
{
    static std::string
    makeManifest(
        PublicKey const& master,
        SecretKey const& masterSecret,
        std::uint32_t sequence,
        std::optional<PublicKey>* signingOut = nullptr)
    {
        auto const signingSecret = randomSecretKey();
        auto const signing = derivePublicKey(KeyType::secp256k1, signingSecret);
        if (signingOut)
            *signingOut = signing;

        STObject st{sfGeneric};
        st[sfSequence] = sequence;
        st[sfPublicKey] = master;
        st[sfSigningPubKey] = signing;
        sign(st, HashPrefix::manifest, *publicKeyType(signing), signingSecret);
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
    domainSetFor(
        jtx::Env& env,
        jtx::Account const& carrier,
        PublicKey const& master,
        SecretKey const& masterSecret,
        std::string const& domain,
        Slice leafDer,
        Slice leafKeyDer)
    {
        auto const chain = lengthPrefixedChain(leafDer);
        auto const rootID = env.app().domainTrustAnchor().rootSetID;
        STTx unsignedTx{
            ttVALIDATOR_DOMAIN_SET, [&](STObject& obj) {
                obj[sfAccount] = carrier.id();
                obj[sfFee] = STAmount{1};
                obj[sfSequence] = 1;
                obj.setFieldVL(sfDomain, makeSlice(domain));
                obj.setFieldVL(sfValidatorPublicKey, master.slice());
                obj.setFieldVL(sfCertificateChain, makeSlice(chain));
                obj.setFieldVL(sfDomainSignature, Blob{0x00});
                obj.setFieldVL(sfValidatorMasterSignature, Blob{0x00});
                obj.setFieldH256(sfRootSetID, rootID);
                obj.setFieldU64(sfNotBefore, certNotBeforeNet);
                obj.setFieldU64(sfNotAfter, certNotAfterNet);
                obj.setFieldH256(sfExpectedDomainHash, beast::zero);
                obj.setFieldH256(sfExpectedValidatorDomainHash, beast::zero);
            }};
        auto const statement =
            domainBindingStatement(unsignedTx, env.app().config().NETWORK_ID);
        auto const domainSig = signWithLeafKey(leafKeyDer, statement.slice());
        auto const masterSigBuf = sign(master, masterSecret, statement.slice());
        Blob const masterSig{
            masterSigBuf.data(), masterSigBuf.data() + masterSigBuf.size()};

        Json::Value tx;
        tx[jss::TransactionType] = "ValidatorDomainSet";
        tx[jss::Account] = carrier.human();
        tx[sfDomain.jsonName] = strHex(makeSlice(domain));
        tx[sfValidatorPublicKey.jsonName] = strHex(master.slice());
        tx[sfCertificateChain.jsonName] = strHex(makeSlice(chain));
        tx[sfDomainSignature.jsonName] = strHex(makeSlice(domainSig));
        tx[sfValidatorMasterSignature.jsonName] = strHex(makeSlice(masterSig));
        tx[sfRootSetID.jsonName] = to_string(rootID);
        tx[sfNotBefore.jsonName] = hexU64(certNotBeforeNet);
        tx[sfNotAfter.jsonName] = hexU64(certNotAfterNet);
        tx[sfExpectedDomainHash.jsonName] = to_string(uint256{beast::zero});
        tx[sfExpectedValidatorDomainHash.jsonName] =
            to_string(uint256{beast::zero});
        return tx;
    }

    void
    testOptionalAccountRootVeto()
    {
        testcase("optional validator AccountRoot veto");
        using namespace jtx;

        TestHook veto = validatoridentityhook_test_wasm.at(R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t id, uint32_t maxiter);
            extern int64_t rollback(
                uint32_t read_ptr, uint32_t read_len, int64_t error_code);

            int64_t hook(uint32_t reserved)
            {
                _g(1, 1);
                return rollback(0, 0, 1);
            }
        )[test.hook]");

        Env env{*this, supported_amendments() | featureOnlineValidatorIdentity};
        Account const carrier{"carrier"};
        Account const validator{"validator"};
        env.fund(XRP(1000), carrier, validator);
        env.close();

        // A validator master without an AccountRoot remains permissionless.
        auto const orphanSecret = randomSecretKey();
        auto const orphanMaster =
            derivePublicKey(KeyType::ed25519, orphanSecret);
        env(manifestSet(carrier, makeManifest(orphanMaster, orphanSecret, 1)),
            fee(XRP(101)),
            ter(tesSUCCESS));
        BEAST_EXPECT(env.current()->exists(keylet::validator(orphanMaster)));

        env(hook(validator, {{hso(veto)}}, 0), fee(XRP(2)), ter(tesSUCCESS));
        env.close();

        // If the master maps to an AccountRoot, that account is an additional
        // strong stakeholder and can veto a third-party carrier submission.
        std::optional<PublicKey> rejectedSigning;
        env(manifestSet(
                carrier,
                makeManifest(
                    validator.pk(), validator.sk(), 1, &rejectedSigning)),
            fee(XRP(101)),
            ter(tecHOOK_REJECTED));
        BEAST_EXPECT(!env.current()->exists(keylet::validator(validator.pk())));
        // Fee/sequence are consumed, but a rejected result must not activate
        // the live ManifestCache.
        BEAST_EXPECT(rejectedSigning);
        if (rejectedSigning)
        {
            BEAST_EXPECT(
                env.app().validatorManifests().getMasterKey(*rejectedSigning) ==
                *rejectedSigning);
            BEAST_EXPECT(
                env.app().validatorManifests().getSigningKey(validator.pk()) ==
                validator.pk());
        }
    }

    void
    testDomainAccountRootVeto()
    {
        testcase("domain AccountRoot hook veto (e2e Env)");
        using namespace jtx;

        TestHook veto = validatoridentityhook_test_wasm.at(R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t id, uint32_t maxiter);
            extern int64_t rollback(
                uint32_t read_ptr, uint32_t read_len, int64_t error_code);

            int64_t hook(uint32_t reserved)
            {
                _g(1, 1);
                return rollback(0, 0, 1);
            }
        )[test.hook]");

        // Same application-scoped test-root seam as the domain lifecycle suite.
        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureOnlineValidatorIdentity,
            nullptr,
            beast::severities::kError,
            asSlice(testRootDer)};
        Account const carrier{"carrier"};
        Account const validator{"validator"};
        env.fund(XRP(10000), carrier, validator);
        env.close(kLedgerTime);

        // Orphan master: no AccountRoot TSH, domain bind succeeds.
        auto const orphanSecret = randomSecretKey();
        auto const orphanMaster =
            derivePublicKey(KeyType::ed25519, orphanSecret);
        env(manifestSet(carrier, makeManifest(orphanMaster, orphanSecret, 1)),
            fee(XRP(101)),
            ter(tesSUCCESS));
        env(domainSetFor(
                env,
                carrier,
                orphanMaster,
                orphanSecret,
                "a.example",
                asSlice(leafADer),
                asSlice(leafAKeyDer)),
            fee(XRP(101)),
            ter(tesSUCCESS));
        BEAST_EXPECT(env.current()->exists(
            keylet::validatorDomain(makeSlice(std::string{"a.example"}))));

        // Publish the AccountRoot-backed validator identity and close it into
        // a ledger *before* installing the veto hook. If both sit in the open
        // ledger, close can reorder and let the hook veto the manifest.
        env(manifestSet(
                carrier, makeManifest(validator.pk(), validator.sk(), 1)),
            fee(XRP(101)),
            ter(tesSUCCESS));
        BEAST_EXPECT(env.current()->exists(keylet::validator(validator.pk())));
        BEAST_EXPECT(env.close());
        BEAST_EXPECT(env.closed()->exists(keylet::validator(validator.pk())));

        env(hook(validator, {{hso(veto)}}, 0), fee(XRP(2)), ter(tesSUCCESS));
        BEAST_EXPECT(env.close());
        BEAST_EXPECT(env.current()->exists(keylet::validator(validator.pk())));

        // Third-party carrier domain set is vetoed by the validator's hook.
        env(domainSetFor(
                env,
                carrier,
                validator.pk(),
                validator.sk(),
                "b.example",
                asSlice(leafBDer),
                asSlice(leafBKeyDer)),
            fee(XRP(101)),
            ter(tecHOOK_REJECTED));
        BEAST_EXPECT(!env.current()->exists(
            keylet::validatorDomain(makeSlice(std::string{"b.example"}))));
        // Registry object must remain, without a domain backlink.
        BEAST_EXPECT(env.current()->exists(keylet::validator(validator.pk())));
        if (auto const v =
                env.current()->read(keylet::validator(validator.pk())))
            BEAST_EXPECT(!v->isFieldPresent(sfDomainID));
    }

public:
    void
    run() override
    {
        testOptionalAccountRootVeto();
        testDomainAccountRootVeto();
    }
};

BEAST_DEFINE_TESTSUITE_PRIO(ValidatorIdentityHook, app, ripple, 2);

}  // namespace test
}  // namespace ripple
