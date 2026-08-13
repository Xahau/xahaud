//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.
*/
//==============================================================================

#include <xrpld/app/tx/detail/ValidatorIdentity.h>

#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/tx/detail/ValidatorIdentityRoot.h>
#include <xrpld/ledger/ApplyView.h>
#include <xrpld/ledger/View.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/digest.h>

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/opensslv.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ripple {
namespace {

constexpr std::size_t maxCertificateChainBytes = 16 * 1024;
constexpr std::size_t maxCertificateCount = 4;

using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using EVPKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using EVPMdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using GeneralNamesPtr =
    std::unique_ptr<GENERAL_NAMES, decltype(&GENERAL_NAMES_free)>;

struct ParsedCertificate
{
    X509Ptr cert{nullptr, X509_free};
    Blob der;
};

bool
sameCertificateDer(Slice a, Slice b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool
isAllowedSignatureNid(int nid)
{
    // Minimum algorithm set every node must implement. New NIDs need an
    // amendment so verification does not depend on the linked OpenSSL.
    return nid == NID_sha256WithRSAEncryption ||
        nid == NID_sha384WithRSAEncryption || nid == NID_ecdsa_with_SHA256 ||
        nid == NID_ecdsa_with_SHA384 || nid == NID_ED25519;
}

bool
isAllowedPublicKey(EVP_PKEY* key)
{
    if (!key)
        return false;
    switch (EVP_PKEY_base_id(key))
    {
        case EVP_PKEY_RSA:
            return EVP_PKEY_bits(key) >= 2048;
        case EVP_PKEY_EC: {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
            char name[64]{};
            std::size_t len = 0;
            if (EVP_PKEY_get_group_name(key, name, sizeof(name), &len) != 1)
                return false;
            auto const group = std::string_view{name, len};
            auto const nid = OBJ_txt2nid(std::string{group}.c_str());
            return nid == NID_X9_62_prime256v1 || nid == NID_secp384r1 ||
                group == "P-256" || group == "P-384";
#else
            auto const* ec = EVP_PKEY_get0_EC_KEY(key);
            if (!ec)
                return false;
            auto const nid = EC_GROUP_get_curve_name(EC_KEY_get0_group(ec));
            return nid == NID_X9_62_prime256v1 || nid == NID_secp384r1;
#endif
        }
        case EVP_PKEY_ED25519:
            return true;
        default:
            return false;
    }
}

std::optional<EVP_MD const*>
statementDigest(EVP_PKEY* key)
{
    // Domain-proof signatures deliberately use SHA-256 for every allowed
    // digest-then-sign key. This is independent of the digest the issuer used
    // to sign the certificate. PureEdDSA takes no external digest.
    switch (EVP_PKEY_base_id(key))
    {
        case EVP_PKEY_RSA:
        case EVP_PKEY_EC:
            return EVP_sha256();
        case EVP_PKEY_ED25519:
            return static_cast<EVP_MD const*>(nullptr);
        default:
            return std::nullopt;
    }
}

std::int64_t
daysFromCivil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    auto const era = (year >= 0 ? year : year - 399) / 400;
    auto const yoe = static_cast<unsigned>(year - era * 400);
    auto const shiftedMonth = month > 2 ? month - 3 : month + 9;
    auto const doy = (153 * shiftedMonth + 2) / 5 + day - 1;
    auto const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + doe - 719468;
}

std::optional<std::uint64_t>
certificateNetTime(ASN1_TIME const* value)
{
    std::tm utc{};
    if (!value || ASN1_TIME_to_tm(value, &utc) != 1 || utc.tm_mon < 0 ||
        utc.tm_mon > 11 || utc.tm_mday < 1 || utc.tm_mday > 31 ||
        utc.tm_hour < 0 || utc.tm_hour > 23 || utc.tm_min < 0 ||
        utc.tm_min > 59 || utc.tm_sec < 0 || utc.tm_sec > 59)
        return std::nullopt;

    auto const unixSeconds =
        daysFromCivil(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday) * 86400 +
        utc.tm_hour * 3600 + utc.tm_min * 60 + utc.tm_sec;
    if (unixSeconds < epoch_offset.count())
        return std::nullopt;
    return static_cast<std::uint64_t>(unixSeconds - epoch_offset.count());
}

bool
certificateValidAt(X509* certificate, std::uint64_t now)
{
    auto const notBefore = certificateNetTime(X509_get0_notBefore(certificate));
    auto const notAfter = certificateNetTime(X509_get0_notAfter(certificate));
    return notBefore && notAfter && *notBefore <= now && now < *notAfter;
}

bool
hasExactDomainSan(X509* certificate, Slice domain)
{
    int critical = -1;
    GeneralNamesPtr names{
        static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(
            certificate, NID_subject_alt_name, &critical, nullptr)),
        GENERAL_NAMES_free};
    if (!names || critical == -2)
        return false;

    for (int i = 0; i < sk_GENERAL_NAME_num(names.get()); ++i)
    {
        auto const* name = sk_GENERAL_NAME_value(names.get(), i);
        if (!name || name->type != GEN_DNS)
            continue;
        auto const* dns = name->d.dNSName;
        auto const length = ASN1_STRING_length(dns);
        auto const* bytes = ASN1_STRING_get0_data(dns);
        if (length >= 0 && static_cast<std::size_t>(length) == domain.size() &&
            std::equal(domain.begin(), domain.end(), bytes))
            return true;
    }
    return false;
}

bool
issuedBy(X509* child, X509* issuer)
{
    if (!child || !issuer)
        return false;
    if (X509_NAME_cmp(
            X509_get_issuer_name(child), X509_get_subject_name(issuer)) != 0)
        return false;
    EVPKeyPtr issuerKey{X509_get_pubkey(issuer), EVP_PKEY_free};
    return issuerKey && X509_verify(child, issuerKey.get()) == 1;
}

Slice
pinnedRoot()
{
    return {
        validator_identity::isrgRootX1Der.data(),
        validator_identity::isrgRootX1Der.size()};
}

uint256
pinnedRootSetID()
{
    static uint256 const id = sha512Half(pinnedRoot());
    return id;
}

bool
isNormalizedDomain(Slice domain)
{
    if (domain.empty() || domain.size() > 253 ||
        domain[domain.size() - 1] == '.')
        return false;

    std::size_t labelStart = 0;
    for (std::size_t i = 0; i <= domain.size(); ++i)
    {
        bool const atEnd = i == domain.size();
        if (!atEnd && domain[i] != '.')
        {
            unsigned char const c = domain[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                return false;
            continue;
        }

        auto const labelSize = i - labelStart;
        if (labelSize == 0 || labelSize > 63 || domain[labelStart] == '-' ||
            domain[i - 1] == '-')
            return false;
        labelStart = i + 1;
    }
    return true;
}

std::vector<ParsedCertificate>
parseCertificateChain(Slice encoded)
{
    // Leaf first, then each issuer. Two-byte big-endian length per DER
    // certificate. The trust-anchor root is not carried in the transaction.
    std::vector<ParsedCertificate> certificates;
    std::size_t offset = 0;
    while (offset < encoded.size())
    {
        if (certificates.size() == maxCertificateCount ||
            encoded.size() - offset < 2)
            return {};
        auto const length = (static_cast<std::size_t>(encoded[offset]) << 8) |
            static_cast<std::size_t>(encoded[offset + 1]);
        offset += 2;
        if (length == 0 || length > encoded.size() - offset)
            return {};

        auto const* cursor =
            reinterpret_cast<unsigned char const*>(encoded.data() + offset);
        auto const* end = cursor + length;
        ParsedCertificate parsed;
        parsed.cert.reset(d2i_X509(nullptr, &cursor, length));
        if (!parsed.cert || cursor != end)
            return {};
        parsed.der.assign(
            encoded.begin() + offset, encoded.begin() + offset + length);
        certificates.push_back(std::move(parsed));
        offset += length;
    }
    return certificates;
}

std::optional<Manifest>
getManifest(STTx const& tx, beast::Journal j)
{
    if (!tx.isFieldPresent(sfManifest))
        return std::nullopt;
    return deserializeManifest(tx.getFieldVL(sfManifest), j);
}

XRPAmount
payloadFee(ReadView const& view, STTx const& tx, SField const& field)
{
    auto fee = Transactor::calculateBaseFee(view, tx);
    if (!tx.isFieldPresent(field))
        return fee;

    // The prototype charges one additional drop per stored input byte. This is
    // intentionally a fee, not an owner reserve: the carrier buys publication
    // but acquires no authority over the internally signed identity assertion.
    auto const bytes = tx.getFieldVL(field).size();
    if (bytes > std::numeric_limits<std::int64_t>::max())
        return INITIAL_XRP;
    return fee + XRPAmount{static_cast<std::int64_t>(bytes)};
}

}  // namespace

DomainTrustAnchor
productionDomainTrustAnchor()
{
    return {pinnedRoot(), pinnedRootSetID()};
}

Serializer
domainBindingStatement(STTx const& tx, std::uint32_t networkID)
{
    // Canonical, domain-separated bytes signed independently by the WebPKI
    // leaf key and validator master. The two expected hashes make replacement
    // compare-and-swap rather than last-wrapper-wins.
    static constexpr std::array<std::uint8_t, 8> prefix = {
        'X', 'A', 'H', 'A', 'U', 'V', 'D', 1};
    Serializer s;
    s.addRaw(prefix.data(), prefix.size());
    s.add32(networkID);
    s.addVL(tx.getFieldVL(sfDomain));
    s.addVL(tx.getFieldVL(sfValidatorPublicKey));
    s.add64(tx.getFieldU64(sfNotBefore));
    s.add64(tx.getFieldU64(sfNotAfter));
    s.addBitString(tx.getFieldH256(sfRootSetID));
    s.addBitString(tx.getFieldH256(sfExpectedDomainHash));
    s.addBitString(tx.getFieldH256(sfExpectedValidatorDomainHash));
    return s;
}

bool
verifyDomainProof(
    STTx const& tx,
    std::uint32_t networkID,
    NetClock::time_point parentCloseTime,
    DomainTrustAnchor const& trustAnchor)
{
    if (trustAnchor.der.empty() ||
        tx.getFieldH256(sfRootSetID) != trustAnchor.rootSetID)
        return false;

    auto const masterBytes = tx.getFieldVL(sfValidatorPublicKey);
    if (!publicKeyType(makeSlice(masterBytes)))
        return false;
    PublicKey const master{makeSlice(masterBytes)};
    auto const statement = domainBindingStatement(tx, networkID);
    if (!verify(
            master,
            statement.slice(),
            makeSlice(tx.getFieldVL(sfValidatorMasterSignature))))
        return false;

    auto certificates =
        parseCertificateChain(makeSlice(tx.getFieldVL(sfCertificateChain)));
    if (certificates.empty())
        return false;

    auto const rootBytes = trustAnchor.der;
    auto const* rootCursor =
        reinterpret_cast<unsigned char const*>(rootBytes.data());
    X509Ptr root{d2i_X509(nullptr, &rootCursor, rootBytes.size()), X509_free};
    if (!root || rootCursor != rootBytes.data() + rootBytes.size())
        return false;

    // Exact ordered path: presented certs are leaf then issuers. The compiled
    // root is the trust anchor and must not appear in the transaction.
    for (auto const& parsed : certificates)
    {
        if (sameCertificateDer(makeSlice(parsed.der), rootBytes))
            return false;
        if (!isAllowedSignatureNid(X509_get_signature_nid(parsed.cert.get())))
            return false;
        EVPKeyPtr subjectKey{X509_get_pubkey(parsed.cert.get()), EVP_PKEY_free};
        if (!isAllowedPublicKey(subjectKey.get()))
            return false;
    }
    if (!isAllowedSignatureNid(X509_get_signature_nid(root.get())))
        return false;
    EVPKeyPtr rootKey{X509_get_pubkey(root.get()), EVP_PKEY_free};
    if (X509_check_ca(root.get()) <= 0 || !isAllowedPublicKey(rootKey.get()))
        return false;

    auto const nowCount = parentCloseTime.time_since_epoch().count();
    if (nowCount < 0)
        return false;
    auto const now = static_cast<std::uint64_t>(nowCount);
    if (!certificateValidAt(root.get(), now))
        return false;
    for (auto const& parsed : certificates)
        if (!certificateValidAt(parsed.cert.get(), now))
            return false;

    for (std::size_t i = 0; i + 1 < certificates.size(); ++i)
    {
        if (!issuedBy(
                certificates[i].cert.get(), certificates[i + 1].cert.get()))
            return false;
        if (X509_check_ca(certificates[i + 1].cert.get()) <= 0)
            return false;
    }
    if (!issuedBy(certificates.back().cert.get(), root.get()))
        return false;

    X509* const leaf = certificates.front().cert.get();
    if (X509_check_ca(leaf) > 0)
        return false;
    if (X509_check_purpose(leaf, X509_PURPOSE_SSL_SERVER, 0) != 1)
        return false;
    auto const domain = tx.getFieldVL(sfDomain);
    if (!hasExactDomainSan(leaf, makeSlice(domain)))
        return false;

    auto const notBefore = tx.getFieldU64(sfNotBefore);
    auto const notAfter = tx.getFieldU64(sfNotAfter);
    if (notBefore > now || notAfter <= now || notBefore >= notAfter)
        return false;

    auto const leafNotBefore = certificateNetTime(X509_get0_notBefore(leaf));
    auto const leafNotAfter = certificateNetTime(X509_get0_notAfter(leaf));
    if (!leafNotBefore || !leafNotAfter || *leafNotBefore > notBefore ||
        *leafNotAfter < notAfter)
        return false;

    EVPKeyPtr domainKey{X509_get_pubkey(leaf), EVP_PKEY_free};
    auto const digest = statementDigest(domainKey.get());
    if (!domainKey || !digest)
        return false;
    EVPMdCtxPtr digestContext{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    auto const domainSignature = tx.getFieldVL(sfDomainSignature);
    return digestContext &&
        EVP_DigestVerifyInit(
            digestContext.get(), nullptr, *digest, nullptr, domainKey.get()) ==
        1 &&
        EVP_DigestVerify(
            digestContext.get(),
            domainSignature.data(),
            domainSignature.size(),
            static_cast<unsigned char const*>(statement.data()),
            statement.size()) == 1;
}

bool
isDomainBindingActive(
    ReadView const& view,
    SLE const& domainObject,
    NetClock::time_point ledgerTime)
{
    auto const now = ledgerTime.time_since_epoch().count();
    auto const notBefore = domainObject.getFieldU64(sfNotBefore);
    auto const notAfter = domainObject.getFieldU64(sfNotAfter);
    if (!(notBefore <= now && now < notAfter))
        return false;

    auto const masterBytes = domainObject.getFieldVL(sfValidatorPublicKey);
    if (!publicKeyType(makeSlice(masterBytes)))
        return false;

    auto const validator =
        view.read(keylet::validator(PublicKey{makeSlice(masterBytes)}));
    if (!validator || Manifest::revoked(validator->getFieldU32(sfSequence)) ||
        !validator->isFieldPresent(sfDomainID) ||
        validator->getFieldH256(sfDomainID) != domainObject.key())
        return false;

    return true;
}

TER
applyValidatorDomainBinding(
    ApplyView& view,
    STTx const& tx,
    std::uint32_t networkID)
{
    auto const domain = tx.getFieldVL(sfDomain);
    auto const masterBytes = tx.getFieldVL(sfValidatorPublicKey);
    if (!publicKeyType(makeSlice(masterBytes)))
        return tefINTERNAL;
    PublicKey const master{makeSlice(masterBytes)};
    auto const validatorKeylet = keylet::validator(master);
    auto validator = view.peek(validatorKeylet);
    if (!validator)
        return tecNO_ENTRY;

    // Terminally revoked masters may not create or replace domain bindings.
    // Stored predecessor objects for previously bound domains remain for CAS.
    if (Manifest::revoked(validator->getFieldU32(sfSequence)))
        return tecNO_PERMISSION;

    auto const domainKeylet = keylet::validatorDomain(makeSlice(domain));
    auto domainObject = view.peek(domainKeylet);
    uint256 const currentDomainHash =
        domainObject ? domainObject->getFieldH256(sfDigest) : beast::zero;
    if (tx.getFieldH256(sfExpectedDomainHash) != currentDomainHash)
        return tecDUPLICATE;

    uint256 const currentValidatorDomain = validator->isFieldPresent(sfDomainID)
        ? validator->getFieldH256(sfDomainID)
        : beast::zero;
    if (tx.getFieldH256(sfExpectedValidatorDomainHash) !=
        currentValidatorDomain)
        return tecDUPLICATE;

    // The WebPKI holder may transfer a domain to a new validator master. Clear
    // the displaced validator backlink only when it still names this exact
    // domain object; a stale wrapper cannot clear an unrelated binding.
    if (domainObject)
    {
        auto const oldMasterBytes =
            domainObject->getFieldVL(sfValidatorPublicKey);
        if (!publicKeyType(makeSlice(oldMasterBytes)))
            return tefINTERNAL;
        PublicKey const oldMaster{makeSlice(oldMasterBytes)};
        if (oldMaster != master)
        {
            if (auto oldValidator = view.peek(keylet::validator(oldMaster));
                oldValidator && oldValidator->isFieldPresent(sfDomainID) &&
                oldValidator->getFieldH256(sfDomainID) == domainKeylet.key)
            {
                oldValidator->makeFieldAbsent(sfDomainID);
                view.update(oldValidator);
            }
        }
        view.erase(domainObject);
        domainObject.reset();
    }

    // A validator has one current certified domain. Replacing it removes the
    // old forward object after the signed compare-and-swap pointer matched.
    if (currentValidatorDomain.isNonZero() &&
        currentValidatorDomain != domainKeylet.key)
    {
        auto oldDomain =
            view.peek(Keylet{ltVALIDATOR_DOMAIN, currentValidatorDomain});
        if (oldDomain)
            view.erase(oldDomain);
    }

    auto const statement = domainBindingStatement(tx, networkID);
    domainObject = std::make_shared<SLE>(domainKeylet);
    domainObject->setFieldVL(sfDomain, makeSlice(domain));
    domainObject->setFieldVL(sfValidatorPublicKey, master.slice());
    domainObject->setFieldVL(
        sfCertificateChain, makeSlice(tx.getFieldVL(sfCertificateChain)));
    domainObject->setFieldVL(
        sfDomainSignature, makeSlice(tx.getFieldVL(sfDomainSignature)));
    domainObject->setFieldVL(
        sfValidatorMasterSignature,
        makeSlice(tx.getFieldVL(sfValidatorMasterSignature)));
    // Store the RootSetID the proof committed to (production preclaim forces
    // this to the embedded ISRG root; tests may use a private test CA).
    domainObject->setFieldH256(sfRootSetID, tx.getFieldH256(sfRootSetID));
    domainObject->setFieldU64(sfNotBefore, tx.getFieldU64(sfNotBefore));
    domainObject->setFieldU64(sfNotAfter, tx.getFieldU64(sfNotAfter));
    domainObject->setFieldH256(sfDigest, sha512Half(statement.slice()));
    view.insert(domainObject);

    validator->setFieldH256(sfDomainID, domainKeylet.key);
    view.update(validator);
    return tesSUCCESS;
}

NotTEC
ValidatorManifestSet::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureOnlineValidatorIdentity))
        return temDISABLED;
    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto manifest = getManifest(ctx.tx, ctx.j);
    if (!manifest || !manifest->verify())
        return temMALFORMED;

    return preflight2(ctx);
}

XRPAmount
ValidatorManifestSet::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    auto fee = payloadFee(view, tx, sfManifest);
    if (auto manifest =
            getManifest(tx, beast::Journal(beast::Journal::getNullSink()));
        manifest && !view.exists(keylet::validator(manifest->masterKey)))
    {
        // Initial publication creates the stable master object and, unless it
        // is a terminal revocation, one reverse signing-key object. The payer
        // burns the equivalent owner reserves without becoming an owner.
        fee += view.fees().increment * (manifest->signingKey ? 2 : 1);
    }
    return fee;
}

TER
ValidatorManifestSet::doApply()
{
    auto manifest = getManifest(ctx_.tx, ctx_.journal);
    if (!manifest || !manifest->verify())
        return tefINTERNAL;

    auto const master = manifest->masterKey;
    auto const validatorKeylet = keylet::validator(master);
    auto validator = view().peek(validatorKeylet);
    bool const creating = !validator;

    if (validator)
    {
        auto const current = validator->getFieldU32(sfSequence);
        if (manifest->sequence <= current)
            return tecDUPLICATE;
    }

    // Current role uniqueness mirrors ManifestCache, but is checked against
    // the transaction ledger view so every node makes the same state change.
    if (view().exists(keylet::validatorManifest(master)))
        return tecDUPLICATE;

    if (manifest->signingKey)
    {
        if (view().exists(keylet::validator(*manifest->signingKey)))
            return tecDUPLICATE;

        if (auto const reverse =
                view().read(keylet::validatorManifest(*manifest->signingKey));
            reverse)
        {
            auto const reverseMaster =
                reverse->getFieldVL(sfValidatorPublicKey);
            if (!publicKeyType(makeSlice(reverseMaster)) ||
                PublicKey{makeSlice(reverseMaster)} != master)
                return tecDUPLICATE;
        }
    }

    if (validator && validator->isFieldPresent(sfSigningPubKey))
    {
        auto const old = validator->getFieldVL(sfSigningPubKey);
        if (publicKeyType(makeSlice(old)))
        {
            auto oldReverse = view().peek(
                keylet::validatorManifest(PublicKey{makeSlice(old)}));
            if (oldReverse)
                view().erase(oldReverse);
        }
    }

    auto const manifestHash = manifest->hash();
    if (creating)
    {
        validator = std::make_shared<SLE>(validatorKeylet);
        validator->setFieldVL(sfPublicKey, master.slice());
    }

    validator->setFieldVL(sfManifest, makeSlice(manifest->serialized));
    validator->setFieldH256(sfManifestHash, manifestHash);
    validator->setFieldU32(sfSequence, manifest->sequence);

    if (manifest->signingKey)
    {
        validator->setFieldVL(sfSigningPubKey, manifest->signingKey->slice());

        auto reverse = std::make_shared<SLE>(
            keylet::validatorManifest(*manifest->signingKey));
        reverse->setFieldVL(sfPublicKey, manifest->signingKey->slice());
        reverse->setFieldVL(sfValidatorPublicKey, master.slice());
        reverse->setFieldH256(sfManifestHash, manifestHash);
        view().insert(reverse);
    }
    else
    {
        validator->makeFieldAbsent(sfSigningPubKey);
    }

    if (creating)
        view().insert(validator);
    else
        view().update(validator);

    return tesSUCCESS;
}

NotTEC
ValidatorDomainSet::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureOnlineValidatorIdentity))
        return temDISABLED;
    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto const domain = ctx.tx.getFieldVL(sfDomain);
    auto const chain = ctx.tx.getFieldVL(sfCertificateChain);
    auto const validatorKey = ctx.tx.getFieldVL(sfValidatorPublicKey);
    if (!isNormalizedDomain(makeSlice(domain)) || chain.empty() ||
        chain.size() > maxCertificateChainBytes ||
        !publicKeyType(makeSlice(validatorKey)) ||
        ctx.tx.getFieldVL(sfDomainSignature).empty() ||
        ctx.tx.getFieldVL(sfValidatorMasterSignature).empty())
        return temMALFORMED;

    return preflight2(ctx);
}

XRPAmount
ValidatorDomainSet::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    auto fee = payloadFee(view, tx, sfCertificateChain);
    fee += XRPAmount{
        static_cast<std::int64_t>(tx.getFieldVL(sfDomainSignature).size())};
    fee += XRPAmount{static_cast<std::int64_t>(
        tx.getFieldVL(sfValidatorMasterSignature).size())};

    auto const masterBytes = tx.getFieldVL(sfValidatorPublicKey);
    if (publicKeyType(makeSlice(masterBytes)))
    {
        auto const validator =
            view.read(keylet::validator(PublicKey{makeSlice(masterBytes)}));
        auto const domainKey =
            keylet::validatorDomain(makeSlice(tx.getFieldVL(sfDomain)));
        // Charge a state-growth fee only when this adds a domain object rather
        // than replacing the validator's existing one.
        if (!view.exists(domainKey) &&
            (!validator || !validator->isFieldPresent(sfDomainID)))
            fee += view.fees().increment;
    }
    return fee;
}

TER
ValidatorDomainSet::preclaim(PreclaimContext const& ctx)
{
    // Application-owned immutable anchor (ISRG Root X1 unless a jtx Env
    // injected a private test root at Application construction).
    if (!verifyDomainProof(
            ctx.tx,
            ctx.app.config().NETWORK_ID,
            ctx.view.parentCloseTime(),
            ctx.app.domainTrustAnchor()))
        return tecNO_PERMISSION;
    return tesSUCCESS;
}

TER
ValidatorDomainSet::doApply()
{
    return applyValidatorDomainBinding(
        view(), ctx_.tx, ctx_.app.config().NETWORK_ID);
}

}  // namespace ripple
