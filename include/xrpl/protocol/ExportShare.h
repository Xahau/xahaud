#ifndef RIPPLE_PROTOCOL_EXPORTSHARE_H_INCLUDED
#define RIPPLE_PROTOCOL_EXPORTSHARE_H_INCLUDED

#include <xrpl/basics/Buffer.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/digest.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace ripple {

/** Canonical post-validation Export signature contribution.

    The multisign signature authenticates the derived destination transaction.
    The remaining fields are bounded lookup and release context which live
    admission checks against the origin latch and local validated chain.
*/
struct ExportShare
{
    static constexpr std::uint8_t currentVersion = 1;

    std::uint8_t version{currentVersion};
    AccountID owner;
    uint256 originTxn;
    LedgerIndex originLedgerSeq{0};
    uint256 originLedgerHash;
    uint256 triggerTxn;
    std::uint16_t committeePosition{0};
    PublicKey signingKey;
    Buffer signature;

    bool
    hasCanonicalSignature() const
    {
        auto const keyType = publicKeyType(signingKey);
        if (!keyType)
            return false;

        auto const sig = Slice{signature.data(), signature.size()};
        if (*keyType == KeyType::secp256k1)
        {
            auto const canonicality = ecdsaCanonicality(sig);
            return canonicality &&
                *canonicality == ECDSACanonicality::fullyCanonical;
        }

        // Full Ed25519 canonicality is enforced by cryptographic admission.
        // The framing layer can still reject every impossible wire length.
        return *keyType == KeyType::ed25519 && signature.size() == 64;
    }

    bool
    validShape() const
    {
        return version == currentVersion && owner != beast::zero &&
            !originTxn.isZero() && originLedgerSeq != 0 &&
            !originLedgerHash.isZero() && !triggerTxn.isZero() &&
            committeePosition < ExportLimits::maxCommitteeMembers &&
            hasCanonicalSignature();
    }

    Serializer
    serialize() const
    {
        if (!validShape())
            throw std::invalid_argument("invalid ExportShare shape");

        Serializer result;
        result.add8(version);
        result.addBitString(owner);
        result.addBitString(originTxn);
        result.add32(originLedgerSeq);
        result.addBitString(originLedgerHash);
        result.addBitString(triggerTxn);
        result.add16(committeePosition);
        result.addRaw(signingKey.slice());
        result.addVL(signature);
        return result;
    }

    uint256
    wireHash() const
    {
        // Raw-wire suppression only. Routing context such as triggerTxn and
        // committeePosition is checked against validated state before relay and
        // is not authenticated by the destination multisignature.
        auto const bytes = serialize();
        return sha512Half(bytes.slice());
    }

    static std::optional<ExportShare>
    parse(Slice bytes)
    {
        if (bytes.empty() ||
            bytes.size() > ExportLimits::maxSerializedExportShareBytes)
            return std::nullopt;

        try
        {
            SerialIter sit{bytes};
            auto const version = sit.get8();
            auto const owner = sit.getBitString<160, detail::AccountIDTag>();
            auto const originTxn = sit.get256();
            auto const originLedgerSeq = sit.get32();
            auto const originLedgerHash = sit.get256();
            auto const triggerTxn = sit.get256();
            auto const committeePosition = sit.get16();
            auto const keySlice = sit.getSlice(33);
            if (!publicKeyType(keySlice))
                return std::nullopt;
            PublicKey const signingKey{keySlice};
            auto signature = sit.getVLBuffer();
            if (!sit.empty())
                return std::nullopt;

            ExportShare result{
                version,
                owner,
                originTxn,
                originLedgerSeq,
                originLedgerHash,
                triggerTxn,
                committeePosition,
                signingKey,
                std::move(signature)};
            if (!result.validShape())
                return std::nullopt;
            return result;
        }
        catch (std::exception const&)
        {
            return std::nullopt;
        }
    }
};

}  // namespace ripple

#endif
