#include <xrpld/app/proof/XPOPv1.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>

namespace ripple {
namespace proof {

namespace {

/// Build a validation STObject, sign it, and serialize to hex.
std::string
buildValidationHex(
    uint256 const& ledgerHash,
    std::uint32_t ledgerSeq,
    PublicKey const& signingPublic,
    SecretKey const& signingSecret)
{
    auto val = std::make_shared<STValidation>(
        NetClock::time_point{},
        signingPublic,
        signingSecret,
        calcNodeID(signingPublic),
        [&](STValidation& v) {
            v.setFieldH256(sfLedgerHash, ledgerHash);
            v.setFieldU32(sfLedgerSequence, ledgerSeq);
            v.setFlag(vfFullValidation);
        });

    auto const serialized = val->getSerialized();
    return strHex(makeSlice(serialized));
}

}  // namespace

Json::Value
buildXPOPv1(
    LedgerProof const& proof,
    std::vector<ValidatorKeys> const& validators,
    VLData const& vl)
{
    Json::Value xpop(Json::objectValue);

    // --- ledger section ---
    Json::Value ledger(Json::objectValue);
    ledger[jss::index] = proof.ledgerIndex;
    ledger[jss::coins] = std::to_string(proof.totalCoins);
    ledger[jss::phash] = to_string(proof.parentHash);
    ledger[jss::txroot] = to_string(proof.txRoot);
    ledger[jss::acroot] = to_string(proof.accountRoot);
    ledger[jss::pclose] = proof.parentCloseTime;
    ledger[jss::close] = proof.closeTime;
    ledger[jss::cres] = proof.closeTimeResolution;
    ledger[jss::flags] = proof.closeFlags;
    xpop[jss::ledger] = ledger;

    // --- transaction section ---
    Json::Value transaction(Json::objectValue);
    transaction[jss::blob] = strHex(makeSlice(proof.txBlob));
    transaction[jss::meta] = strHex(makeSlice(proof.metaBlob));

    if (proof.txProof)
        transaction[jss::proof] = proof.txProof->toJsonV1();
    else
        transaction[jss::proof] = Json::Value(Json::arrayValue);

    xpop[jss::transaction] = transaction;

    // --- validation section ---
    auto const ledgerHash = proof.computeLedgerHash();

    Json::Value validation(Json::objectValue);

    // validation.data: map of nodepub → serialized validation hex
    Json::Value data(Json::objectValue);
    for (auto const& vk : validators)
    {
        auto const nodepub = toBase58(TokenType::NodePublic, vk.signingPublic);
        data[nodepub] = buildValidationHex(
            ledgerHash, proof.ledgerIndex, vk.signingPublic, vk.signingSecret);
    }
    validation[jss::data] = data;

    // validation.unl: VL data
    Json::Value unl(Json::objectValue);
    unl[jss::public_key] = strHex(vl.masterPublic);
    unl[jss::manifest] = vl.manifest;
    unl[jss::blob] = vl.blob;
    unl[jss::signature] = vl.signature;
    unl[jss::version] = vl.version;
    validation[jss::unl] = unl;

    xpop[jss::validation] = validation;

    return xpop;
}

Json::Value
buildXPOPv1(
    Ledger const& ledger,
    uint256 const& txHash,
    std::vector<ValidatorKeys> const& validators,
    VLData const& vl)
{
    auto proof = buildLedgerProof(ledger, txHash);
    if (!proof)
        return Json::Value{};
    return buildXPOPv1(*proof, validators, vl);
}

std::string
xpopToHex(Json::Value const& xpop)
{
    auto const json = Json::FastWriter().write(xpop);
    return strHex(json);
}

}  // namespace proof
}  // namespace ripple
