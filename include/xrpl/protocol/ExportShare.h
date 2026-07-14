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
    std::uint16_t universePosition{0};
    PublicKey signingKey;
    Buffer signature;

    bool
    validShape() const
    {
        return version == currentVersion && owner != beast::zero &&
            !originTxn.isZero() && originLedgerSeq != 0 &&
            !originLedgerHash.isZero() && !triggerTxn.isZero() &&
            universePosition < ExportLimits::maxValidatorUniverseMembers &&
            !signature.empty() &&
            signature.size() <= ExportSigCollectorV2SignatureMax;
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
        result.add16(universePosition);
        result.addRaw(signingKey.slice());
        result.addVL(signature);
        return result;
    }

    uint256
    contentHash() const
    {
        auto const bytes = serialize();
        return sha512Half(bytes.slice());
    }

    static std::optional<ExportShare>
    parse(Slice bytes)
    {
        if (bytes.empty() ||
            bytes.size() > ExportLimits::maxExportShareRelayBytes)
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
            auto const universePosition = sit.get16();
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
                universePosition,
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

private:
    // Canonical secp256k1 DER signatures are at most 72 bytes; ed25519 uses
    // 64. Kept local to avoid coupling the protocol codec to collector code.
    static constexpr std::size_t ExportSigCollectorV2SignatureMax = 72;
};

}  // namespace ripple

#endif
