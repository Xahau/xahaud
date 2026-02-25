#include <xrpld/app/hook/HookAPI.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/misc/NetworkOPs.h>
#include <xrpld/app/misc/Transaction.h>
#include <xrpld/app/misc/TxQ.h>
#include <xrpld/app/tx/detail/Import.h>
#include <xrpld/app/tx/detail/NFTokenUtils.h>
#include <xrpl/basics/Log.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_value.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/st.h>
#include <xrpl/protocol/tokens.h>
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <any>
#include <cfenv>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <quickjs/quickjs.h>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <wasmedge/wasmedge.h>

using namespace ripple;
// check if any std::optionals are missing (any !has_value())
template <typename... Optionals>
inline bool
any_missing(const Optionals&... optionals)
{
    return ((!optionals.has_value() || ...));
}

// check if all optional ints are within uint32_t range
template <typename... OptionalInts>
inline bool
fits_u32(const OptionalInts&... optionals)
{
    constexpr uint32_t uint32_max = std::numeric_limits<uint32_t>::max();
    return (
        (optionals.has_value() && *optionals >= 0 &&
         *optionals <= uint32_max) &&
        ...);
}

// check if all optional ints are within int32_t range
template <typename... OptionalInts>
inline bool
fits_i32(const OptionalInts&... optionals)
{
    constexpr int32_t int32_max = std::numeric_limits<int32_t>::max();
    constexpr int32_t int32_min = std::numeric_limits<int32_t>::min();
    return (
        (optionals.has_value() && *optionals >= int32_min &&
         *optionals <= int32_max) &&
        ...);
}

// execute FromJSInt on more than one variable at the same time
template <typename... Args>
auto
FromJSInts(JSContext* ctx, Args... args)
{
    return std::make_tuple(FromJSInt(ctx, args)...);
}

namespace hook {
std::vector<std::pair<AccountID, bool>>
getTransactionalStakeHolders(STTx const& tx, ReadView const& rv)
{
    if (!rv.rules().enabled(featureHooks))
        return {};

    if (!tx.isFieldPresent(sfAccount))
        return {};

    std::optional<AccountID> destAcc = tx.at(~sfDestination);
    std::optional<AccountID> otxnAcc = tx.at(~sfAccount);

    if (!otxnAcc)
        return {};

    uint16_t tt = tx.getFieldU16(sfTransactionType);

    std::map<AccountID, std::pair<int, bool>> tshEntries;

    int upto = 0;

    auto const ADD_TSH = [&otxnAcc, &tshEntries, &upto](
                             const AccountID& acc_r, bool rb) {
        if (acc_r != *otxnAcc)
        {
            if (tshEntries.find(acc_r) != tshEntries.end())
                tshEntries[acc_r].second |= rb;
            else
                tshEntries.emplace(acc_r, std::make_pair(upto++, rb));
        }
    };

    bool const tshSTRONG = true;  // tshROLLBACK
    bool const tshWEAK = false;   // tshCOLLECT

    auto const getNFTOffer =
        [](std::optional<uint256> id,
           ReadView const& rv) -> std::shared_ptr<const SLE> {
        if (!id || *id == beast::zero)
            return nullptr;

        return rv.read(keylet::nftoffer(*id));
    };

    bool const fixV1 = rv.rules().enabled(fixXahauV1);
    bool const fixV2 = rv.rules().enabled(fixXahauV2);

    switch (tt)
    {
        case ttCRON: {
            ADD_TSH(tx.getAccountID(sfOwner), tshWEAK);
            break;
        }

        case ttREMIT: {
            if (destAcc)
                ADD_TSH(*destAcc, tshSTRONG);

            if (tx.isFieldPresent(sfInform))
            {
                auto const inform = tx.getAccountID(sfInform);
                if (*otxnAcc != inform && *destAcc != inform)
                    ADD_TSH(inform, tshWEAK);
            }

            if (tx.isFieldPresent(sfURITokenIDs))
            {
                STVector256 tokenIds = tx.getFieldV256(sfURITokenIDs);
                for (uint256 const klRaw : tokenIds)
                {
                    Keylet const id{ltURI_TOKEN, klRaw};
                    if (!rv.exists(id))
                        continue;

                    auto const ut = rv.read(id);
                    if (!ut ||
                        ut->getFieldU16(sfLedgerEntryType) != ltURI_TOKEN)
                        continue;

                    auto const owner = ut->getAccountID(sfOwner);
                    auto const issuer = ut->getAccountID(sfIssuer);
                    if (issuer != owner && issuer != *destAcc)
                    {
                        ADD_TSH(
                            issuer,
                            (ut->getFlags() & lsfBurnable) ? tshSTRONG
                                                           : tshWEAK);
                    }
                }
            }
            break;
        }

        case ttIMPORT: {
            if (tx.isFieldPresent(sfIssuer))
                ADD_TSH(tx.getAccountID(sfIssuer), fixV2 ? tshWEAK : tshSTRONG);
            break;
        }

        case ttURITOKEN_BURN: {
            Keylet const id{ltURI_TOKEN, tx.getFieldH256(sfURITokenID)};
            if (!rv.exists(id))
                return {};

            auto const ut = rv.read(id);
            if (!ut || ut->getFieldU16(sfLedgerEntryType) != ltURI_TOKEN)
                return {};

            auto const owner = ut->getAccountID(sfOwner);
            auto const issuer = ut->getAccountID(sfIssuer);

            // three possible burn scenarios:
            //  the burner is the owner and issuer of the token
            //  the burner is the owner and not the issuer of the token
            //  the burner is the issuer and not the owner of the token

            if (issuer == owner)
                break;
            // pass, already a TSH

            // new logic
            if (fixV1)
            {
                // the owner burns their token, and the issuer is a weak TSH
                if (*otxnAcc == owner && rv.exists(keylet::account(issuer)))
                    ADD_TSH(issuer, tshWEAK);
                // the issuer burns the owner's token, and the owner is a weak
                // TSH
                else if (rv.exists(keylet::account(owner)))
                    ADD_TSH(owner, tshWEAK);

                break;
            }

            // old logic
            {
                if (*otxnAcc == owner)
                {
                    // the owner burns their token, and the issuer is a weak TSH
                    ADD_TSH(issuer, tshSTRONG);
                }
                else
                {
                    // the issuer burns the owner's token, and the owner is a
                    // weak TSH
                    ADD_TSH(owner, tshSTRONG);
                }
            }

            break;
        }

        case ttURITOKEN_BUY: {
            Keylet const id{ltURI_TOKEN, tx.getFieldH256(sfURITokenID)};
            if (!rv.exists(id))
                return {};

            auto const ut = rv.read(id);
            if (!ut || ut->getFieldU16(sfLedgerEntryType) != ltURI_TOKEN)
                return {};

            auto const owner = ut->getAccountID(sfOwner);

            if (owner != tx.getAccountID(sfAccount))
            {
                // current owner is a strong TSH
                ADD_TSH(owner, tshSTRONG);
            }

            // issuer is also a strong TSH if the burnable flag is set
            auto const issuer = ut->getAccountID(sfIssuer);
            if (issuer != owner)
                ADD_TSH(
                    issuer,
                    (ut->getFlags() & lsfBurnable) ? tshSTRONG : tshWEAK);

            break;
        }

        case ttURITOKEN_MINT: {
            // destination is a strong tsh
            if (fixV2 && tx.isFieldPresent(sfDestination))
                ADD_TSH(tx.getAccountID(sfDestination), tshSTRONG);
            break;
        }

        case ttURITOKEN_CANCEL_SELL_OFFER: {
            if (!fixV2)
                break;

            Keylet const id{ltURI_TOKEN, tx.getFieldH256(sfURITokenID)};
            if (!rv.exists(id))
                return {};

            auto const ut = rv.read(id);
            if (!ut || ut->getFieldU16(sfLedgerEntryType) != ltURI_TOKEN)
                return {};

            if (ut->isFieldPresent(sfDestination))
            {
                auto const dest = ut->getAccountID(sfDestination);
                ADD_TSH(dest, tshWEAK);
            }
            break;
        }

        case ttURITOKEN_CREATE_SELL_OFFER: {
            Keylet const id{ltURI_TOKEN, tx.getFieldH256(sfURITokenID)};
            if (!rv.exists(id))
                return {};

            auto const ut = rv.read(id);
            if (!ut || ut->getFieldU16(sfLedgerEntryType) != ltURI_TOKEN)
                return {};

            auto const owner = ut->getAccountID(sfOwner);
            auto const issuer = ut->getAccountID(sfIssuer);

            // issuer is a strong TSH if the burnable flag is set
            if (issuer != owner)
                ADD_TSH(
                    issuer,
                    (ut->getFlags() & lsfBurnable) ? tshSTRONG : tshWEAK);

            // destination is a strong tsh
            if (tx.isFieldPresent(sfDestination))
                ADD_TSH(tx.getAccountID(sfDestination), tshSTRONG);

            break;
        }

        // NFT
        case ttNFTOKEN_MINT:
        case ttCLAIM_REWARD: {
            if (tx.isFieldPresent(sfIssuer))
                ADD_TSH(tx.getAccountID(sfIssuer), tshSTRONG);
            break;
        };

        case ttNFTOKEN_BURN:
        case ttNFTOKEN_CREATE_OFFER: {
            if (!tx.isFieldPresent(sfNFTokenID) ||
                !tx.isFieldPresent(sfAccount))
                return {};

            uint256 nid = tx.getFieldH256(sfNFTokenID);
            bool hasOwner = tx.isFieldPresent(sfOwner);
            AccountID owner = tx.getAccountID(hasOwner ? sfOwner : sfAccount);

            if (!nft::findToken(rv, owner, nid))
                return {};

            auto const issuer = nft::getIssuer(nid);

            bool issuerCanRollback = nft::getFlags(nid) & tfStrongTSH;

            ADD_TSH(issuer, issuerCanRollback);
            if (hasOwner)
                ADD_TSH(owner, tshWEAK);
            break;
        }

        case ttNFTOKEN_ACCEPT_OFFER: {
            auto const bo = getNFTOffer(tx[~sfNFTokenBuyOffer], rv);
            auto const so = getNFTOffer(tx[~sfNFTokenSellOffer], rv);

            if (!bo && !so)
                return {};

            // issuer only has rollback ability if NFT specifies it in flags
            uint256 nid = (bo ? bo : so)->getFieldH256(sfNFTokenID);
            auto const issuer = nft::getIssuer(nid);
            bool issuerCanRollback = nft::getFlags(nid) & tfStrongTSH;
            ADD_TSH(issuer, issuerCanRollback);

            if (bo)
            {
                ADD_TSH(bo->getAccountID(sfOwner), tshSTRONG);
                if (bo->isFieldPresent(sfDestination))
                    ADD_TSH(bo->getAccountID(sfDestination), tshSTRONG);
            }

            if (so)
            {
                ADD_TSH(so->getAccountID(sfOwner), tshSTRONG);
                if (so->isFieldPresent(sfDestination))
                    ADD_TSH(so->getAccountID(sfDestination), tshSTRONG);
            }

            break;
        }

        case ttNFTOKEN_CANCEL_OFFER: {
            if (!tx.isFieldPresent(sfNFTokenOffers))
                return {};

            auto const& offerVec = tx.getFieldV256(sfNFTokenOffers);
            for (auto const& offerID : offerVec)
            {
                auto const offer = getNFTOffer(offerID, rv);
                if (offer)
                {
                    ADD_TSH(offer->getAccountID(sfOwner), tshWEAK);
                    if (offer->isFieldPresent(sfDestination))
                        ADD_TSH(offer->getAccountID(sfDestination), tshWEAK);

                    // issuer can't stop people canceling their offers, but can
                    // get weak executions
                    uint256 nid = offer->getFieldH256(sfNFTokenID);
                    auto const issuer = nft::getIssuer(nid);
                    ADD_TSH(issuer, tshWEAK);
                }
            }
            break;
        }

        // self transactions
        case ttACCOUNT_SET:
        case ttOFFER_CANCEL:
        case ttTICKET_CREATE:
        case ttHOOK_SET:
        case ttOFFER_CREATE: {
            break;
        }

        case ttREGULAR_KEY_SET: {
            if (!tx.isFieldPresent(sfRegularKey))
                return {};
            ADD_TSH(tx.getAccountID(sfRegularKey), tshSTRONG);
            break;
        }

        case ttDEPOSIT_PREAUTH: {
            if (!tx.isFieldPresent(sfAuthorize))
                return {};
            ADD_TSH(tx.getAccountID(sfAuthorize), tshSTRONG);
            break;
        }

        // simple two party transactions
        case ttPAYMENT:
        case ttESCROW_CREATE:
        case ttCHECK_CREATE:
        case ttACCOUNT_DELETE:
        case ttPAYCHAN_CREATE:
        case ttINVOKE: {
            if (destAcc)
                ADD_TSH(*destAcc, tshSTRONG);
            break;
        }

        case ttTRUST_SET: {
            if (!tx.isFieldPresent(sfLimitAmount))
                return {};

            auto const& lim = tx.getFieldAmount(sfLimitAmount);
            AccountID const& issuer = lim.getIssuer();

            ADD_TSH(issuer, tshWEAK);
            break;
        }

        case ttESCROW_CANCEL:
        case ttESCROW_FINISH: {
            // new logic
            if (fixV1)
            {
                if (!tx.isFieldPresent(sfOwner))
                    return {};

                AccountID const owner = tx.getAccountID(sfOwner);

                bool const hasSeq = tx.isFieldPresent(sfOfferSequence);
                bool const hasID = tx.isFieldPresent(sfEscrowID);
                if (!hasSeq && !hasID)
                    return {};

                Keylet kl = hasSeq
                    ? keylet::escrow(owner, tx.getFieldU32(sfOfferSequence))
                    : Keylet(ltESCROW, tx.getFieldH256(sfEscrowID));

                auto escrow = rv.read(kl);

                if (!escrow ||
                    escrow->getFieldU16(sfLedgerEntryType) != ltESCROW)
                    return {};

                // this should always be the same as owner, but defensively...
                AccountID const src = escrow->getAccountID(sfAccount);
                AccountID const dst = escrow->getAccountID(sfDestination);

                // the source account is a strong transacitonal stakeholder for
                // fin and can
                ADD_TSH(src, tshSTRONG);

                // the dest acc is a strong tsh for fin and weak for can
                if (src != dst)
                    ADD_TSH(dst, tt == ttESCROW_FINISH ? tshSTRONG : tshWEAK);

                break;
            }
            // old logic
            {
                if (!tx.isFieldPresent(sfOwner) ||
                    !tx.isFieldPresent(sfOfferSequence))
                    return {};

                auto escrow = rv.read(keylet::escrow(
                    tx.getAccountID(sfOwner), tx.getFieldU32(sfOfferSequence)));

                if (!escrow)
                    return {};

                ADD_TSH(escrow->getAccountID(sfAccount), tshSTRONG);
                ADD_TSH(
                    escrow->getAccountID(sfDestination),
                    tt == ttESCROW_FINISH ? tshSTRONG : tshWEAK);
                break;
            }
        }

        case ttPAYCHAN_FUND:
        case ttPAYCHAN_CLAIM: {
            if (!tx.isFieldPresent(sfChannel))
                return {};

            auto chan = rv.read(Keylet{ltPAYCHAN, tx.getFieldH256(sfChannel)});
            if (!chan)
                return {};

            ADD_TSH(chan->getAccountID(sfAccount), tshSTRONG);
            ADD_TSH(chan->getAccountID(sfDestination), tshWEAK);
            break;
        }

        case ttCHECK_CASH:
        case ttCHECK_CANCEL: {
            if (!tx.isFieldPresent(sfCheckID))
                return {};

            auto check = rv.read(Keylet{ltCHECK, tx.getFieldH256(sfCheckID)});
            if (!check)
                return {};

            ADD_TSH(check->getAccountID(sfAccount), tshSTRONG);
            ADD_TSH(check->getAccountID(sfDestination), tshWEAK);
            break;
        }

        // the owners of accounts whose keys appear on a signer list are
        // entitled to prevent their inclusion
        case ttSIGNER_LIST_SET: {
            STArray const& signerEntries = tx.getFieldArray(sfSignerEntries);
            for (auto const& entryObj : signerEntries)
                if (entryObj.isFieldPresent(sfAccount))
                    ADD_TSH(entryObj.getAccountID(sfAccount), tshSTRONG);
            break;
        }

        case ttGENESIS_MINT: {
            if (tx.isFieldPresent(sfGenesisMints))
            {
                auto const& mints = tx.getFieldArray(sfGenesisMints);
                for (auto const& mint : mints)
                {
                    if (mint.isFieldPresent(sfDestination))
                    {
                        ADD_TSH(mint.getAccountID(sfDestination), tshWEAK);
                    }
                }
            }
            break;
        }

        case ttCLAWBACK: {
            auto const amount = tx.getFieldAmount(sfAmount);

            if (amount.holds<MPTIssue>())
            {
                if (!tx.isFieldPresent(sfHolder))
                    return {};
                auto const holder = tx.getAccountID(sfHolder);
                ADD_TSH(holder, tshWEAK);
            }
            else
                ADD_TSH(amount.getIssuer(), tshWEAK);

            break;
        }

        case ttCRON_SET: {
            break;
        }
        case ttAMM_CREATE:
        case ttAMM_DEPOSIT:
        case ttAMM_WITHDRAW:
        case ttAMM_VOTE:
        case ttAMM_BID:
        case ttAMM_DELETE:
        case ttAMM_CLAWBACK: {
            // The issuer or holder of tokens related to AMM is weakTSH with
            // IOUIssuerWeakTSH Amendment.
            break;
        }
        case ttORACLE_SET:
        case ttORACLE_DELETE: {
            break;
        }
        case ttXCHAIN_CREATE_CLAIM_ID:
        case ttXCHAIN_COMMIT:
        case ttXCHAIN_CLAIM:
        case ttXCHAIN_ACCOUNT_CREATE_COMMIT:
        case ttXCHAIN_ADD_CLAIM_ATTESTATION:
        case ttXCHAIN_ADD_ACCOUNT_CREATE_ATTESTATION:
        case ttXCHAIN_MODIFY_BRIDGE:
        case ttXCHAIN_CREATE_BRIDGE: {
            // TODO: Implement if needed
            break;
        }
        case ttDID_SET:
        case ttDID_DELETE: {
            // TODO: Implement if needed
            break;
        }
        case ttLEDGER_STATE_FIX: {
            // TODO: Implement if needed
            break;
        }
        case ttMPTOKEN_ISSUANCE_CREATE:
        case ttMPTOKEN_ISSUANCE_DESTROY:
        case ttMPTOKEN_ISSUANCE_SET:
        case ttMPTOKEN_AUTHORIZE: {
            // TODO: Implement if needed
            break;
        }
        case ttCREDENTIAL_CREATE:
        case ttCREDENTIAL_ACCEPT:
        case ttCREDENTIAL_DELETE: {
            // TODO: Implement if needed
            break;
        }
        case ttNFTOKEN_MODIFY: {
            // TODO: Implement if needed
            break;
        }
        case ttPERMISSIONED_DOMAIN_SET:
        case ttPERMISSIONED_DOMAIN_DELETE: {
            // TODO: Implement if needed
            break;
        }
        case ttREMARKS_SET: {
            break;
        }
        // pseudo transactions
        case ttAMENDMENT:
        case ttFEE:
        case ttUNL_MODIFY:
        case ttEMIT_FAILURE:
        case ttUNL_REPORT: {
            break;
        }
        default: {
            UNREACHABLE("Unknown transaction type");
        }
    }

    std::vector<std::pair<AccountID, bool>> ret{tshEntries.size()};
    for (auto& [a, e] : tshEntries)
        ret[e.first] = std::pair<AccountID, bool>{a, e.second};

    return ret;
}

}  // namespace hook

namespace hook_float {

using namespace hook_api;
static int64_t const minMantissa = 1000000000000000ull;
static int64_t const maxMantissa = 9999999999999999ull;
static int32_t const minExponent = -96;
static int32_t const maxExponent = 80;
inline int32_t
get_exponent(int64_t float1)
{
    if (float1 < 0)
        return INVALID_FLOAT;
    if (float1 == 0)
        return 0;
    uint64_t float_in = (uint64_t)float1;
    float_in >>= 54U;
    float_in &= 0xFFU;
    return ((int32_t)float_in) - 97;
}

inline int64_t
get_mantissa(int64_t float1)
{
    if (float1 < 0)
        return INVALID_FLOAT;
    if (float1 == 0)
        return 0;
    float1 -= ((((uint64_t)float1) >> 54U) << 54U);
    return float1;
}

inline bool
is_negative(int64_t float1)
{
    return ((float1 >> 62U) & 1ULL) == 0;
}

inline int64_t
invert_sign(int64_t float1)
{
    int64_t r = (int64_t)(((uint64_t)float1) ^ (1ULL << 62U));
    return r;
}

inline int64_t
set_sign(int64_t float1, bool set_negative)
{
    bool neg = is_negative(float1);
    if ((neg && set_negative) || (!neg && !set_negative))
        return float1;

    return invert_sign(float1);
}

inline int64_t
set_mantissa(int64_t float1, uint64_t mantissa)
{
    if (mantissa > maxMantissa)
        return MANTISSA_OVERSIZED;
    if (mantissa < minMantissa)
        return MANTISSA_UNDERSIZED;
    return float1 - get_mantissa(float1) + mantissa;
}

inline int64_t
set_exponent(int64_t float1, int32_t exponent)
{
    if (exponent > maxExponent)
        return EXPONENT_OVERSIZED;
    if (exponent < minExponent)
        return EXPONENT_UNDERSIZED;

    uint64_t exp = (exponent + 97);
    exp <<= 54U;
    float1 &= ~(0xFFLL << 54);
    float1 += (int64_t)exp;
    return float1;
}

inline int64_t
make_float(ripple::IOUAmount& amt)
{
    int64_t man_out = amt.mantissa();
    int64_t float_out = 0;
    bool neg = man_out < 0;
    if (neg)
        man_out *= -1;

    float_out = set_sign(float_out, neg);
    float_out = set_mantissa(float_out, (uint64_t)man_out);
    float_out = set_exponent(float_out, amt.exponent());
    return float_out;
}

inline int64_t
make_float(uint64_t mantissa, int32_t exponent, bool neg)
{
    if (mantissa == 0)
        return 0;
    if (mantissa > maxMantissa)
        return MANTISSA_OVERSIZED;
    if (mantissa < minMantissa)
        return MANTISSA_UNDERSIZED;
    if (exponent > maxExponent)
        return EXPONENT_OVERSIZED;
    if (exponent < minExponent)
        return EXPONENT_UNDERSIZED;
    int64_t out = 0;
    out = set_mantissa(out, mantissa);
    out = set_exponent(out, exponent);
    out = set_sign(out, neg);
    return out;
}

}  // namespace hook_float
using namespace hook_float;
using hook::Bytes;

// JS helpers

// extract a std::string from a JSValue if possible
// 0, nullopt = invalid string
// 0, "" = empty string
// > 0, nullopt = longer than maxlen
// > 0, populated = normal string
inline std::pair<size_t, std::optional<std::string>>
FromJSString(JSContext* ctx, JSValueConst& v, int max_len)
{
    std::optional<std::string> out;
    size_t len{0};
    if (!JS_IsString(v))
        return {len, out};

    const char* cstr = JS_ToCStringLen(ctx, &len, v);
    if (len > max_len)
        return {len, out};

    out = std::string(cstr, len);
    JS_FreeCString(ctx, cstr);
    return {len, out};
}

inline std::optional<int64_t>
FromJSInt(JSContext* ctx, JSValueConst& v)
{
    if (JS_IsNumber(v))
    {
        int64_t out = 0;
        JS_ToInt64(ctx, &out, v);
        return out;
    }

    if (JS_IsBigInt(ctx, v))
    {
        int64_t out = 0;
        JS_ToBigInt64(ctx, &out, v);
        return out;
    }

    // if the value is a string, then try to parse it,
    // mindful and tolerant of unary minus, whitespace, commas, underscores,
    // alternative case, decimal point and bignum notation
    if (JS_IsString(v))
    {
        auto [len, s] = FromJSString(ctx, v, 20);

        if (!s.has_value() || len <= 0)
            return {};

        char buffer[21] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

        char* out = buffer;

        const char* start = s->data();
        const char* ptr = start;
        const char* end = ptr + len;

        // Strip leading whitespace
        while (ptr < end &&
               (*ptr == ' ' || *ptr == '\t' || *ptr == '\n' || *ptr == '\r'))
            ++ptr;

        if (ptr == end)
            return {};

        // Check for unary minus
        bool isNegative = (*ptr == '-');
        if (isNegative)
            *out++ = *ptr++;

        // Check for hex prefix
        bool isHex =
            (ptr + 1 < end && ptr[0] == '0' &&
             (ptr[1] == 'x' || ptr[1] == 'X'));
        if (isHex)
        {
            *out++ = *ptr++;
            *out++ = *ptr++;
        }

        bool hasDecimalPoint = false;

        // Character validation loop
        while (ptr < end)
        {
            char c = *ptr;
            if (c >= '0' && c <= '9')
                *out++ = *ptr++;
            else if (
                isHex && ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
                *out++ = *ptr++;
            else if (c == '_' || c == ',')
            {
                if (ptr != start && *(ptr - 1) == '_' || *(ptr - 1) == ',')
                    return {};
                ++ptr;
            }
            else if (c == 'n' && ptr + 1 == end)  // Bignum suffix
                ++ptr;
            else if (c == '.')
            {
                if (!hasDecimalPoint && !isHex)
                {
                    hasDecimalPoint = true;
                    ++ptr;
                    break;  // we will discard / truncate the decimal
                }

                return {};
            }
            else
                break;
        }

        // truncate decimal if any
        for (; hasDecimalPoint && ptr < end && *ptr >= '0' && *ptr <= '9';
             ++ptr)
            ;

        // Strip trailing whitespace if any
        while (ptr < end &&
               (*ptr == ' ' || *ptr == '\t' || *ptr == '\n' || *ptr == '\r'))
            ++ptr;

        // if the value wasn't fully parsed it's not a valid number
        if (ptr != end)
            return {};

        // Parse the buffer into an int64_t
        int64_t result;
        if (sscanf(buffer, isHex ? "%llx" : "%lld", &result) != 1)
            return {};

        // Return the parsed value
        return result;
    }

    return {};
}

#define returnJS(X) return JS_NewInt64(ctx, X)
#define returnJSBigInt(X) return JS_NewBigInt64(ctx, X)
#define returnJSXFL(X) return JS_NewBigInt64(ctx, X)

template <typename T>
inline std::optional<JSValue>
ToJSIntArray(JSContext* ctx, T const& vec)
{
    if (vec.size() > 65535)
        return {};

    JSValue out = JS_NewArray(ctx);
    if (JS_IsException(out))
        return {};

    int i = 0;
    for (auto& x : vec)
        JS_DefinePropertyValueUint32(
            ctx, out, i++, JS_NewInt32(ctx, x), JS_PROP_C_W_E);

    return out;
}

inline JSValue
ToJSHash(JSContext* ctx, uint256 const hash_in)
{
    std::vector<uint8_t> hash{hash_in.data(), hash_in.data() + 32};
    auto out = ToJSIntArray(ctx, hash);
    if (!out.has_value())
        returnJS(INTERNAL_ERROR);
    return *out;
};

inline int64_t
GetLengthOfAlreadyValidatedJSIntArrayOrHexString(
    JSContext* ctx,
    JSValueConst& v)
{
    int64_t len = 0;
    js_get_length64(ctx, &len, v);
    if (JS_IsArray(ctx, v))
        return len;
    return (len / 2);
}

inline std::optional<std::vector<uint8_t>>
FromJSIntArrayOrHexString(JSContext* ctx, JSValueConst& v, int max_len)
{
    std::vector<uint8_t> out;
    out.reserve(max_len);

    auto const a = JS_IsArray(ctx, v);
    auto const s = JS_IsString(v);

    // std::cout << "FromJSIAOHS: a=" << a << ", s=" << s << "\n";

    if (JS_IsArray(ctx, v) > 0)
    {
        int64_t n = 0;
        js_get_length64(ctx, &n, v);

        if (n == 0)
            return out;

        if (n > max_len)
            return {};

        for (int64_t i = 0; i < n; ++i)
        {
            JSValue x = JS_GetPropertyInt64(ctx, v, i);
            if (!JS_IsNumber(x))
                return {};

            int64_t byte = 0;
            JS_ToInt64(ctx, &byte, x);
            if (byte > 256 || byte < 0)
                return {};

            out.push_back((uint8_t)byte);
        }

        return out;
    }

    if (JS_IsString(v))
    {
        auto [len, str] = FromJSString(ctx, v, max_len << 1U);

        // std::cout << "Debug FromJSIAOHS: len=" << len << ", str=";

        // if (str)
        //     std::cout << "`" << *str << "`\n";
        // else
        //     std::cout << "<no string>\n";

        if (!str)
            return {};

        if (len <= 0)
            return out;

        if (len > (max_len << 1U))
            return {};

        if (len != str->size())
            return {};

        auto const parseHexNibble = [](uint8_t a) -> std::optional<uint8_t> {
            if (a >= '0' && a <= '9')
                return a - '0';

            if (a >= 'A' && a <= 'F')
                return a - 'A' + 10;

            if (a >= 'a' && a <= 'f')
                return a - 'a' + 10;

            return {};
        };

        uint8_t const* cstr = reinterpret_cast<uint8_t const*>(str->c_str());

        int i = 0;
        if (len % 2 == 1)
        {
            auto first = parseHexNibble(*cstr++);
            if (!first.has_value())
                return {};

            out[i++] = *first;
        }

        for (; i < len && *cstr != '\0'; ++i)
        {
            auto a = parseHexNibble(*cstr++);
            auto b = parseHexNibble(*cstr++);

            if (!a.has_value() || !b.has_value())
                return {};

            out.push_back((*a << 4U) | (*b));
        }

        return out;
    }

    return {};
}

inline int32_t
no_free_slots(hook::HookContext& hookCtx)
{
    return hook_api::max_slots - hookCtx.slot.size() <= 0;
}

inline std::optional<int32_t>
get_free_slot(hook::HookContext& hookCtx)
{
    // allocate a slot
    int32_t slot_into = 0;
    if (hookCtx.slot_free.size() > 0)
    {
        slot_into = hookCtx.slot_free.front();
        hookCtx.slot_free.pop();
        return slot_into;
    }

    // no slots were available in the queue so increment slot counter until we
    // find a free slot usually this will be the next available but the hook
    // developer may have allocated any slot ahead of when the counter gets
    // there
    do
    {
        slot_into = ++hookCtx.slot_counter;
    } while (hookCtx.slot.find(slot_into) != hookCtx.slot.end() &&
             // this condition should always be met, if for some reason, somehow
             // it is not then we will return the final slot every time.
             hookCtx.slot_counter <= hook_api::max_slots);

    if (hookCtx.slot_counter > hook_api::max_slots)
        return {};

    return slot_into;
}

// cu_ptr is a pointer into memory, bounds check is assumed to have already
// happened
inline std::optional<Currency>
parseCurrency(uint8_t* cu_ptr, uint32_t cu_len)
{
    if (cu_len == 20)
    {
        // normal 20 byte currency
        return Currency::fromVoid(cu_ptr);
    }
    else if (cu_len == 3)
    {
        // 3 byte ascii currency
        // need to check what data is in these three bytes, to ensure ISO4217
        // compliance
        auto const validateChar = [](uint8_t c) -> bool {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '?' || c == '!' || c == '@' ||
                c == '#' || c == '$' || c == '%' || c == '^' || c == '&' ||
                c == '*' || c == '<' || c == '>' || c == '(' || c == ')' ||
                c == '{' || c == '}' || c == '[' || c == ']' || c == '|';
        };

        if (!validateChar(*((uint8_t*)(cu_ptr + 0U))) ||
            !validateChar(*((uint8_t*)(cu_ptr + 1U))) ||
            !validateChar(*((uint8_t*)(cu_ptr + 2U))))
            return {};

        uint8_t cur_buf[20] = {
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            *((uint8_t*)(cu_ptr + 0U)),
            *((uint8_t*)(cu_ptr + 1U)),
            *((uint8_t*)(cu_ptr + 2U)),
            0,
            0,
            0,
            0,
            0};
        return Currency::fromVoid(cur_buf);
    }
    else
        return {};
}

inline int64_t
serialize_keylet(
    ripple::Keylet& kl,
    uint8_t* memory,
    uint32_t write_ptr,
    uint32_t write_len)
{
    if (write_len < 34)
        return hook_api::TOO_SMALL;

    memory[write_ptr + 0] = (kl.type >> 8) & 0xFFU;
    memory[write_ptr + 1] = (kl.type >> 0) & 0xFFU;

    for (int i = 0; i < 32; ++i)
        memory[write_ptr + 2 + i] = kl.key.data()[i];

    return 34;
}

inline std::vector<uint8_t>
serialize_keylet_vec(ripple::Keylet const& kl)
{
    std::vector<uint8_t> out;
    out.reserve(34);

    out.push_back((kl.type >> 8) & 0xFFU);
    out.push_back((kl.type >> 0) & 0xFFU);

    for (int i = 0; i < 32; ++i)
        out.push_back(kl.key.data()[i]);

    return out;
}

std::optional<ripple::Keylet>
unserialize_keylet(uint8_t const* ptr, uint32_t len)
{
    if (len != 34)
        return {};

    uint16_t ktype = ((uint16_t)ptr[0] << 8) + ((uint16_t)ptr[1]);

    return ripple::Keylet{
        static_cast<LedgerEntryType>(ktype),
        ripple::uint256::fromVoid(ptr + 2)};
}

bool
hook::isEmittedTxn(ripple::STTx const& tx)
{
    return tx.isFieldPresent(ripple::sfEmitDetails);
}

int64_t
hook::computeExecutionFee(uint64_t instructionCount)
{
    int64_t fee = (int64_t)instructionCount;
    if (fee < instructionCount)
        return 0x7FFFFFFFFFFFFFFFLL;

    return fee;
}

int64_t
hook::computeCreationFee(uint64_t byteCount)
{
    int64_t fee = ((int64_t)byteCount) * 500ULL;
    if (fee < byteCount)
        return 0x7FFFFFFFFFFFFFFFLL;

    return fee;
}

// many datatypes can be encoded into an int64_t
inline int64_t
data_as_int64(void const* ptr_raw, uint32_t len)
{
    if (len > 8)
        return hook_api::hook_return_code::TOO_BIG;

    uint8_t const* ptr = reinterpret_cast<uint8_t const*>(ptr_raw);
    uint64_t output = 0;
    for (int i = 0, j = (len - 1) * 8; i < len; ++i, j -= 8)
        output += (((uint64_t)ptr[i]) << j);
    if ((1ULL << 63U) & output)
        return hook_api::hook_return_code::TOO_BIG;
    return (int64_t)output;
}

/* returns true iff every even char is ascii and every odd char is 00
 * only a hueristic, may be inaccurate in edgecases */
inline bool
is_UTF16LE(const uint8_t* buffer, size_t len)
{
    if (len % 2 != 0 || len == 0)
        return false;

    for (int i = 0; i < len; i += 2)
        if (buffer[i + 0] == 0 || buffer[i + 1] != 0)
            return false;

    return true;
}

// return true if sleAccount has been modified as a result of the call
bool
hook::addHookNamespaceEntry(ripple::SLE& sleAccount, ripple::uint256 ns)
{
    STVector256 vec = sleAccount.getFieldV256(sfHookNamespaces);
    for (auto u : vec.value())
        if (u == ns)
            return false;

    vec.push_back(ns);
    sleAccount.setFieldV256(sfHookNamespaces, vec);
    return true;
}

// return true if sleAccount has been modified as a result of the call
bool
hook::removeHookNamespaceEntry(ripple::SLE& sleAccount, ripple::uint256 ns)
{
    if (sleAccount.isFieldPresent(sfHookNamespaces))
    {
        STVector256 const& vec = sleAccount.getFieldV256(sfHookNamespaces);
        if (vec.size() == 0)
        {
            // clean up structure if it's present but empty
            sleAccount.makeFieldAbsent(sfHookNamespaces);
            return true;
        }
        else
        {
            // defensively ensure the uniqueness of the namespace array
            std::set<uint256> spaces;

            for (auto u : vec.value())
                if (u != ns)
                    spaces.emplace(u);

            // drop through if it wasn't present (see comment block 20 lines
            // above)
            if (spaces.size() != vec.size())
            {
                if (spaces.size() == 0)
                    sleAccount.makeFieldAbsent(sfHookNamespaces);
                else
                {
                    std::vector<uint256> nv;
                    nv.reserve(spaces.size());

                    for (auto u : spaces)
                        nv.push_back(u);

                    sleAccount.setFieldV256(
                        sfHookNamespaces, STVector256{std::move(nv)});
                }
                return true;
            }
        }
    }
    return false;
}

// Called by Transactor.cpp to determine if a transaction type can trigger a
// given hook... The HookOn field in the SetHook transaction determines which
// transaction types (tt's) trigger the hook. Every bit except ttHookSet is
// active low, so for example ttESCROW_FINISH = 2, so if the 2nd bit (counting
// from 0) from the right is 0 then the hook will trigger on ESCROW_FINISH. If
// it is 1 then ESCROW_FINISH will not trigger the hook. However ttHOOK_SET = 22
// is active high, so by default (HookOn == 0) ttHOOK_SET is not triggered by
// transactions. If you wish to set a hook that has control over ttHOOK_SET then
// set bit 1U<<22.
bool
hook::canHook(ripple::TxType txType, ripple::uint256 hookOn)
{
    // invert ttHOOK_SET bit
    hookOn ^= UINT256_BIT[ttHOOK_SET];

    // invert entire field
    hookOn = ~hookOn;

    return (hookOn & UINT256_BIT[txType]) != beast::zero;
}

bool
hook::canEmit(ripple::TxType txType, ripple::uint256 hookCanEmit)
{
    return hook::canHook(txType, hookCanEmit);
}

ripple::uint256
hook::getHookCanEmit(
    ripple::STObject const& hookObj,
    SLE::pointer const& hookDef)
{
    // default allows all transaction types
    uint256 defaultHookCanEmit = UINT256_BIT[ttHOOK_SET];

    uint256 hookCanEmit =
        (hookObj.isFieldPresent(sfHookCanEmit)
             ? hookObj.getFieldH256(sfHookCanEmit)
             : hookDef->isFieldPresent(sfHookCanEmit)
             ? hookDef->getFieldH256(sfHookCanEmit)
             : defaultHookCanEmit);
    return hookCanEmit;
}

ripple::uint256
hook::getHookOn(
    STObject const& obj,
    std::shared_ptr<SLE const> const& def,
    SField const& field)
{
    if (obj.isFieldPresent(field))
        return obj.getFieldH256(field);
    if (obj.isFieldPresent(sfHookOn))
        return obj.getFieldH256(sfHookOn);
    if (def->isFieldPresent(field))
        return def->getFieldH256(field);
    if (def->isFieldPresent(sfHookOn))
        return def->getFieldH256(sfHookOn);
    return uint256{0};
}

// Update HookState ledger objects for the hook... only called after accept()
// assumes the specified acc has already been checked for authoriation (hook
// grants)
TER
hook::setHookState(
    ripple::ApplyContext& applyCtx,
    ripple::AccountID const& acc,
    ripple::uint256 const& ns,
    ripple::uint256 const& key,
    ripple::Slice const& data)
{
    auto& view = applyCtx.view();
    auto j = applyCtx.app.journal("View");
    auto const sleAccount = view.peek(ripple::keylet::account(acc));

    if (!sleAccount)
        return tefINTERNAL;

    // if the blob is too large don't set it
    uint16_t const hookStateScale = sleAccount->isFieldPresent(sfHookStateScale)
        ? sleAccount->getFieldU16(sfHookStateScale)
        : 1;

    if (data.size() > hook::maxHookStateDataSize(hookStateScale))
        return temHOOK_DATA_TOO_LARGE;

    auto hookStateKeylet = ripple::keylet::hookState(acc, key, ns);
    auto hookStateDirKeylet = ripple::keylet::hookStateDir(acc, ns);

    uint32_t stateCount = sleAccount->getFieldU32(sfHookStateCount);
    uint32_t oldStateCount = stateCount;

    auto hookState = view.peek(hookStateKeylet);

    bool createNew = !hookState;

    // if the blob is nil then delete the entry if it exists
    if (data.empty())
    {
        if (!view.peek(hookStateKeylet))
            return tesSUCCESS;  // a request to remove a non-existent entry is
                                // defined as success

        if (!view.peek(hookStateDirKeylet))
            return tefBAD_LEDGER;

        auto const hint = (*hookState)[sfOwnerNode];
        // Remove the node from the namespace directory
        if (!view.dirRemove(
                hookStateDirKeylet, hint, hookStateKeylet.key, false))
            return tefBAD_LEDGER;

        bool nsDestroyed = !view.peek(hookStateDirKeylet);

        // remove the actual hook state obj
        view.erase(hookState);

        // adjust state object count
        if (stateCount > 0)
            --stateCount;  // guard this because in the "impossible" event it is
                           // already 0 we'll wrap back to int_max
        // if removing this state entry would destroy the allotment then reduce
        // the owner count
        if (stateCount < oldStateCount)
            adjustOwnerCount(view, sleAccount, -hookStateScale, j);

        if (view.rules().enabled(featureExtendedHookState) && stateCount == 0)
            sleAccount->makeFieldAbsent(sfHookStateCount);
        else
            sleAccount->setFieldU32(sfHookStateCount, stateCount);

        if (nsDestroyed)
            hook::removeHookNamespaceEntry(*sleAccount, ns);

        view.update(sleAccount);

        /*
        // if the root page of this namespace was removed then also remove the
        root page
        // from the owner directory
        if (!view.peek(hookStateDirKeylet) && rootHint)
        {
            if (!view.dirRemove(keylet::ownerDir(acc), *rootHint,
        hookStateDirKeylet.key, false)) return tefBAD_LEDGER;
        }
        */

        return tesSUCCESS;
    }

    std::uint32_t ownerCount{(*sleAccount)[sfOwnerCount]};

    if (createNew)
    {
        ++stateCount;

        if (stateCount > oldStateCount)
        {
            // the hook used its allocated allotment of state entries for its
            // previous ownercount increment ownercount and give it another
            // allotment

            ownerCount += hookStateScale;
            XRPAmount const newReserve{view.fees().accountReserve(ownerCount)};

            if (STAmount((*sleAccount)[sfBalance]).xrp() < newReserve)
                return tecINSUFFICIENT_RESERVE;

            adjustOwnerCount(view, sleAccount, hookStateScale, j);
        }

        // update state count
        sleAccount->setFieldU32(sfHookStateCount, stateCount);
        view.update(sleAccount);

        // create an entry
        hookState = std::make_shared<SLE>(hookStateKeylet);
    }

    hookState->setFieldVL(sfHookStateData, data);
    hookState->setFieldH256(sfHookStateKey, key);

    if (createNew)
    {
        bool nsExists = !!view.peek(hookStateDirKeylet);

        auto const page = view.dirInsert(
            hookStateDirKeylet, hookStateKeylet.key, describeOwnerDir(acc));
        if (!page)
            return tecDIR_FULL;

        hookState->setFieldU64(sfOwnerNode, *page);

        // add new data to ledger
        view.insert(hookState);

        // update namespace vector where necessary
        if (!nsExists)
        {
            if (addHookNamespaceEntry(*sleAccount, ns))
                view.update(sleAccount);
        }
    }
    else
    {
        view.update(hookState);
    }

    return tesSUCCESS;
}

hook::HookResult
hook::apply(
    ripple::uint256 const& hookSetTxnID, /* this is the txid of the sethook,
                                            used for caching (one day) */
    ripple::uint256 const&
        hookHash, /* hash of the actual hook byte code, used for metadata */
    uint16_t hookApiVersion,
    ripple::uint256 const& hookCanEmit,
    ripple::uint256 const& hookNamespace,
    ripple::Blob const& bytecode,
    std::map<
        std::vector<uint8_t>, /* param name  */
        std::vector<uint8_t>  /* param value */
        > const& hookParams,
    std::map<
        ripple::uint256, /* hook hash */
        std::map<std::vector<uint8_t>, std::vector<uint8_t>>> const&
        hookParamOverrides,
    HookStateMap& stateMap,
    ApplyContext& applyCtx,
    ripple::AccountID const& account, /* the account the hook is INSTALLED ON
                                         not always the otxn account */
    bool hasCallback,
    bool isCallback,
    bool isStrong,
    uint32_t hookArgument,
    uint8_t hookChainPosition,
    std::shared_ptr<STObject const> const& provisionalMeta,
    uint32_t instructionLimit)
{
    HookContext hookCtx = {
        .applyCtx = applyCtx,
        // we will return this context object (RVO / move constructed)
        .result =
            {.hookSetTxnID = hookSetTxnID,
             .hookHash = hookHash,
             .hookCanEmit = hookCanEmit,
             .accountKeylet = keylet::account(account),
             .hookKeylet = keylet::hook(account),
             .account = account,
             .otxnAccount = applyCtx.tx.getAccountID(sfAccount),
             .hookNamespace = hookNamespace,
             .stateMap = stateMap,
             .changedStateCount = 0,
             .hookParamOverrides = hookParamOverrides,
             .hookParams = hookParams,
             .hookSkips = {},
             .exitType = applyCtx.view().rules().enabled(fixXahauV3)
                 ? hook_api::ExitType::UNSET
                 : hook_api::ExitType::ROLLBACK,  // default is to rollback
                                                  // unless hook calls accept()
             .exitReason = std::string(""),
             .exitCode = -1,
             .hasCallback = hasCallback,
             .isCallback = isCallback,
             .isStrong = isStrong,
             .hookArgument = hookArgument,
             .hookChainPosition = hookChainPosition,
             .foreignStateSetDisabled = false,
             .provisionalMeta = provisionalMeta},
        .emitFailure = isCallback && hookArgument & 1
            ? std::optional<ripple::STObject>(
                  (*(applyCtx.view().peek(keylet::emittedTxn(
                       applyCtx.tx.getFieldH256(sfTransactionHash)))))
                      .downcast<STObject>())
            : std::optional<ripple::STObject>()};

    auto const& j = applyCtx.app.journal("View");

    switch (hookApiVersion)
    {
        case hook_api::CodeType::WASM: {
            HookExecutorWasm executor{hookCtx};

            executor.execute(
                bytecode.data(),
                (size_t)bytecode.size(),
                isCallback,
                hookArgument,
                0 /* instructioin limit not used in wasm */,
                j);

            break;
        }

        case hook_api::CodeType::JS: {
            // RHUPTO: populate hookArgument, bytecode, perform execution
            HookExecutorJS executor{hookCtx};

            executor.execute(
                bytecode.data(),
                (size_t)bytecode.size(),
                isCallback,
                hookArgument,
                instructionLimit,
                j);

            break;
        }

        default: {
            hookCtx.result.exitType = hook_api::ExitType::LEDGER_ERROR;
            return hookCtx.result;
        }
    }

    JLOG(j.trace()) << "HookInfo[" << HC_ACC() << "]: "
                    << "ApiVersion: " << hookApiVersion << " "
                    << (hookCtx.result.exitType == hook_api::ExitType::ROLLBACK
                            ? "ROLLBACK"
                            : "ACCEPT")
                    << " RS: '" << hookCtx.result.exitReason.c_str()
                    << "' RC: " << hookCtx.result.exitCode;
    return hookCtx.result;
}

/* If XRPLD is running with trace log level hooks may produce debugging output
 * to the trace log specifying both a string and an integer to output */
DEFINE_WASM_FUNCTION(
    int64_t,
    trace_num,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t number)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx
                        // on current stack
    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (!j.trace())
        return 0;

    if (read_len > 128)
        read_len = 128;

    if (read_len > 0)
    {
        // skip \0 if present at the end
        if (*((const char*)memory + read_ptr + read_len - 1) == '\0')
            read_len--;

        if (read_len > 0)
        {
            j.trace() << "HookTrace[" << HC_ACC() << "]: "
                      << std::string_view(
                             (const char*)memory + read_ptr, read_len)
                      << ": " << number;

            return 0;
        }
    }

    j.trace() << "HookTrace[" << HC_ACC() << "]: " << number;
    return 0;
    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(int64_t, trace, JSValue msg, JSValue data, JSValue as_hex)
{
    JS_HOOK_SETUP();

    std::string out;
    if (JS_IsString(msg))
    {
        // RH TODO: check if there's a way to ensure the string isn't
        // arbitrarily long before calling ToCStringLen
        size_t len;
        const char* cstr = JS_ToCStringLen(ctx, &len, msg);
        if (len > 256)
            len = 256;
        out = std::string(cstr, len);
        JS_FreeCString(ctx, cstr);
    }

    out += ": ";

    if (JS_IsBool(as_hex) && !!JS_ToBool(ctx, as_hex))
    {
        auto in = FromJSIntArrayOrHexString(ctx, data, 64 * 1024);
        if (in.has_value())
        {
            if (in->size() > 1024)
                in->resize(1024);
            out += strHex(*in);
        }
        else
            out += "<could not display hex>";
    }
    else if (JS_IsBigInt(ctx, data))
    {
        size_t len;
        const char* cstr = JS_ToCStringLen(ctx, &len, data);
        out += std::string(cstr, len);
        JS_FreeCString(ctx, cstr);
    }
    else
    {
        // replacer function that converts BigInts to strings
        JSValueConst replacer = JS_NewCFunction(
            ctx,
            [](JSContext* ctx,
               JSValueConst this_val,
               int argc,
               JSValueConst* argv) -> JSValue {
                if (argc < 2)
                    return JS_DupValue(ctx, argv[1]);
                if (JS_IsBigInt(ctx, argv[1]))
                {
                    size_t len;
                    const char* str = JS_ToCStringLen(ctx, &len, argv[1]);
                    JSValue ret = JS_NewStringLen(ctx, str, len);
                    JS_FreeCString(ctx, str);
                    return ret;
                }
                return JS_DupValue(ctx, argv[1]);
            },
            "replacer",
            2);

        JSValue sdata = JS_JSONStringify(ctx, data, replacer, JS_UNDEFINED);
        JS_FreeValue(ctx, replacer);

        if (JS_IsString(sdata))
        {
            assert(JS_IsString(sdata));
            size_t len;
            const char* cstr = JS_ToCStringLen(ctx, &len, sdata);
            if (len > 1023)
                len = 1023;
            out += std::string(cstr, len);
            JS_FreeCString(ctx, cstr);
            JS_FreeValue(ctx, sdata);
        }
        else
        {
            out += "<could not display data>";
        }
    }

    if (out.size() > 0)
        j.trace() << "HookTrace[" << HC_ACC() << "]: " << out;

    return JS_NewInt64(ctx, 0);
    //    return JS_NewString(ctx, out.c_str());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    trace,
    uint32_t mread_ptr,
    uint32_t mread_len,
    uint32_t dread_ptr,
    uint32_t dread_len,
    uint32_t as_hex)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx
                        // on current stack
    if (NOT_IN_BOUNDS(mread_ptr, mread_len, memory_length) ||
        NOT_IN_BOUNDS(dread_ptr, dread_len, memory_length))
        return OUT_OF_BOUNDS;

    if (!j.trace())
        return 0;

    if (mread_len > 128)
        mread_len = 128;

    if (dread_len > 1023)
        dread_len = 1023;

    uint8_t output_storage[2200];
    size_t out_len = 0;

    uint8_t* output = output_storage;

    if (mread_len > 0)
    {
        memcpy(output, memory + mread_ptr, mread_len);
        out_len += mread_len;

        // detect and skip \0 if it appears at the end
        if (output[out_len - 1] == '\0')
            out_len--;

        output[out_len++] = ':';
        output[out_len++] = ' ';
    }

    output = output_storage + out_len;

    if (dread_len > 0)
    {
        if (as_hex)
        {
            out_len += dread_len * 2;
            for (int i = 0; i < dread_len && i < memory_length; ++i)
            {
                uint8_t high = (memory[dread_ptr + i] >> 4) & 0xFU;
                uint8_t low = (memory[dread_ptr + i] & 0xFU);
                high += (high < 10U ? '0' : 'A' - 10);
                low += (low < 10U ? '0' : 'A' - 10);
                output[i * 2 + 0] = high;
                output[i * 2 + 1] = low;
            }
        }
        else if (is_UTF16LE(memory + dread_ptr, dread_len))
        {
            out_len += dread_len /
                2;  // is_UTF16LE will only return true if read_len is even
            for (int i = 0; i < (dread_len / 2); ++i)
                output[i] = memory[dread_ptr + i * 2];
        }
        else
        {
            out_len += dread_len;
            memcpy(output, memory + dread_ptr, dread_len);
        }
    }

    if (out_len > 0)
    {
        j.trace() << "HookTrace[" << HC_ACC() << "]: "
                  << std::string_view((const char*)output_storage, out_len);
    }

    return 0;
    WASM_HOOK_TEARDOWN();
}

// zero pad on the left a key to bring it up to 32 bytes
std::optional<ripple::uint256> inline make_state_key(std::string_view source)
{
    size_t source_len = source.size();

    if (source_len > 32 || source_len < 1)
        return std::nullopt;

    unsigned char key_buffer[32];
    int i = 0;
    int pad = 32 - source_len;

    // zero pad on the left
    for (; i < pad; ++i)
        key_buffer[i] = 0;

    const char* data = source.data();

    for (; i < 32; ++i)
        key_buffer[i] = data[i - pad];

    return ripple::uint256::fromVoid(key_buffer);
}

DEFINE_WASM_FUNCTION(
    int64_t,
    state_set,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t kread_ptr,
    uint32_t kread_len)
{
    return state_foreign_set(
        hookCtx,
        frameCtx,
        read_ptr,
        read_len,
        kread_ptr,
        kread_len,
        0,
        0,
        0,
        0);
}

DEFINE_JS_FUNCTION(JSValue, state_set, JSValue data, JSValue key)
{
    JS_HOOK_SETUP();

    JSValueConst argv2[] = {argv[0], argv[1], JS_UNDEFINED, JS_UNDEFINED};

    return FORWARD_JS_FUNCTION_CALL(state_foreign_set, 4, argv2);

    JS_HOOK_TEARDOWN();
}

// update or create a hook state object
// read_ptr = data to set, kread_ptr = key
// RH NOTE passing 0 size causes a delete operation which is as-intended
/*
    uint32_t write_ptr, uint32_t write_len,
    uint32_t kread_ptr, uint32_t kread_len,         // key
    uint32_t nread_ptr, uint32_t nread_len,         // namespace
    uint32_t aread_ptr, uint32_t aread_len )        // account
 */
DEFINE_WASM_FUNCTION(
    int64_t,
    state_foreign_set,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t kread_ptr,
    uint32_t kread_len,
    uint32_t nread_ptr,
    uint32_t nread_len,
    uint32_t aread_ptr,
    uint32_t aread_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (read_ptr == 0 && read_len == 0)
    {
        // valid, this is a delete operation
    }
    else if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (kread_len > 32)
        return TOO_BIG;

    if (kread_len < 1)
        return TOO_SMALL;

    if (nread_len != 0 && nread_len != 32)
        return INVALID_ARGUMENT;

    if (aread_len != 0 && aread_len != 20)
        return INVALID_ARGUMENT;

    if (NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length))
        return OUT_OF_BOUNDS;

    // ns can be null if and only if this is a local set
    if (nread_ptr == 0 && nread_len == 0 && !(aread_ptr == 0 && aread_len == 0))
        return INVALID_ARGUMENT;

    if ((nread_len && NOT_IN_BOUNDS(nread_ptr, nread_len, memory_length)) ||
        (kread_len && NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length)) ||
        (aread_len && NOT_IN_BOUNDS(aread_ptr, aread_len, memory_length)))
        return OUT_OF_BOUNDS;

    auto const sleAccount = view.peek(hookCtx.result.accountKeylet);
    if (!sleAccount && view.rules().enabled(featureExtendedHookState))
        return tefINTERNAL;

    uint16_t const hookStateScale = sleAccount->isFieldPresent(sfHookStateScale)
        ? sleAccount->getFieldU16(sfHookStateScale)
        : 1;

    uint32_t maxSize = hook::maxHookStateDataSize(hookStateScale);
    if (read_len > maxSize)
        return TOO_BIG;

    uint256 ns = nread_len == 0
        ? hookCtx.result.hookNamespace
        : ripple::base_uint<256>::fromVoid(memory + nread_ptr);

    ripple::AccountID acc = aread_len == 20
        ? AccountID::fromVoid(memory + aread_ptr)
        : hookCtx.result.account;

    auto const key = make_state_key(
        std::string_view{(const char*)(memory + kread_ptr), (size_t)kread_len});

    if (view.rules().enabled(fixXahauV1))
    {
        auto const sleAccount = view.peek(hookCtx.result.accountKeylet);
        if (!sleAccount)
            return tefINTERNAL;
    }

    if (!key)
        return INTERNAL_ERROR;

    ripple::Blob data{memory + read_ptr, memory + read_ptr + read_len};

    auto const result = api.state_foreign_set(*key, ns, acc, data);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(
    JSValue,
    state_foreign_set,
    JSValue raw_val,
    JSValue raw_key,
    JSValue raw_ns,
    JSValue raw_acc)
{
    JS_HOOK_SETUP();

    auto val = FromJSIntArrayOrHexString(ctx, raw_val, 0x10000);
    auto key_in = FromJSIntArrayOrHexString(ctx, raw_key, 0x10000);
    auto ns_in = FromJSIntArrayOrHexString(ctx, raw_ns, 0x10000);
    auto acc_in = FromJSIntArrayOrHexString(ctx, raw_acc, 0x10000);

    if (!val.has_value() && !JS_IsUndefined(raw_val))
        returnJS(INVALID_ARGUMENT);

    if (!ns_in.has_value() && !JS_IsUndefined(raw_ns))
        returnJS(INVALID_ARGUMENT);

    if (!acc_in.has_value() && !JS_IsUndefined(raw_acc))
        returnJS(INVALID_ARGUMENT);

    // val may be populated and empty, this is a delete operation...

    if (val.has_value())
    {
        auto const sleAccount = view.peek(hookCtx.result.accountKeylet);
        if (!sleAccount && view.rules().enabled(featureExtendedHookState))
            returnJS(tefINTERNAL);
        uint16_t const hookStateScale =
            sleAccount->isFieldPresent(sfHookStateScale)
            ? sleAccount->getFieldU16(sfHookStateScale)
            : 1;
        if (val->size() > hook::maxHookStateDataSize(hookStateScale))
            returnJS(TOO_BIG);
    }

    if (key_in.has_value())
    {
        if (key_in->size() > 32)
            returnJS(TOO_BIG);

        if (key_in->size() < 1)
            // FromJSIntArrayOrHexString() does not return data of length 0.
            returnJS(TOO_SMALL);
    }
    else
    {
        returnJS(INVALID_ARGUMENT);
    }

    if (ns_in.has_value() && ns_in->size() != 32)
        returnJS(INVALID_ARGUMENT);

    if (acc_in.has_value() && acc_in->size() != 20)
        returnJS(INVALID_ARGUMENT);

    uint256 ns = ns_in.has_value() ? uint256::fromVoid(ns_in->data())
                                   : hookCtx.result.hookNamespace;

    AccountID acc = acc_in.has_value() ? AccountID::fromVoid(acc_in->data())
                                       : hookCtx.result.account;

    auto key = make_state_key(
        std::string_view{(const char*)(key_in->data()), key_in->size()});

    auto const sleAccount = view.peek(hookCtx.result.accountKeylet);
    if (!sleAccount)
        returnJS(tefINTERNAL);

    if (!key)
        returnJS(INTERNAL_ERROR);

    ripple::Blob data;
    if (val.has_value())
        data = ripple::Blob(val->data(), val->data() + val->size());

    auto const result = api.state_foreign_set(*key, ns, acc, data);
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

ripple::TER
hook::finalizeHookState(
    HookStateMap const& stateMap,
    ripple::ApplyContext& applyCtx,
    ripple::uint256 const& txnID)
{
    auto const& j = applyCtx.app.journal("View");
    uint16_t changeCount = 0;

    // write all changes to state, if in "apply" mode
    for (const auto& accEntry : stateMap)
    {
        const auto& acc = accEntry.first;
        for (const auto& nsEntry : std::get<3>(accEntry.second))
        {
            const auto& ns = nsEntry.first;
            for (const auto& cacheEntry : nsEntry.second)
            {
                bool is_modified = cacheEntry.second.first;
                const auto& key = cacheEntry.first;
                const auto& blob = cacheEntry.second.second;
                if (is_modified)
                {
                    changeCount++;
                    if (changeCount > max_state_modifications + 1)
                    {
                        // overflow
                        JLOG(j.warn())
                            << "HooKError[TX:" << txnID
                            << "]: SetHooKState failed: Too many state changes";
                        return tecHOOK_REJECTED;
                    }

                    // this entry isn't just cached, it was actually modified
                    auto slice = Slice(blob.data(), blob.size());

                    TER result = setHookState(applyCtx, acc, ns, key, slice);

                    if (!isTesSuccess(result))
                    {
                        JLOG(j.warn())
                            << "HookError[TX:" << txnID
                            << "]: SetHookState failed: " << result
                            << " Key: " << key << " Value: " << slice;
                        return result;
                    }
                    // ^ should not fail... checks were done before map insert
                }
            }
        }
    }
    return tesSUCCESS;
}

bool /* retval of true means an error */
hook::gatherHookParameters(
    std::shared_ptr<ripple::STLedgerEntry> const& hookDef,
    ripple::STObject const& hookObj,
    std::map<std::vector<uint8_t>, std::vector<uint8_t>>& parameters,
    beast::Journal const& j_)
{
    if (!hookDef->isFieldPresent(sfHookParameters))
    {
        JLOG(j_.fatal())
            << "HookError[]: Failure: hook def missing parameters (send)";
        return true;
    }

    // first defaults
    auto const& defaultParameters = hookDef->getFieldArray(sfHookParameters);
    for (auto const& hookParameterObj : defaultParameters)
    {
        parameters[hookParameterObj.getFieldVL(sfHookParameterName)] =
            hookParameterObj.getFieldVL(sfHookParameterValue);
    }

    // and then custom
    if (hookObj.isFieldPresent(sfHookParameters))
    {
        auto const& hookParameters = hookObj.getFieldArray(sfHookParameters);
        for (auto const& hookParameterObj : hookParameters)
        {
            parameters[hookParameterObj.getFieldVL(sfHookParameterName)] =
                hookParameterObj.getFieldVL(sfHookParameterValue);
        }
    }
    return false;
}

ripple::TER
hook::removeEmissionEntry(ripple::ApplyContext& applyCtx)
{
    auto const& j = applyCtx.app.journal("View");

    auto const& tx = applyCtx.tx;
    if (!const_cast<ripple::STTx&>(tx).isFieldPresent(sfEmitDetails))
        return tesSUCCESS;

    auto key = keylet::emittedTxn(tx.getTransactionID());

    auto const& sle = applyCtx.view().peek(key);

    if (!sle)
        return tesSUCCESS;

    if (!applyCtx.view().dirRemove(
            keylet::emittedDir(), sle->getFieldU64(sfOwnerNode), key, false))
    {
        JLOG(j.fatal()) << "HookError[TX:" << tx.getTransactionID()
                        << "]: removeEmissionEntry failed tefBAD_LEDGER";
        return tefBAD_LEDGER;
    }

    applyCtx.view().erase(sle);
    return tesSUCCESS;
}

TER
hook::finalizeHookResult(
    hook::HookResult& hookResult,
    ripple::ApplyContext& applyCtx,
    bool doEmit)
{
    auto const& j = applyCtx.app.journal("View");

    // open views do not modify add/remove ledger entries
    if (applyCtx.view().open() && !(applyCtx.flags() & tapDRY_RUN))
        return tesSUCCESS;

    // RH TODO: this seems hacky... and also maybe there's a way this cast might
    // fail?
    ApplyViewImpl& avi = dynamic_cast<ApplyViewImpl&>(applyCtx.view());

    uint16_t exec_index = avi.nextHookExecutionIndex();
    // apply emitted transactions to the ledger (by adding them to the emitted
    // directory) if we are allowed to
    std::vector<std::pair<uint256 /* txnid */, uint256 /* emit nonce */>>
        emission_txnid;

    if (doEmit)
    {
        DBG_PRINTF("emitted txn count: %d\n", hookResult.emittedTxn.size());
        for (; hookResult.emittedTxn.size() > 0; hookResult.emittedTxn.pop())
        {
            auto& tpTrans = hookResult.emittedTxn.front();
            auto& id = tpTrans->getID();
            JLOG(j.trace()) << "HookEmit[" << HR_ACC() << "]: " << id;

            applyCtx.app.getHashRouter().setFlags(id, SF_EMITTED);

            std::shared_ptr<const ripple::STTx> ptr =
                tpTrans->getSTransaction();

            auto emittedId = keylet::emittedTxn(id);
            auto sleEmitted = applyCtx.view().peek(emittedId);

            if (!sleEmitted)
            {
                auto const& emitDetails = const_cast<ripple::STTx&>(*ptr)
                                              .getField(sfEmitDetails)
                                              .downcast<STObject>();

                emission_txnid.emplace_back(
                    id, emitDetails.getFieldH256(sfEmitNonce));
                sleEmitted = std::make_shared<SLE>(emittedId);

                // RH TODO: add a new constructor to STObject to avoid this
                // serder thing
                ripple::Serializer s;
                ptr->add(s);
                SerialIter sit(s.slice());

                sleEmitted->emplace_back(ripple::STObject(sit, sfEmittedTxn));
                auto page = applyCtx.view().dirInsert(
                    keylet::emittedDir(), emittedId, [&](SLE::ref sle) {
                        (*sle)[sfFlags] = lsfEmittedDir;
                    });

                if (page)
                {
                    (*sleEmitted)[sfOwnerNode] = *page;
                    applyCtx.view().insert(sleEmitted);
                }
                else
                {
                    JLOG(j.warn())
                        << "HookError[" << HR_ACC() << "]: "
                        << "Emission Directory full when trying to insert "
                        << id;
                    return tecDIR_FULL;
                }
            }
        }
    }

    bool const fixV2 = applyCtx.view().rules().enabled(fixXahauV2);
    // add a metadata entry for this hook execution result
    {
        STObject meta{sfHookExecution};
        meta.setFieldU8(sfHookResult, hookResult.exitType);
        meta.setAccountID(sfHookAccount, hookResult.account);

        // RH NOTE: this is probably not necessary, a direct cast should always
        // put the (negative) 1 bit at the MSB however to ensure this is
        // consistent across different arch/compilers it's done explicitly here.
        uint64_t unsigned_exit_code =
            (hookResult.exitCode >= 0
                 ? hookResult.exitCode
                 : 0x8000000000000000ULL + (-1 * hookResult.exitCode));

        meta.setFieldU64(sfHookReturnCode, unsigned_exit_code);
        meta.setFieldVL(
            sfHookReturnString,
            ripple::Slice{
                hookResult.exitReason.data(), hookResult.exitReason.size()});
        meta.setFieldU64(sfHookInstructionCount, hookResult.instructionCount);
        meta.setFieldU16(
            sfHookEmitCount,
            emission_txnid.size());  // this will never wrap, hard limit
        meta.setFieldU16(sfHookExecutionIndex, exec_index);
        meta.setFieldU16(sfHookStateChangeCount, hookResult.changedStateCount);
        meta.setFieldH256(sfHookHash, hookResult.hookHash);

        // add informational flags in fix2
        if (fixV2)
        {
            uint32_t flags = 0;
            if (hookResult.isStrong)
                flags |= hefSTRONG;
            if (hookResult.isCallback)
                flags |= hefCALLBACK;
            if (hookResult.executeAgainAsWeak)
                flags |= hefDOAAW;
            meta.setFieldU32(sfFlags, flags);
        }
        avi.addHookExecutionMetaData(std::move(meta));
    }

    // if any txns were emitted then add them to the HookEmissions
    if (applyCtx.view().rules().enabled(featureHooksUpdate1) &&
        !emission_txnid.empty())
    {
        for (auto const& [etxnid, enonce] : emission_txnid)
        {
            STObject meta{sfHookEmission};
            meta.setFieldH256(sfHookHash, hookResult.hookHash);
            meta.setAccountID(sfHookAccount, hookResult.account);
            meta.setFieldH256(sfEmittedTxnID, etxnid);
            if (fixV2)
                meta.setFieldH256(sfEmitNonce, enonce);
            avi.addHookEmissionMetaData(std::move(meta));
        }
    }

    return tesSUCCESS;
}

/* Retrieve the state into write_ptr identified by the key in kread_ptr */
DEFINE_WASM_FUNCTION(
    int64_t,
    state,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t kread_ptr,
    uint32_t kread_len)
{
    return state_foreign(
        hookCtx,
        frameCtx,
        write_ptr,
        write_len,
        kread_ptr,
        kread_len,
        0,
        0,
        0,
        0);
}

/* This api actually serves both local and foreign state requests
 * feeding aread_ptr = 0 and aread_len = 0 will cause it to read local
 * feeding nread_len = 0 will cause hook's native namespace to be used */
DEFINE_WASM_FUNCTION(
    int64_t,
    state_foreign,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t kread_ptr,
    uint32_t kread_len,  // key
    uint32_t nread_ptr,
    uint32_t nread_len,  // namespace
    uint32_t aread_ptr,
    uint32_t aread_len)  // account
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    bool is_foreign = false;
    if (aread_ptr == 0)
    {
        // valid arguments, local state
        if (aread_len != 0)
            return INVALID_ARGUMENT;
    }
    else
    {
        // valid arguments, foreign state
        is_foreign = true;
        if (aread_len != 20)
            return INVALID_ARGUMENT;
    }

    if (kread_len > 32)
        return TOO_BIG;

    if (kread_len < 1)
        return TOO_SMALL;

    if (write_len < 1 && write_ptr != 0)
        return TOO_SMALL;

    if (!is_foreign && nread_len == 0)
    {
        // local account will be populated with local hook namespace unless
        // otherwise specified
    }
    else if (nread_len != 32)
        return INVALID_ARGUMENT;

    if (NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length) ||
        NOT_IN_BOUNDS(nread_ptr, nread_len, memory_length) ||
        NOT_IN_BOUNDS(aread_ptr, aread_len, memory_length) ||
        NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    uint256 ns = nread_len == 0
        ? hookCtx.result.hookNamespace

        : ripple::base_uint<256>::fromVoid(memory + nread_ptr);

    ripple::AccountID acc = is_foreign ? AccountID::fromVoid(memory + aread_ptr)
                                       : hookCtx.result.account;

    auto const key = make_state_key(
        std::string_view{(const char*)(memory + kread_ptr), (size_t)kread_len});

    if (!key)
        return INVALID_ARGUMENT;

    auto const result = api.state_foreign(*key, ns, acc);
    if (!result)
        return result.error();
    auto const& b = result.value();

    WRITE_WASM_MEMORY_OR_RETURN_AS_INT64(
        write_ptr, write_len, b.data(), b.size(), false);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(
    JSValue,
    state_foreign,
    JSValue raw_key,
    JSValue raw_ns,
    JSValue raw_accid)
{
    JS_HOOK_SETUP();

    auto key_in = FromJSIntArrayOrHexString(ctx, raw_key, 32);
    auto ns_in = FromJSIntArrayOrHexString(ctx, raw_ns, 32);
    auto accid_in = FromJSIntArrayOrHexString(ctx, raw_accid, 20);

    if (!key_in.has_value() || key_in->empty())
        returnJS(INVALID_ARGUMENT);

    // RH TODO: enhance this check to only allow undefined or false or array or
    // hexstring

    if (ns_in.has_value() && ns_in->size() != 32)
        returnJS(INVALID_ARGUMENT);

    if (accid_in.has_value() && accid_in->size() != 20)
        returnJS(INVALID_ARGUMENT);

    uint256 ns = ns_in.has_value() ? uint256::fromVoid(ns_in->data())
                                   : hookCtx.result.hookNamespace;

    AccountID acc = accid_in.has_value() ? AccountID::fromVoid(accid_in->data())
                                         : hookCtx.result.account;

    auto key = make_state_key(
        std::string_view{(const char*)(key_in->data()), key_in->size()});

    if (!key.has_value())
        returnJS(INVALID_ARGUMENT);

    auto const result = api.state_foreign(*key, ns, acc);
    if (!result)
        returnJS(result.error());

    auto out = ToJSIntArray(ctx, result.value());

    if (!out)
        returnJS(INTERNAL_ERROR);

    return *out;

    JS_HOOK_TEARDOWN();
}

/* Retrieve the state into write_ptr identified by the key in kread_ptr */
DEFINE_JS_FUNCTION(JSValue, state, JSValue key)
{
    JS_HOOK_SETUP();

    JSValueConst argv2[] = {argv[0], JS_UNDEFINED, JS_UNDEFINED};

    return FORWARD_JS_FUNCTION_CALL(state_foreign, 3, argv2);

    JS_HOOK_TEARDOWN();
}

// Cause the originating transaction to go through, save state changes and emit
// emitted tx, exit hook
DEFINE_WASM_FUNCTION(
    int64_t,
    accept,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t error_code)
{
    WASM_HOOK_SETUP();
    HOOK_EXIT(read_ptr, read_len, error_code, hook_api::ExitType::ACCEPT);
    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(int64_t, accept, JSValue error_msg, JSValue error_code)
{
    JS_HOOK_SETUP();
    HOOK_EXIT_JS(error_msg, error_code, hook_api::ExitType::ACCEPT);
    JS_HOOK_TEARDOWN();
}

// Cause the originating transaction to be rejected, discard state changes and
// discard emitted tx, exit hook
DEFINE_WASM_FUNCTION(
    int64_t,
    rollback,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t error_code)
{
    WASM_HOOK_SETUP();
    HOOK_EXIT(read_ptr, read_len, error_code, hook_api::ExitType::ROLLBACK);
    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(int64_t, rollback, JSValue error_msg, JSValue error_code)
{
    JS_HOOK_SETUP();
    HOOK_EXIT_JS(error_msg, error_code, hook_api::ExitType::ROLLBACK);
    JS_HOOK_TEARDOWN();
}

// Write the TxnID of the originating transaction into the write_ptr
DEFINE_WASM_FUNCTION(
    int64_t,
    otxn_id,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t flags)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    auto const result = api.otxn_id(flags);
    if (!result)
        return result.error();

    auto const& txID = result.value();

    if (txID.size() > write_len)
        return TOO_SMALL;

    if (NOT_IN_BOUNDS(write_ptr, txID.size(), memory_length) ||
        NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr,
        txID.size(),
        txID.data(),
        txID.size(),
        memory,
        memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, otxn_id, JSValue flags_in)
{
    JS_HOOK_SETUP();

    int64_t flags = 0;

    if (JS_IsNumber(flags_in))
        JS_ToInt64(ctx, &flags, flags_in);

    auto const result = api.otxn_id(flags);
    if (!result)
        returnJS(result.error());

    auto const& txID = result.value();

    auto out = ToJSIntArray(ctx, Slice{txID.data(), txID.size()});

    if (!out.has_value())
        returnJS(INTERNAL_ERROR);

    return *out;

    JS_HOOK_TEARDOWN();
}

// Return the tt (Transaction Type) numeric code of the originating transaction
DEFINE_WASM_FUNCTION(int64_t, otxn_type)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    return api.otxn_type();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, otxn_type)
{
    JS_HOOK_SETUP();

    returnJS(api.otxn_type());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, otxn_slot, uint32_t slot_into)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    auto const result = api.otxn_slot(slot_into);
    if (!result)
        return result.error();

    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, otxn_slot, JSValue slot_into)
{
    JS_HOOK_SETUP();

    auto si = FromJSInt(ctx, slot_into);
    if (!si.has_value() || *si > 0xFFFFFFFFULL)
        returnJS(INVALID_ARGUMENT);

    auto const result = api.otxn_slot(*si);
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

// Return the burden of the originating transaction... this will be 1 unless the
// originating transaction was itself an emitted transaction from a previous
// hook invocation
DEFINE_WASM_FUNCTION(int64_t, otxn_burden)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    return api.otxn_burden();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, otxn_burden)
{
    JS_HOOK_SETUP();

    returnJS(api.otxn_burden());

    JS_HOOK_TEARDOWN();
}

// Return the generation of the originating transaction... this will be 1 unless
// the originating transaction was itself an emitted transaction from a previous
// hook invocation
DEFINE_WASM_FUNCTION(int64_t, otxn_generation)
{
    WASM_HOOK_SETUP();
    return api.otxn_generation();
    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, otxn_generation)
{
    JS_HOOK_SETUP();

    returnJS(api.otxn_generation());

    JS_HOOK_TEARDOWN();
}

// Return the generation of a hypothetically emitted transaction from this hook
DEFINE_WASM_FUNCTION(int64_t, etxn_generation)
{
    // proxy only, no setup or teardown
    return hookCtx.api().etxn_generation();
}

DEFINE_JS_FUNCTION(JSValue, etxn_generation)
{
    JS_HOOK_SETUP();

    returnJS(api.etxn_generation());

    JS_HOOK_TEARDOWN();
}

// Return the current ledger sequence number
DEFINE_WASM_FUNCTION(int64_t, ledger_seq)
{
    WASM_HOOK_SETUP();

    return api.ledger_seq();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, ledger_seq)
{
    JS_HOOK_SETUP();

    returnJS(api.ledger_seq());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    ledger_last_hash,
    uint32_t write_ptr,
    uint32_t write_len)
{
    WASM_HOOK_SETUP();

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;
    if (write_len < 32)
        return TOO_SMALL;

    auto const hash = api.ledger_last_hash();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, hash.data(), 32, memory, memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, ledger_last_hash)
{
    JS_HOOK_SETUP();

    return ToJSHash(ctx, api.ledger_last_hash());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, ledger_last_time)
{
    WASM_HOOK_SETUP();

    return api.ledger_last_time();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, ledger_last_time)
{
    JS_HOOK_SETUP();

    returnJS(api.ledger_last_time());

    JS_HOOK_TEARDOWN();
}

// Dump a field from the originating transaction into the hook's memory
DEFINE_WASM_FUNCTION(
    int64_t,
    otxn_field,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t field_id)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (write_ptr == 0)
    {
        if (write_len != 0)
            return INVALID_ARGUMENT;

        // otherwise pass, we're trying to return the data as an int64_t
    }
    else if NOT_IN_BOUNDS (write_ptr, write_len, memory_length)
        return OUT_OF_BOUNDS;

    auto const result = api.otxn_field(field_id);
    if (!result)
        return result.error();

    auto const& field = result.value();

    Serializer s;
    field->add(s);

    WRITE_WASM_MEMORY_OR_RETURN_AS_INT64(
        write_ptr,
        write_len,
        s.getDataPtr(),
        s.getDataLength(),
        field->getSType() == STI_ACCOUNT);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, otxn_field, JSValue raw_field_id)
{
    JS_HOOK_SETUP();

    auto field_id = FromJSInt(ctx, raw_field_id);
    if (!field_id.has_value() || *field_id > 0xFFFFFFFFULL)
        returnJS(INVALID_ARGUMENT);

    auto const result = api.otxn_field(*field_id);
    if (!result)
        returnJS(result.error());
    auto const& field = result.value();

    Serializer s;
    field->add(s);

    uint8_t const* ptr = reinterpret_cast<uint8_t const*>(s.getDataPtr());
    size_t len = s.getDataLength();

    if (field->getSType() == STI_ACCOUNT && len > 1)
    {
        ptr++;
        len--;
    }

    auto out = ToJSIntArray(ctx, Slice{ptr, (size_t)len});

    if (!out.has_value())
        returnJS(INTERNAL_ERROR);

    return *out;

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    slot,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t slot_no)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (write_ptr == 0)
    {
        // in this mode the function returns the data encoded in an int64_t
        if (write_len != 0)
            return INVALID_ARGUMENT;
    }
    else
    {
        if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
            return OUT_OF_BOUNDS;

        if (write_len < 1)
            return TOO_SMALL;
    }

    auto const result = api.slot(slot_no);
    if (!result)
        return result.error();

    Serializer s;
    (*result)->add(s);

    WRITE_WASM_MEMORY_OR_RETURN_AS_INT64(
        write_ptr,
        write_len,
        s.getDataPtr(),
        s.getDataLength(),
        (*result)->getSType() == STI_ACCOUNT);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, slot, JSValue raw_slot_no)
{
    JS_HOOK_SETUP();

    if (argc == 2 && !JS_IsBool(argv[1]))
        returnJS(INVALID_ARGUMENT);

    auto slot_no = FromJSInt(ctx, raw_slot_no);
    if (!slot_no.has_value())
        returnJS(INVALID_ARGUMENT);

    auto const result = api.slot(*slot_no);
    if (!result)
        returnJS(result.error());
    auto const& slot = result.value();

    Serializer s;
    slot->add(s);

    ssize_t len = s.getDataLength();
    ssize_t olen = len;
    uint8_t const* ptr = reinterpret_cast<uint8_t const*>(s.getDataPtr());

    if (hookCtx.slot[*slot_no].entry->getSType() == STI_ACCOUNT)
    {
        --len;
        ++ptr;
    }

    if (len <= 0 || len > olen)
        returnJS(INTERNAL_ERROR);

    auto out = ToJSIntArray(ctx, Slice{ptr, (size_t)len});

    if (!out.has_value())
        returnJS(INTERNAL_ERROR);

    if (argc == 2 && !!JS_ToBool(ctx, argv[1]))
    {
        JS_FreeValue(ctx, *out);
        int64_t data = data_as_int64(ptr, len);
        if (data == TOO_BIG)
            returnJS(data);
        returnJSBigInt(data);
    }

    return *out;

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, slot_clear, uint32_t slot_no)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    auto const result = api.slot_clear(slot_no);
    if (!result)
        return result.error();

    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, slot_clear, JSValue raw_slot_no)
{
    JS_HOOK_SETUP();

    auto slot_no = FromJSInt(ctx, raw_slot_no);
    if (!slot_no.has_value())
        returnJS(INVALID_ARGUMENT);

    auto const result = api.slot_clear(*slot_no);
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, slot_count, uint32_t slot_no)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    auto const result = api.slot_count(slot_no);
    if (!result)
        return result.error();

    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, slot_count, JSValue raw_slot_no)
{
    JS_HOOK_SETUP();

    auto slot_no = FromJSInt(ctx, raw_slot_no);
    if (!slot_no.has_value())
        returnJS(INVALID_ARGUMENT);

    auto const result = api.slot_count(slot_no.value());
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    slot_set,
    uint32_t read_ptr,
    uint32_t read_len,  // readptr is a keylet
    uint32_t slot_into /* providing 0 allocates a slot to you */)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    Bytes data{memory + read_ptr, memory + read_ptr + read_len};
    auto const result = api.slot_set(data, slot_into);
    if (!result)
        return result.error();

    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, slot_set, JSValue raw_key, JSValue raw_slot_into)
{
    JS_HOOK_SETUP();

    auto key = FromJSIntArrayOrHexString(ctx, raw_key, 34);
    auto slot_into = FromJSInt(ctx, raw_slot_into);

    if (!key.has_value() || !slot_into.has_value())
        returnJS(INVALID_ARGUMENT);

    auto const result = api.slot_set(*key, *slot_into);
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, slot_size, uint32_t slot_no)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    auto const result = api.slot_size(slot_no);
    if (!result)
        return result.error();

    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, slot_size, JSValue raw_slot_no)
{
    JS_HOOK_SETUP();

    auto slot_no = FromJSInt(ctx, raw_slot_no);
    if (!slot_no.has_value() || *slot_no < 0 || *slot_no > hook_api::max_slots)
        returnJS(INVALID_ARGUMENT);

    auto const result = api.slot_size(*slot_no);
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    slot_subarray,
    uint32_t parent_slot,
    uint32_t array_id,
    uint32_t new_slot)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    auto const result = api.slot_subarray(parent_slot, array_id, new_slot);
    if (!result)
        return result.error();

    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(
    JSValue,
    slot_subarray,
    JSValue raw_parent_slot,
    JSValue raw_array_id,
    JSValue raw_new_slot)
{
    JS_HOOK_SETUP();

    auto parent_slot = FromJSInt(ctx, raw_parent_slot);
    auto array_id = FromJSInt(ctx, raw_array_id);
    auto new_slot = FromJSInt(ctx, raw_new_slot);

    if (!parent_slot.has_value() || !array_id.has_value() ||
        !new_slot.has_value())
        returnJS(INVALID_ARGUMENT);

    auto const result = api.slot_subarray(*parent_slot, *array_id, *new_slot);
    if (!result)
        returnJS(result.error());

    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    slot_subfield,
    uint32_t parent_slot,
    uint32_t field_id,
    uint32_t new_slot)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    auto const result = api.slot_subfield(parent_slot, field_id, new_slot);
    if (!result)
        return result.error();

    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(
    JSValue,
    slot_subfield,
    JSValue raw_parent_slot,
    JSValue raw_field_id,
    JSValue raw_new_slot)
{
    JS_HOOK_SETUP();

    auto parent_slot = FromJSInt(ctx, raw_parent_slot);
    auto field_id = FromJSInt(ctx, raw_field_id);
    auto new_slot = FromJSInt(ctx, raw_new_slot);

    if (!parent_slot.has_value() || !field_id.has_value() ||
        !new_slot.has_value())
        returnJS(INVALID_ARGUMENT);

    auto const result = api.slot_subfield(*parent_slot, *field_id, *new_slot);
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, slot_type, uint32_t slot_no, uint32_t flags)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    auto const result = api.slot_type(slot_no, flags);
    if (!result)
        return result.error();
    if (flags == 0)
    {
        auto const base = std::get<0>(*result);
        return base.getFName().fieldCode;
    }
    else
    {
        auto const amount = std::get<1>(*result);
        return amount.native();
    }

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, slot_type, JSValue raw_slot_no, JSValue raw_flags)
{
    JS_HOOK_SETUP();

    auto slot_no = FromJSInt(ctx, raw_slot_no);
    auto flags = FromJSInt(ctx, raw_flags);

    if (!slot_no.has_value() || !flags.has_value())
        returnJS(INVALID_ARGUMENT);

    auto const result = api.slot_type(*slot_no, *flags);
    if (!result)
        returnJS(result.error());
    if (flags == 0)
    {
        auto const base = std::get<0>(*result);
        returnJS(base.getFName().fieldCode);
    }
    else
    {
        auto const amount = std::get<1>(*result);
        returnJS(amount.native());
    }

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, slot_float, uint32_t slot_no)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    auto const result = api.slot_float(slot_no);
    if (!result)
        return result.error();

    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, slot_float, JSValue raw_slot_no)
{
    JS_HOOK_SETUP();

    auto slot_no = FromJSInt(ctx, raw_slot_no);

    if (!slot_no.has_value())
        returnJS(INVALID_ARGUMENT);

    auto const result = api.slot_float(*slot_no);
    if (!result)
        returnJS(result.error());
    returnJSXFL(result.value());

    JS_HOOK_TEARDOWN();
}

/*
#define VAR_JSASSIGN(T, V) T& V = argv[_stack++]

#define DEFINE_JS_FUNCTION(R, F, ...)\
JSValue hook_api::JSFunction##F(JSContext *ctx, JSValueConst this_val,\
                        int argc, JSValueConst *argv)\
*/

DEFINE_JS_FUNCTION(JSValue, util_keylet, JSValue kt_raw)
/* use JSValueConst* argv & int argc */
{
    JS_HOOK_SETUP();

    std::optional<int64_t> kt = FromJSInt(ctx, kt_raw);

    if (!kt.has_value() || *kt == 0)
        returnJS(INVALID_ARGUMENT);

    // computed keylet is populated into this for return
    std::optional<JSValue> kl_out;

    try
    {
        switch (*kt)
        {
            case keylet_code::QUALITY: {
                // looking for a 34 byte keylet and high 32 bits and low 32 bits
                // of quality RHTODO: accept optionally a bigint here?
                if (argc != 4)
                    returnJS(INVALID_ARGUMENT);

                auto h = FromJSInt(ctx, argv[2]);
                auto l = FromJSInt(ctx, argv[3]);
                auto kl = FromJSIntArrayOrHexString(ctx, argv[1], 34);

                if (!h.has_value() || !l.has_value() || !kl.has_value() ||
                    kl->size() != 34)
                    returnJS(INVALID_ARGUMENT);

                // ensure it's a dir keylet or we will fail an assertion
                if ((*kl)[0] != 0 || (*kl)[1] != 0x64U)
                    returnJS(INVALID_ARGUMENT);

                std::optional<ripple::Keylet> k =
                    unserialize_keylet(kl->data(), kl->size());

                if (!k)
                    returnJS(NO_SUCH_KEYLET);

                uint64_t arg = (((uint64_t)(*h)) << 32U) + ((uint64_t)(*l));

                kl_out = ToJSIntArray(
                    ctx,
                    serialize_keylet_vec(ripple::keylet::quality(*k, arg)));

                break;
            }

            case keylet_code::HOOK_DEFINITION:
            case keylet_code::CHILD:
            case keylet_code::EMITTED_TXN:
            case keylet_code::UNCHECKED: {
                if (argc != 2)
                    returnJS(INVALID_ARGUMENT);

                auto hash = FromJSIntArrayOrHexString(ctx, argv[1], 32);

                if (!hash.has_value() || hash->size() != 32)
                    returnJS(INVALID_ARGUMENT);

                base_uint<256> id =
                    ripple::base_uint<256>::fromVoid(hash->data());

                kl_out = ToJSIntArray(
                    ctx,
                    serialize_keylet_vec(
                        *kt == keylet_code::CHILD ? ripple::keylet::child(id)
                            : *kt == keylet_code::EMITTED_TXN
                            ? ripple::keylet::emittedTxn(id)
                            : *kt == keylet_code::HOOK_DEFINITION
                            ? ripple::keylet::hookDefinition(id)
                            : ripple::keylet::unchecked(id)));

                break;
            }

            case keylet_code::OWNER_DIR:
            case keylet_code::SIGNERS:
            case keylet_code::ACCOUNT:
            case keylet_code::HOOK: {
                if (argc != 2)
                    returnJS(INVALID_ARGUMENT);

                auto accid = FromJSIntArrayOrHexString(ctx, argv[1], 20);

                if (!accid.has_value() || accid->size() != 20)
                    returnJS(INVALID_ARGUMENT);

                auto const id = AccountID::fromVoid(accid->data());

                kl_out = ToJSIntArray(
                    ctx,
                    serialize_keylet_vec(
                        *kt == keylet_code::HOOK ? ripple::keylet::hook(id)
                            : *kt == keylet_code::SIGNERS
                            ? ripple::keylet::signers(id)
                            : *kt == keylet_code::OWNER_DIR
                            ? ripple::keylet::ownerDir(id)
                            : ripple::keylet::account(id)));

                break;
            }

            case keylet_code::OFFER:
            case keylet_code::CHECK:
            case keylet_code::ESCROW:
            case keylet_code::NFT_OFFER: {
                if (argc != 3)
                    returnJS(INVALID_ARGUMENT);

                auto accid = FromJSIntArrayOrHexString(ctx, argv[1], 20);

                auto u = FromJSInt(ctx, argv[2]);

                auto u2 = FromJSIntArrayOrHexString(ctx, argv[2], 32);

                if (!accid.has_value() || accid->size() != 20)
                    returnJS(INVALID_ARGUMENT);

                // sanity check, the second param can be either a uint32 or a
                // uint256
                if (!(u.has_value() && *u < 0xFFFFFFFFULL) &&
                    !(u2.has_value() && u2->size() == 32))
                    returnJS(INVALID_ARGUMENT);

                auto const id = AccountID::fromVoid(accid->data());

                std::variant<uint32_t, uint256> seq;
                if (u.has_value())
                    seq = (uint32_t)(*u);
                else
                    seq = uint256::fromVoid(u2->data());

                kl_out = ToJSIntArray(
                    ctx,
                    serialize_keylet_vec(
                        *kt == keylet_code::CHECK
                            ? ripple::keylet::check(id, seq)
                            : *kt == keylet_code::ESCROW
                            ? ripple::keylet::escrow(id, seq)
                            : *kt == keylet_code::NFT_OFFER
                            ? ripple::keylet::nftoffer(id, seq)
                            : ripple::keylet::offer(id, seq)));
                break;
            }

            case keylet_code::PAGE: {
                if (argc != 4)
                    returnJS(INVALID_ARGUMENT);

                auto h = FromJSInt(ctx, argv[2]);
                auto l = FromJSInt(ctx, argv[3]);
                auto u = FromJSIntArrayOrHexString(ctx, argv[1], 32);

                if (!h.has_value() || !l.has_value() || !u.has_value() ||
                    u->size() != 32)
                    returnJS(INVALID_ARGUMENT);

                uint64_t index = (((uint64_t)(*h)) << 32U) + ((uint64_t)(*l));
                ripple::Keylet kl = ripple::keylet::page(
                    ripple::base_uint<256>::fromVoid(u->data()), index);

                kl_out = ToJSIntArray(ctx, serialize_keylet_vec(kl));
                break;
            }

            case keylet_code::HOOK_STATE: {
                if (argc != 4)
                    returnJS(INVALID_ARGUMENT);

                auto accid = FromJSIntArrayOrHexString(ctx, argv[1], 20);
                auto key = FromJSIntArrayOrHexString(ctx, argv[2], 32);
                auto ns = FromJSIntArrayOrHexString(ctx, argv[3], 32);

                if (!accid.has_value() || accid->size() != 20 ||
                    !key.has_value() || key->size() != 32 || !ns.has_value() ||
                    ns->size() != 32)
                    returnJS(INVALID_ARGUMENT);

                kl_out = ToJSIntArray(
                    ctx,
                    serialize_keylet_vec(ripple::keylet::hookState(
                        AccountID::fromVoid(accid->data()),
                        ripple::base_uint<256>::fromVoid(key->data()),
                        ripple::base_uint<256>::fromVoid(ns->data()))));

                break;
            }

            case keylet_code::HOOK_STATE_DIR: {
                if (argc != 3)
                    returnJS(INVALID_ARGUMENT);

                auto accid = FromJSIntArrayOrHexString(ctx, argv[1], 20);
                auto ns = FromJSIntArrayOrHexString(ctx, argv[2], 32);

                if (!accid.has_value() || accid->size() != 20 ||
                    !ns.has_value() || ns->size() != 32)
                    returnJS(INVALID_ARGUMENT);

                kl_out = ToJSIntArray(
                    ctx,
                    serialize_keylet_vec(ripple::keylet::hookStateDir(
                        AccountID::fromVoid(accid->data()),
                        ripple::base_uint<256>::fromVoid(ns->data()))));

                break;
            }

            // skip is overloaded, has a single, optional 4 byte argument
            case keylet_code::SKIP: {
                if (argc > 2)
                    returnJS(INVALID_ARGUMENT);

                std::optional<int64_t> param;

                if (argc == 2)
                    param = FromJSInt(ctx, argv[1]);

                kl_out = ToJSIntArray(
                    ctx,
                    serialize_keylet_vec(
                        param.has_value()
                            ? ripple::keylet::skip((uint32_t)*param)
                            : ripple::keylet::skip()));

                break;
            }

            case keylet_code::AMENDMENTS:
            case keylet_code::FEES:
            case keylet_code::NEGATIVE_UNL:
            case keylet_code::EMITTED_DIR: {
                if (argc != 1)
                    returnJS(INVALID_ARGUMENT);

                kl_out = ToJSIntArray(
                    ctx,
                    serialize_keylet_vec(
                        *kt == keylet_code::AMENDMENTS
                            ? ripple::keylet::amendments()
                            : *kt == keylet_code::FEES ? ripple::keylet::fees()
                            : *kt == keylet_code::NEGATIVE_UNL
                            ? ripple::keylet::negativeUNL()
                            : ripple::keylet::emittedDir()));

                break;
            }

            case keylet_code::LINE: {
                if (argc != 4)
                    returnJS(INVALID_ARGUMENT);

                auto accid1 = FromJSIntArrayOrHexString(ctx, argv[1], 20);
                auto accid2 = FromJSIntArrayOrHexString(ctx, argv[2], 20);
                auto cur = FromJSIntArrayOrHexString(ctx, argv[3], 20);

                if (!accid1.has_value() || accid1->size() != 20 ||
                    !accid2.has_value() || accid2->size() != 20 ||
                    !cur.has_value() || (cur->size() != 20 && cur->size() != 3))
                    returnJS(INVALID_ARGUMENT);

                std::optional<Currency> cur2 =
                    parseCurrency(cur->data(), cur->size());

                if (!cur2.has_value())
                    returnJS(INVALID_ARGUMENT);

                kl_out = ToJSIntArray(
                    ctx,
                    serialize_keylet_vec(ripple::keylet::line(
                        AccountID::fromVoid(accid1->data()),
                        AccountID::fromVoid(accid2->data()),
                        *cur2)));
                break;
            }

            case keylet_code::DEPOSIT_PREAUTH: {
                if (argc != 3)
                    returnJS(INVALID_ARGUMENT);

                auto accid1 = FromJSIntArrayOrHexString(ctx, argv[1], 20);
                auto accid2 = FromJSIntArrayOrHexString(ctx, argv[2], 20);

                if (!accid1.has_value() || accid1->size() != 20 ||
                    !accid2.has_value() || accid2->size() != 20)
                    returnJS(INVALID_ARGUMENT);

                ripple::AccountID aid = AccountID::fromVoid(accid1->data());
                ripple::AccountID bid = AccountID::fromVoid(accid2->data());

                kl_out = ToJSIntArray(
                    ctx,
                    serialize_keylet_vec(
                        ripple::keylet::depositPreauth(aid, bid)));

                break;
            }

            case keylet_code::PAYCHAN: {
                if (argc != 4)
                    returnJS(INVALID_ARGUMENT);

                auto accid1 = FromJSIntArrayOrHexString(ctx, argv[1], 20);
                auto accid2 = FromJSIntArrayOrHexString(ctx, argv[2], 20);

                auto u = FromJSInt(ctx, argv[3]);
                auto u2 = FromJSIntArrayOrHexString(ctx, argv[3], 32);

                if (!accid1.has_value() || accid1->size() != 20 ||
                    !accid2.has_value() || accid2->size() != 20)
                    returnJS(INVALID_ARGUMENT);

                // sanity check, the second param can be either a uint32 or a
                // uint256
                if (!(u.has_value() && *u < 0xFFFFFFFFULL) &&
                    !(u2.has_value() && u2->size() == 32))
                    returnJS(INVALID_ARGUMENT);

                auto const id1 = AccountID::fromVoid(accid1->data());
                auto const id2 = AccountID::fromVoid(accid2->data());

                std::variant<uint32_t, uint256> seq;
                if (u.has_value())
                    seq = (uint32_t)(*u);
                else
                    seq = uint256::fromVoid(u2->data());

                kl_out = ToJSIntArray(
                    ctx,
                    serialize_keylet_vec(
                        ripple::keylet::payChan(id1, id2, seq)));

                break;
            }

            default: {
                returnJS(INVALID_ARGUMENT);
            }
        }

        if (!kl_out.has_value())
            returnJS(INTERNAL_ERROR);

        return *kl_out;
    }
    catch (std::exception& e)
    {
        JLOG(j.warn()) << "HookError[" << HC_ACC()
                       << "]: Keylet (JS) exception " << e.what();
        returnJS(INTERNAL_ERROR);
    }

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    util_keylet,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t keylet_type,
    uint32_t a,
    uint32_t b,
    uint32_t c,
    uint32_t d,
    uint32_t e,
    uint32_t f)
{
    WASM_HOOK_SETUP();

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (write_len < 34)
        return TOO_SMALL;

    try
    {
        switch (keylet_type)
        {
            // keylets that take a keylet and an 8 byte uint
            case keylet_code::QUALITY: {
                if (a == 0 || b == 0)
                    return INVALID_ARGUMENT;
                if (e != 0 || f != 0)
                    return INVALID_ARGUMENT;

                uint32_t read_ptr = a, read_len = b;

                if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
                    return OUT_OF_BOUNDS;

                if (read_len != 34)
                    return INVALID_ARGUMENT;

                // ensure it's a dir keylet or we will fail an assertion
                if (*(read_ptr + memory) != 0 ||
                    *(read_ptr + memory + 1) != 0x64U)
                    return INVALID_ARGUMENT;

                std::optional<ripple::Keylet> kl =
                    unserialize_keylet(memory + read_ptr, read_len);
                if (!kl)
                    return NO_SUCH_KEYLET;

                uint64_t arg = (((uint64_t)c) << 32U) + ((uint64_t)d);

                ripple::Keylet kl_out = ripple::keylet::quality(*kl, arg);

                return serialize_keylet(kl_out, memory, write_ptr, write_len);
            }

            // keylets that take a 32 byte uint
            case keylet_code::HOOK_DEFINITION:
            case keylet_code::CHILD:
            case keylet_code::EMITTED_TXN:
            case keylet_code::UNCHECKED: {
                if (a == 0 || b == 0)
                    return INVALID_ARGUMENT;

                if (c != 0 || d != 0 || e != 0 || f != 0)
                    return INVALID_ARGUMENT;

                uint32_t read_ptr = a, read_len = b;

                if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
                    return OUT_OF_BOUNDS;

                if (read_len != 32)
                    return INVALID_ARGUMENT;

                uint256 id = uint256::fromVoid(memory + read_ptr);

                ripple::Keylet kl = keylet_type == keylet_code::CHILD
                    ? ripple::keylet::child(id)
                    : keylet_type == keylet_code::EMITTED_TXN
                    ? ripple::keylet::emittedTxn(id)
                    : keylet_type == keylet_code::HOOK_DEFINITION
                    ? ripple::keylet::hookDefinition(id)
                    : ripple::keylet::unchecked(id);

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // keylets that take a 20 byte account id
            case keylet_code::OWNER_DIR:
            case keylet_code::SIGNERS:
            case keylet_code::ACCOUNT:
            case keylet_code::HOOK:
            case keylet_code::DID: {
                if (keylet_type == keylet_code::DID)
                {
                    if (!applyCtx.view().rules().enabled(featureDID))
                        return INVALID_ARGUMENT;
                }
                if (a == 0 || b == 0)
                    return INVALID_ARGUMENT;

                if (c != 0 || d != 0 || e != 0 || f != 0)
                    return INVALID_ARGUMENT;

                uint32_t read_ptr = a, read_len = b;

                if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
                    return OUT_OF_BOUNDS;

                if (read_len != 20)
                    return INVALID_ARGUMENT;

                ripple::AccountID id = AccountID::fromVoid(memory + read_ptr);

                ripple::Keylet kl = keylet_type == keylet_code::HOOK
                    ? ripple::keylet::hook(id)
                    : keylet_type == keylet_code::SIGNERS
                    ? ripple::keylet::signers(id)
                    : keylet_type == keylet_code::OWNER_DIR
                    ? ripple::keylet::ownerDir(id)
                    : keylet_type == keylet_code::DID
                    ? ripple::keylet::did(id)
                    : ripple::keylet::account(id);

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

                // keylets that take 20 byte account id, and (4 byte uint for 32
                // byte hash)
            case keylet_code::ORACLE: {
                if (!applyCtx.view().rules().enabled(featurePriceOracle))
                    return INVALID_ARGUMENT;

                if (a == 0 || b == 0)
                    return INVALID_ARGUMENT;
                if (d != 0 || e != 0 || f != 0)
                    return INVALID_ARGUMENT;

                uint32_t read_ptr = a, read_len = b;

                if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
                    return OUT_OF_BOUNDS;

                if (read_len != 20)
                    return INVALID_ARGUMENT;

                ripple::AccountID id = AccountID::fromVoid(memory + read_ptr);

                uint32_t seqId = c;

                ripple::Keylet kl = ripple::keylet::oracle(id, seqId);

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // keylets that take 20 byte account id, and UInt32or256 (4 byte
            // uint or 32 byte hash)
            case keylet_code::OFFER:
            case keylet_code::CHECK:
            case keylet_code::ESCROW:
            case keylet_code::NFT_OFFER: {
                if (a == 0 || b == 0)
                    return INVALID_ARGUMENT;
                if (e != 0 || f != 0)
                    return INVALID_ARGUMENT;

                uint32_t read_ptr = a, read_len = b;

                if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
                    return OUT_OF_BOUNDS;

                if (read_len != 20)
                    return INVALID_ARGUMENT;

                ripple::AccountID id = AccountID::fromVoid(memory + read_ptr);

                std::variant<uint32_t, uint256> seq;
                if (d == 0)
                    seq = c;
                else if (d != 32)
                    return INVALID_ARGUMENT;
                else
                {
                    if (NOT_IN_BOUNDS(c, 32, memory_length))
                        return OUT_OF_BOUNDS;
                    seq = uint256::fromVoid(memory + c);
                }

                ripple::Keylet kl = keylet_type == keylet_code::CHECK
                    ? ripple::keylet::check(id, seq)
                    : keylet_type == keylet_code::ESCROW
                    ? ripple::keylet::escrow(id, seq)
                    : keylet_type == keylet_code::NFT_OFFER
                    ? ripple::keylet::nftoffer(id, seq)
                    : ripple::keylet::offer(id, seq);

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

                // keylets that take 20 byte account id, and 4 byte uint
            case keylet_code::CRON: {
                if (!applyCtx.view().rules().enabled(featureCron))
                    return INVALID_ARGUMENT;

                if (a == 0 || b == 0)
                    return INVALID_ARGUMENT;
                if (e != 0 || f != 0 || d != 0)
                    return INVALID_ARGUMENT;

                uint32_t read_ptr = a, read_len = b;

                if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
                    return OUT_OF_BOUNDS;

                if (read_len != 20)
                    return INVALID_ARGUMENT;

                ripple::AccountID id = AccountID::fromVoid(memory + read_ptr);

                uint32_t seq = c;

                ripple::Keylet kl = ripple::keylet::cron(seq, id);

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // keylets that take a 32 byte uint and an 8byte uint64
            case keylet_code::PAGE: {
                if (a == 0 || b == 0)
                    return INVALID_ARGUMENT;

                if (e != 0 || f != 0)
                    return INVALID_ARGUMENT;

                uint32_t kread_ptr = a, kread_len = b;

                if (NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length))
                    return OUT_OF_BOUNDS;

                if (b != 32)
                    return INVALID_ARGUMENT;

                uint64_t index = (((uint64_t)c) << 32U) + ((uint64_t)d);
                ripple::Keylet kl =
                    ripple::keylet::page(uint256::fromVoid(memory + a), index);
                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // keylets that take both a 20 byte account id and a 32 byte uint
            case keylet_code::HOOK_STATE: {
                if (a == 0 || b == 0 || c == 0 || d == 0 || e == 0 || f == 0)
                    return INVALID_ARGUMENT;

                uint32_t aread_ptr = a, aread_len = b, kread_ptr = c,
                         kread_len = d, nread_ptr = e, nread_len = f;

                if (NOT_IN_BOUNDS(aread_ptr, aread_len, memory_length) ||
                    NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length) ||
                    NOT_IN_BOUNDS(nread_ptr, nread_len, memory_length))
                    return OUT_OF_BOUNDS;

                if (aread_len != 20 || kread_len != 32 || nread_len != 32)
                    return INVALID_ARGUMENT;

                ripple::Keylet kl = ripple::keylet::hookState(
                    AccountID::fromVoid(memory + aread_ptr),
                    uint256::fromVoid(memory + kread_ptr),
                    uint256::fromVoid(memory + nread_ptr));

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            case keylet_code::HOOK_STATE_DIR: {
                if (!applyCtx.view().rules().enabled(featureHooksUpdate1))
                    return INVALID_ARGUMENT;

                if (a == 0 || b == 0 || c == 0 || d == 0)
                    return INVALID_ARGUMENT;

                if (e != 0 || f != 0)
                    return INVALID_ARGUMENT;

                uint32_t aread_ptr = a, aread_len = b, nread_ptr = c,
                         nread_len = d;

                if (NOT_IN_BOUNDS(aread_ptr, aread_len, memory_length) ||
                    NOT_IN_BOUNDS(nread_ptr, nread_len, memory_length))
                    return OUT_OF_BOUNDS;

                if (aread_len != 20 || nread_len != 32)
                    return INVALID_ARGUMENT;

                ripple::Keylet kl = ripple::keylet::hookStateDir(
                    AccountID::fromVoid(memory + aread_ptr),
                    uint256::fromVoid(memory + nread_ptr));

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // skip is overloaded, has a single, optional 4 byte argument
            case keylet_code::SKIP: {
                if (c != 0 || d != 0 || e != 0 || f != 0 || b > 1)
                    return INVALID_ARGUMENT;

                ripple::Keylet kl =
                    (b == 0 ? ripple::keylet::skip() : ripple::keylet::skip(a));

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // no arguments
            case keylet_code::AMENDMENTS:
            case keylet_code::FEES:
            case keylet_code::NEGATIVE_UNL:
            case keylet_code::EMITTED_DIR: {
                if (a != 0 || b != 0 || c != 0 || d != 0 || e != 0 || f != 0)
                    return INVALID_ARGUMENT;

                auto makeKeyCache =
                    [](ripple::Keylet kl) -> std::array<uint8_t, 34> {
                    std::array<uint8_t, 34> d;

                    d[0] = (kl.type >> 8) & 0xFFU;
                    d[1] = (kl.type >> 0) & 0xFFU;
                    for (int i = 0; i < 32; ++i)
                        d[2 + i] = kl.key.data()[i];

                    return d;
                };

                static std::array<uint8_t, 34> cAmendments =
                    makeKeyCache(ripple::keylet::amendments());
                static std::array<uint8_t, 34> cFees =
                    makeKeyCache(ripple::keylet::fees());
                static std::array<uint8_t, 34> cNegativeUNL =
                    makeKeyCache(ripple::keylet::negativeUNL());
                static std::array<uint8_t, 34> cEmittedDir =
                    makeKeyCache(ripple::keylet::emittedDir());

                WRITE_WASM_MEMORY_AND_RETURN(
                    write_ptr,
                    write_len,
                    keylet_type == keylet_code::AMENDMENTS ? cAmendments.data()
                        : keylet_type == keylet_code::FEES ? cFees.data()
                        : keylet_type == keylet_code::NEGATIVE_UNL
                        ? cNegativeUNL.data()
                        : cEmittedDir.data(),
                    34,
                    memory,
                    memory_length);
            }

            case keylet_code::LINE: {
                if (a == 0 || b == 0 || c == 0 || d == 0 || e == 0 || f == 0)
                    return INVALID_ARGUMENT;

                uint32_t acc1_ptr = a, acc1_len = b, acc2_ptr = c, acc2_len = d,
                         cu_ptr = e, cu_len = f;

                if (NOT_IN_BOUNDS(acc1_ptr, acc1_len, memory_length) ||
                    NOT_IN_BOUNDS(acc2_ptr, acc2_len, memory_length) ||
                    NOT_IN_BOUNDS(cu_ptr, cu_len, memory_length))
                    return OUT_OF_BOUNDS;

                if (acc1_len != 20 || acc2_len != 20)
                    return INVALID_ARGUMENT;

                std::optional<Currency> cur =
                    parseCurrency(memory + cu_ptr, cu_len);
                if (!cur)
                    return INVALID_ARGUMENT;

                auto kl = ripple::keylet::line(
                    AccountID::fromVoid(memory + acc1_ptr),
                    AccountID::fromVoid(memory + acc2_ptr),
                    *cur);
                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // keylets that take two 20 byte account ids
            case keylet_code::DEPOSIT_PREAUTH: {
                if (a == 0 || b == 0 || c == 0 || d == 0)
                    return INVALID_ARGUMENT;

                if (e != 0 || f != 0)
                    return INVALID_ARGUMENT;

                uint32_t aread_ptr = a, aread_len = b;
                uint32_t bread_ptr = c, bread_len = d;

                if (NOT_IN_BOUNDS(aread_ptr, aread_len, memory_length) ||
                    NOT_IN_BOUNDS(bread_ptr, bread_len, memory_length))
                    return OUT_OF_BOUNDS;

                if (aread_len != 20 || bread_len != 20)
                    return INVALID_ARGUMENT;

                ripple::AccountID aid = AccountID::fromVoid(memory + aread_ptr);
                ripple::AccountID bid = AccountID::fromVoid(memory + bread_ptr);

                ripple::Keylet kl = ripple::keylet::depositPreauth(aid, bid);

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // keylets that take two 20 byte account ids and a 4 byte uint
            case keylet_code::PAYCHAN: {
                if (a == 0 || b == 0 || c == 0 || d == 0 || e == 0)
                    return INVALID_ARGUMENT;

                uint32_t aread_ptr = a, aread_len = b;
                uint32_t bread_ptr = c, bread_len = d;

                if (NOT_IN_BOUNDS(aread_ptr, aread_len, memory_length) ||
                    NOT_IN_BOUNDS(bread_ptr, bread_len, memory_length))
                    return OUT_OF_BOUNDS;

                if (aread_len != 20 || bread_len != 20)
                    return INVALID_ARGUMENT;

                ripple::AccountID aid = AccountID::fromVoid(memory + aread_ptr);
                ripple::AccountID bid = AccountID::fromVoid(memory + bread_ptr);

                std::variant<uint32_t, uint256> seq;
                if (f == 0)
                    seq = e;
                else if (f != 32)
                    return INVALID_ARGUMENT;
                else
                {
                    if (NOT_IN_BOUNDS(e, 32, memory_length))
                        return OUT_OF_BOUNDS;
                    seq = uint256::fromVoid(memory + e);
                }

                ripple::Keylet kl = ripple::keylet::payChan(aid, bid, seq);

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // keylets that take two 40 byte assets
            case keylet_code::AMM: {
                if (!applyCtx.view().rules().enabled(featureAMM))
                    return INVALID_ARGUMENT;

                if (a == 0 || b == 0 || c == 0 || d == 0)
                    return INVALID_ARGUMENT;

                if (e != 0 || f != 0)
                    return INVALID_ARGUMENT;

                uint32_t aread_ptr = a, aread_len = b;
                uint32_t bread_ptr = c, bread_len = d;

                if (NOT_IN_BOUNDS(aread_ptr, aread_len, memory_length) ||
                    NOT_IN_BOUNDS(bread_ptr, bread_len, memory_length))
                    return OUT_OF_BOUNDS;

                if (aread_len != 40 || bread_len != 40)
                    return INVALID_ARGUMENT;

                Currency aCur = Currency::fromVoid(memory + aread_ptr);
                Currency bCur = Currency::fromVoid(memory + bread_ptr);

                AccountID aAcc = AccountID::fromVoid(memory + aread_ptr + 20);
                AccountID bAcc = AccountID::fromVoid(memory + bread_ptr + 20);

                Issue aIss = Issue{aCur, aAcc};
                Issue bIss = Issue{bCur, bAcc};

                ripple::Keylet kl =
                    ripple::keylet::amm(Asset{aIss}, Asset{bIss});

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }
            case keylet_code::BRIDGE:
            case keylet_code::XCHAIN_OWNED_CLAIM_ID:
            case keylet_code::XCHAIN_OWNED_CREATE_ACCOUNT_CLAIM_ID: {
                if (!applyCtx.view().rules().enabled(featureXChainBridge))
                    return INVALID_ARGUMENT;
            }
            case keylet_code::MPTOKEN_ISSUANCE:
            case keylet_code::MPTOKEN: {
                if (!applyCtx.view().rules().enabled(featureMPTokensV1))
                    return INVALID_ARGUMENT;
            }
            case keylet_code::CREDENTIAL: {
                if (!applyCtx.view().rules().enabled(featureCredentials))
                    return INVALID_ARGUMENT;
            }
            case keylet_code::PERMISSIONED_DOMAIN: {
                if (!applyCtx.view().rules().enabled(
                        featurePermissionedDomains))
                    return INVALID_ARGUMENT;
            }
        }
    }
    catch (std::exception& e)
    {
        JLOG(j.warn()) << "HookError[" << HC_ACC() << "]: Keylet exception "
                       << e.what();
        return INTERNAL_ERROR;
    }

    return INVALID_ARGUMENT;

    WASM_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    prepare,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    ripple::Slice txBlob{
        reinterpret_cast<const void*>(memory + read_ptr), read_len};

    auto const res = api.prepare(txBlob);
    if (!res)
        return res.error();

    auto tx_blob = res.value();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr,
        tx_blob.size(),
        tx_blob.data(),
        tx_blob.size(),
        memory,
        memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    emit,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (write_len < 32)
        return TOO_SMALL;

    // Delegate to decoupled HookAPI for emit logic
    ripple::Slice txBlob{
        reinterpret_cast<const void*>(memory + read_ptr), read_len};
    auto const res = api.emit(txBlob);

    if (!res)
        return res.error();

    auto const& tpTrans = *res;  // 32 bytes
    auto const& txID = tpTrans->getID();

    if (txID.size() > write_len)
        return TOO_SMALL;

    if (NOT_IN_BOUNDS(write_ptr, txID.size(), memory_length))
        return OUT_OF_BOUNDS;

    auto const write_txid = [&]() -> int64_t {
        WRITE_WASM_MEMORY_AND_RETURN(
            write_ptr,
            txID.size(),
            txID.data(),
            txID.size(),
            memory,
            memory_length);
    };

    int64_t result = write_txid();

    if (result == 32)
        hookCtx.result.emittedTxn.push(tpTrans);

    return result;
    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, emit, JSValue raw_tx)
{
    JS_HOOK_SETUP();

    std::optional<std::vector<uint8_t>> tx =
        FromJSIntArrayOrHexString(ctx, raw_tx, 0x10000);

    if (!tx.has_value() || tx->empty())
    {
        // the user may specify the tx as a js object
        if (!JS_IsObject(raw_tx))
            returnJS(INVALID_ARGUMENT);

        // stringify it
        JSValue sdata =
            JS_JSONStringify(ctx, raw_tx, JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsString(sdata))
            returnJS(INVALID_ARGUMENT);

        size_t len;
        const char* cstr = JS_ToCStringLen(ctx, &len, sdata);
        if (len > 1024 * 1024)
        {
            JS_FreeCString(ctx, cstr);
            JS_FreeValue(ctx, sdata);
            returnJS(TOO_BIG);
        }
        std::string const tmpl(cstr, len);
        JS_FreeCString(ctx, cstr);
        JS_FreeValue(ctx, sdata);

        // parse it on rippled side
        Json::Value json;
        Json::Reader reader;
        if (!reader.parse(tmpl, json) || !json || !json.isObject())
            returnJS(INVALID_ARGUMENT);

        // turn the json into a stobject
        STParsedJSONObject parsed(std::string(jss::tx_json), json);
        if (!parsed.object.has_value())
            returnJS(INVALID_ARGUMENT);

        // turn the stobject into a tx_blob
        STObject& obj = *(parsed.object);
        Serializer s;
        obj.add(s);
        tx = s.getData();
    }

    auto const result = api.emit(Slice(tx->data(), tx->size()));
    if (!result)
        returnJS(result.error());
    auto const& tpTrans = *result;
    auto const& txID = tpTrans->getID();
    auto out = ToJSIntArray(ctx, txID);
    if (!out.has_value())
        returnJS(INTERNAL_ERROR);
    hookCtx.result.emittedTxn.push(tpTrans);

    return *out;

    JS_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, prepare, JSValue raw_tmpl)
{
    JS_HOOK_SETUP();

    if (!JS_IsObject(raw_tmpl))
        returnJS(INVALID_ARGUMENT);

    auto& view = applyCtx.view();

    // stringify it
    JSValue sdata = JS_JSONStringify(ctx, raw_tmpl, JS_UNDEFINED, JS_UNDEFINED);
    if (!JS_IsString(sdata))
        returnJS(INVALID_ARGUMENT);
    size_t len;
    const char* cstr = JS_ToCStringLen(ctx, &len, sdata);
    if (len > 1024 * 1024)
    {
        JS_FreeCString(ctx, cstr);
        JS_FreeValue(ctx, sdata);
        returnJS(TOO_BIG);
    }
    std::string tmpl(cstr, len);
    JS_FreeCString(ctx, cstr);
    JS_FreeValue(ctx, sdata);

    // parse it on rippled side
    Json::Value json;
    Json::Reader reader;
    if (!reader.parse(tmpl, json) || !json || !json.isObject())
        returnJS(INVALID_ARGUMENT);

    // add a dummy fee
    json[jss::Fee] = "0";

    // force key to empty
    json[jss::SigningPubKey] = "";

    // force sequence to 0
    json[jss::Sequence] = Json::Value(0u);

    std::string raddr = encodeBase58Token(
        TokenType::AccountID, hookCtx.result.account.data(), 20);

    json[jss::Account] = raddr;

    int64_t seq = view.info().seq;
    if (!json.isMember(jss::FirstLedgerSequence))
        json[jss::FirstLedgerSequence] = Json::Value((uint32_t)(seq + 1));

    if (!json.isMember(jss::LastLedgerSequence))
        json[jss::LastLedgerSequence] = Json::Value((uint32_t)(seq + 5));

    uint8_t details[512];
    if (!json.isMember(jss::EmitDetails))
    {
        auto const ret = api.etxn_details(details);
        if (!ret || ret.value() <= 2)
            returnJS(INTERNAL_ERROR);

        // truncate the head and tail (emit details object markers)
        Slice s(
            reinterpret_cast<void const*>(details + 1),
            (size_t)(ret.value() - 2));

        try
        {
            SerialIter sit{s};
            STObject st{sit, sfEmitDetails};
            json[jss::EmitDetails] = st.getJson(JsonOptions::none);
        }
        catch (std::exception const& ex)
        {
            JLOG(j.warn()) << "Exception in " << __func__ << ": " << ex.what();
            returnJS(INTERNAL_ERROR);
        }
    }

    STParsedJSONObject parsed(std::string(jss::tx_json), json);
    if (!parsed.object.has_value())
        returnJS(INVALID_ARGUMENT);

    STObject& obj = *(parsed.object);

    // serialize it
    Serializer s;
    obj.add(s);
    Blob tx_blob = s.getData();

    // run it through the fee estimate, this doubles as a txn sanity check
    auto const fee = api.etxn_fee_base(Slice(tx_blob.data(), tx_blob.size()));
    if (!fee || fee.value() < 0)
        returnJS(INVALID_ARGUMENT);

    json[jss::Fee] = to_string(fee.value());

    // send it back to the user

    const std::string flat = Json::FastWriter().write(json);

    JSValue out;
    out = JS_ParseJSON(ctx, flat.data(), flat.size(), "<json>");

    if (JS_IsException(out))
        returnJS(INTERNAL_ERROR);

    return out;

    JS_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, otxn_json)
{
    JS_HOOK_SETUP();

    auto const& st = std::make_unique<ripple::STObject>(
        hookCtx.emitFailure ? *(hookCtx.emitFailure)
                            : const_cast<ripple::STTx&>(applyCtx.tx)
                                  .downcast<ripple::STObject>());

    const std::string flat =
        Json::FastWriter().write(st->getJson(JsonOptions::none));

    JSValue out;
    out = JS_ParseJSON(ctx, flat.data(), flat.size(), "<json>");

    if (JS_IsException(out))
        returnJS(INTERNAL_ERROR);

    return out;

    JS_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, slot_json, JSValue raw_slot_no)
{
    JS_HOOK_SETUP();

    auto slot_no = FromJSInt(ctx, raw_slot_no);
    if (!slot_no.has_value())
        returnJS(INVALID_ARGUMENT);

    if (hookCtx.slot.find(*slot_no) == hookCtx.slot.end())
        returnJS(DOESNT_EXIST);

    if (hookCtx.slot[*slot_no].entry == 0)
        returnJS(INTERNAL_ERROR);

    const std::string flat = Json::FastWriter().write(
        hookCtx.slot[*slot_no].entry->getJson(JsonOptions::none));

    JSValue out;
    out = JS_ParseJSON(ctx, flat.data(), flat.size(), "<json>");

    if (JS_IsException(out))
        returnJS(INTERNAL_ERROR);

    return out;

    JS_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, sto_to_json, JSValue raw_sto_in)
{
    JS_HOOK_SETUP();
    auto sto_in = FromJSIntArrayOrHexString(ctx, raw_sto_in, 16 * 1024);
    if (!sto_in.has_value() || sto_in->empty())
        returnJS(INVALID_ARGUMENT);

    std::unique_ptr<STObject> obj;
    try
    {
        SerialIter sit(makeSlice(*sto_in));
        obj = std::make_unique<STObject>(std::ref(sit), sfGeneric);

        if (!obj)
            returnJS(INVALID_ARGUMENT);

        const std::string flat =
            Json::FastWriter().write(obj->getJson(JsonOptions::none));

        JSValue out;
        out = JS_ParseJSON(ctx, flat.data(), flat.size(), "<json>");

        if (JS_IsException(out))
            returnJS(INTERNAL_ERROR);

        return out;
    }
    catch (std::exception const& e)
    {
        returnJS(INVALID_ARGUMENT);
    }

    JS_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, sto_from_json, JSValue raw_json_in)
{
    JS_HOOK_SETUP();

    auto [len, in] = FromJSString(ctx, raw_json_in, 64 * 1024);
    if (!in.has_value())
    {
        if (!JS_IsObject(raw_json_in))
            returnJS(INVALID_ARGUMENT);

        // stringify it
        JSValue sdata =
            JS_JSONStringify(ctx, raw_json_in, JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsString(sdata))
            returnJS(INVALID_ARGUMENT);

        const char* cstr = JS_ToCStringLen(ctx, &len, sdata);
        if (len > 64 * 1024)
        {
            JS_FreeCString(ctx, cstr);
            JS_FreeValue(ctx, sdata);
            returnJS(TOO_BIG);
        }

        in = std::string(cstr, len);
        JS_FreeCString(ctx, cstr);
        JS_FreeValue(ctx, sdata);
    }

    if (!in.has_value() || len <= 0 || in->empty())
        returnJS(INVALID_ARGUMENT);

    Json::Value json;
    Json::Reader reader;
    if (!reader.parse(*in, json) || !json || !json.isObject())
        returnJS(INVALID_ARGUMENT);

    // turn the json into a stobject
    STParsedJSONObject parsed(std::string(jss::tx_json), json);
    if (!parsed.object.has_value())
        returnJS(INVALID_ARGUMENT);

    // turn the stobject into a tx_blob
    STObject& obj = *(parsed.object);
    Serializer s;
    obj.add(s);

    Blob b = s.getData();

    auto out = ToJSIntArray(ctx, b);

    if (!out.has_value())
        returnJS(INTERNAL_ERROR);

    return *out;

    JS_HOOK_TEARDOWN();
}

// When implemented will return the hash of the current hook
DEFINE_WASM_FUNCTION(
    int64_t,
    hook_hash,
    uint32_t write_ptr,
    uint32_t write_len,
    int32_t hook_no)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (write_len < 32)
        return TOO_SMALL;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    auto const result = api.hook_hash(hook_no);
    if (!result)
        return result.error();
    auto const& hash = result.value();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, hash.data(), hash.size(), memory, memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, hook_hash, JSValue raw_hook_no)
{
    JS_HOOK_SETUP();

    auto hook_no_opt = FromJSInt(ctx, raw_hook_no);
    if (!hook_no_opt.has_value())
        returnJS(INVALID_ARGUMENT);

    int32_t hook_no = *hook_no_opt;

    auto const result = api.hook_hash(hook_no);
    if (!result)
        returnJS(result.error());
    auto const& hash = result.value();

    return ToJSHash(ctx, hash);

    JS_HOOK_TEARDOWN();
}

// Write the account id that the running hook is installed on into write_ptr
DEFINE_WASM_FUNCTION(
    int64_t,
    hook_account,
    uint32_t write_ptr,
    uint32_t ptr_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(write_ptr, ptr_len, memory_length))
        return OUT_OF_BOUNDS;

    if (ptr_len < 20)
        return TOO_SMALL;

    auto const result = api.hook_account();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 20, result.data(), 20, memory, memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, hook_account)
{
    JS_HOOK_SETUP();

    auto out = ToJSIntArray(ctx, api.hook_account());

    if (!out.has_value())
        returnJS(INTERNAL_ERROR);

    return *out;

    JS_HOOK_TEARDOWN();
}

// Writes nonce into the write_ptr
DEFINE_WASM_FUNCTION(
    int64_t,
    etxn_nonce,
    uint32_t write_ptr,
    uint32_t write_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx, view on current stack

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    // It is also checked in api.etxn_nonce, but for backwards compatibility, it
    // must be checked before the TOO_SMALL check.
    if (hookCtx.emit_nonce_counter > hook_api::max_nonce)
        return TOO_MANY_NONCES;

    if (write_len < 32)
        return TOO_SMALL;

    auto const result = api.etxn_nonce();
    if (!result)
        return result.error();
    auto const& hash = result.value();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 32, hash.data(), 32, memory, memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, etxn_nonce)
{
    JS_HOOK_SETUP();

    auto const result = api.etxn_nonce();
    if (!result)
        returnJS(result.error());
    auto const& hash = result.value();

    std::vector<uint8_t> vec{hash.data(), hash.data() + 32};

    auto out = ToJSIntArray(ctx, vec);

    if (!out.has_value())
        returnJS(INTERNAL_ERROR);

    return *out;

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    ledger_nonce,
    uint32_t write_ptr,
    uint32_t write_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx, view on current stack

    if (write_len < 32)
        return TOO_SMALL;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    auto const result = api.ledger_nonce();
    if (!result)
        return result.error();
    auto const& hash = result.value();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 32, hash.data(), 32, memory, memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, ledger_nonce)
{
    JS_HOOK_SETUP();

    auto const result = api.ledger_nonce();
    if (!result)
        returnJS(result.error());
    auto const& hash = result.value();

    return ToJSHash(ctx, hash);

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    ledger_keylet,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t lread_ptr,
    uint32_t lread_len,
    uint32_t hread_ptr,
    uint32_t hread_len)
{
    WASM_HOOK_SETUP();

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length) ||
        NOT_IN_BOUNDS(lread_ptr, lread_len, memory_length) ||
        NOT_IN_BOUNDS(hread_ptr, hread_len, memory_length))
        return OUT_OF_BOUNDS;

    if (lread_len < 34U || hread_len < 34U || write_len < 34U)
        return TOO_SMALL;
    if (lread_len > 34U || hread_len > 34U || write_len > 34U)
        return TOO_BIG;

    std::optional<ripple::Keylet> klLo =
        unserialize_keylet(memory + lread_ptr, lread_len);
    if (!klLo)
        return INVALID_ARGUMENT;

    std::optional<ripple::Keylet> klHi =
        unserialize_keylet(memory + hread_ptr, hread_len);
    if (!klHi)
        return INVALID_ARGUMENT;

    auto const result = api.ledger_keylet(*klLo, *klHi);
    if (!result)
        return result.error();
    auto kl_out = result.value();

    return serialize_keylet(kl_out, memory, write_ptr, write_len);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, ledger_keylet, JSValue raw_lo, JSValue raw_hi)
{
    JS_HOOK_SETUP();

    auto lo = FromJSIntArrayOrHexString(ctx, raw_lo, 34);
    auto hi = FromJSIntArrayOrHexString(ctx, raw_hi, 34);

    if (!lo.has_value() || lo->size() != 34 || !hi.has_value() ||
        hi->size() != 34)
        returnJS(INVALID_ARGUMENT);

    std::optional<ripple::Keylet> klLo = unserialize_keylet(lo->data(), 34);
    std::optional<ripple::Keylet> klHi = unserialize_keylet(hi->data(), 34);
    if (!klLo || !klHi)
        returnJS(INVALID_ARGUMENT);

    auto const result = api.ledger_keylet(*klLo, *klHi);
    if (!result)
        returnJS(result.error());
    auto kl_out = result.value();

    auto out = ToJSIntArray(ctx, serialize_keylet_vec(kl_out));

    if (!out.has_value())
        returnJS(INTERNAL_ERROR);

    return *out;

    JS_HOOK_TEARDOWN();
}

// Reserve one or more transactions for emission from the running hook
DEFINE_WASM_FUNCTION(int64_t, etxn_reserve, uint32_t count)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    auto const result = api.etxn_reserve(count);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, etxn_reserve, JSValue raw_count)
{
    JS_HOOK_SETUP();

    auto count = FromJSInt(ctx, raw_count);

    auto const result = api.etxn_reserve(*count);
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, etxn_burden)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    auto const burden = api.etxn_burden();
    if (!burden)
        return burden.error();
    return burden.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, etxn_burden)
{
    JS_HOOK_SETUP();

    auto const result = api.etxn_burden();
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    util_sha512h,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx, view on current stack

    if (write_len < 32)
        return TOO_SMALL;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length) ||
        NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    auto const hash =
        api.util_sha512h(ripple::Slice{memory + read_ptr, read_len});

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 32, hash.data(), 32, memory, memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, util_sha512h, JSValue data)
{
    JS_HOOK_SETUP();

    auto vec = FromJSIntArrayOrHexString(ctx, data, 65536);
    if (!vec)
        returnJS(INVALID_ARGUMENT);

    // printf("jsutilsha512 debug. size=%d, ", vec->size());
    // for (int i = 0; i < vec->size(); ++i)
    // {
    //     printf("%02x,", (*vec)[i]);
    // }
    // printf("\n");

    auto const hash = api.util_sha512h(Slice{vec->data(), vec->size()});

    auto ret = ToJSIntArray(ctx, hash);

    if (!ret)
        returnJS(INTERNAL_ERROR);

    return *ret;

    JS_HOOK_TEARDOWN();
}

// Given an serialized object in memory locate and return the offset and length
// of the payload of a subfield of that object. Arrays are returned fully
// formed. If successful returns offset and length joined as int64_t. Use
// SUB_OFFSET and SUB_LENGTH to extract.
DEFINE_WASM_FUNCTION(
    int64_t,
    sto_subfield,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t field_id)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    Bytes data{memory + read_ptr, memory + read_ptr + read_len};
    auto const result = api.sto_subfield(data, field_id);
    if (!result)
        return result.error();
    auto const& pair = result.value();
    return (uint64_t(pair.first) << 32U) + (uint32_t)pair.second;

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, sto_subfield, JSValue raw_sto, JSValue raw_field_id)
{
    JS_HOOK_SETUP();

    auto sto_in = FromJSIntArrayOrHexString(ctx, raw_sto, 1024);
    auto field_id = FromJSInt(ctx, raw_field_id);

    if (!sto_in.has_value() || !field_id.has_value())
        returnJS(INVALID_ARGUMENT);

    if (*field_id > 0xFFFFFFFFULL || *field_id < 0)
        returnJS(INVALID_ARGUMENT);

    auto const result = api.sto_subfield(*sto_in, *field_id);
    if (!result)
        returnJS(result.error());
    auto const& pair = result.value();
    returnJS((uint64_t(pair.first) << 32U) + (uint32_t)pair.second);

    JS_HOOK_TEARDOWN();
}

// Same as subfield but indexes into a serialized array
DEFINE_WASM_FUNCTION(
    int64_t,
    sto_subarray,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t index_id)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    Bytes data{memory + read_ptr, memory + read_ptr + read_len};
    auto const result = api.sto_subarray(data, index_id);
    if (!result)
        return result.error();
    auto const& pair = result.value();
    return (uint64_t(pair.first) << 32U) + (uint32_t)pair.second;

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, sto_subarray, JSValue raw_sto, JSValue raw_index_id)
{
    JS_HOOK_SETUP();

    auto sto_in = FromJSIntArrayOrHexString(ctx, raw_sto, 1024);
    auto index_id = FromJSInt(ctx, raw_index_id);

    if (!sto_in.has_value() || !index_id.has_value())
        returnJS(INVALID_ARGUMENT);

    if (*index_id > 0xFFFFFFFFULL || *index_id < 0)
        returnJS(INVALID_ARGUMENT);

    auto const result = api.sto_subarray(*sto_in, *index_id);
    if (!result)
        returnJS(result.error());
    auto const& pair = result.value();
    returnJS((uint64_t(pair.first) << 32U) + (uint32_t)pair.second);

    JS_HOOK_TEARDOWN();
}

// Convert an account ID into a base58-check encoded r-address
DEFINE_WASM_FUNCTION(
    int64_t,
    util_raddr,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    auto const result =
        api.util_raddr(Bytes{memory + read_ptr, memory + read_ptr + read_len});
    if (!result)
        return result.error();
    auto const& raddr = result.value();

    if (write_len < raddr.size())
        return TOO_SMALL;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr,
        write_len,
        raddr.c_str(),
        raddr.size(),
        memory,
        memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, util_raddr, JSValue acc_id)
{
    JS_HOOK_SETUP();

    auto vec = FromJSIntArrayOrHexString(ctx, acc_id, 20);

    if (!vec)
        returnJS(INVALID_ARGUMENT);

    auto const result = api.util_raddr(*vec);
    if (!result)
        returnJS(result.error());
    auto const& raddr = result.value();

    return JS_NewString(ctx, raddr.c_str());

    JS_HOOK_TEARDOWN();
}

// Convert a base58-check encoded r-address into a 20 byte account id
DEFINE_WASM_FUNCTION(
    int64_t,
    util_accid,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (write_len < 20)
        return TOO_SMALL;

    if (read_len > 49)
        return TOO_BIG;

    // RH TODO we shouldn't need to slice this input but the base58 routine
    // fails if we dont... maybe some encoding or padding that shouldnt be there
    // or maybe something that should be there

    char buffer[50];
    for (int i = 0; i < read_len; ++i)
        buffer[i] = *(memory + read_ptr + i);
    buffer[read_len] = 0;

    std::string raddr{buffer};

    auto const result = api.util_accid(raddr);
    if (!result)
        return result.error();
    auto const& accountID = result.value();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, accountID.data(), 20, memory, memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, util_accid, JSValue raddr)
{
    JS_HOOK_SETUP();

    if (!JS_IsString(raddr))
        return JS_NewInt64(ctx, INVALID_ARGUMENT);

    auto [len, str] = FromJSString(ctx, raddr, 49);
    if (len > 49)
        returnJS(TOO_BIG);

    if (len < 20)
        returnJS(TOO_SMALL);

    if (!str)
        returnJS(INVALID_ARGUMENT);

    auto const result = api.util_accid(*str);
    if (!result)
        returnJS(result.error());
    auto const& accountID = result.value();

    if (auto ret = ToJSIntArray(ctx, accountID); ret)
        return *ret;

    returnJS(INTERNAL_ERROR);

    JS_HOOK_TEARDOWN();
}
/**
 * Check if any of the integer intervals overlap
 * [a,b,  c,d, ... ] ::== {a-b}, {c-d}, ...
 * TODO: naive implementation consider revising if
 * will be called with > 4 regions
 */
inline bool
overlapping_memory(std::vector<uint64_t> regions)
{
    for (uint64_t i = 0; i < regions.size() - 2; i += 2)
    {
        uint64_t a = regions[i + 0];
        uint64_t b = regions[i + 1];

        for (uint64_t j = i + 2; j < regions.size(); j += 2)
        {
            uint64_t c = regions[j + 0];
            uint64_t d = regions[j + 1];

            // only valid ways not to overlap are
            //
            // |===|  |===|
            // a   b  c   d
            //
            //      or
            // |===|  |===|
            // c   d  a   b

            if (d <= a || b <= c)
            {
                // no collision
                continue;
            }

            return true;
        }
    }

    return false;
}

/**
 * Inject a field into an sto if there is sufficient space
 * Field must be fully formed and wrapped (NOT JUST PAYLOAD)
 * sread - source object
 * fread - field to inject
 */
DEFINE_WASM_FUNCTION(
    int64_t,
    sto_emplace,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t sread_ptr,
    uint32_t sread_len,
    uint32_t fread_ptr,
    uint32_t fread_len,
    uint32_t field_id)
{
    WASM_HOOK_SETUP();

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(sread_ptr, sread_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(fread_ptr, fread_len, memory_length))
        return OUT_OF_BOUNDS;

    if (write_len < sread_len + fread_len)
        return TOO_SMALL;

    // RH TODO: put these constants somewhere (votable?)
    if (sread_len > 1024 * 16)
        return TOO_BIG;

    if (sread_len < 2)
        return TOO_SMALL;

    if (fread_len == 0 && fread_ptr == 0)
    {
        // this is a delete operation
        if (overlapping_memory(
                {write_ptr,
                 write_ptr + write_len,
                 sread_ptr,
                 sread_ptr + sread_len}))
            return MEM_OVERLAP;
    }
    else
    {
        if (fread_len > 4096)
            return TOO_BIG;

        if (fread_len < 2)
            return TOO_SMALL;

        // check for buffer overlaps
        if (overlapping_memory(
                {write_ptr,
                 write_ptr + write_len,
                 sread_ptr,
                 sread_ptr + sread_len,
                 fread_ptr,
                 fread_ptr + fread_len}))
            return MEM_OVERLAP;
    }

    Bytes source{memory + sread_ptr, memory + sread_ptr + sread_len};
    std::optional<Bytes> field;
    if (fread_len > 0 && fread_ptr > 0)
        field = Bytes{memory + fread_ptr, memory + fread_ptr + fread_len};
    auto const result = api.sto_emplace(source, field, field_id);
    if (!result)
        return result.error();
    auto const& bytes = result.value();

    if (bytes.size() > write_len)
        return INTERNAL_ERROR;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr,
        write_len,
        bytes.data(),
        bytes.size(),
        memory,
        memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(
    JSValue,
    sto_emplace,
    JSValue raw_sto,
    JSValue raw_field,
    JSValue raw_field_id)
{
    JS_HOOK_SETUP();

    auto sto = FromJSIntArrayOrHexString(ctx, raw_sto, 16 * 1024);
    auto field = FromJSIntArrayOrHexString(ctx, raw_field, 4 * 1024);
    auto field_id = FromJSInt(ctx, raw_field_id);

    if (!sto.has_value() || !field_id.has_value())
        returnJS(INVALID_ARGUMENT);

    if (*field_id > 0xFFFFFFFFULL || *field_id < 0)
        returnJS(INVALID_ARGUMENT);

    if (!field.has_value() && !JS_IsUndefined(raw_field))
        returnJS(INVALID_ARGUMENT);

    bool const isErase = !field.has_value();

    auto const result =
        api.sto_emplace(*sto, isErase ? std::nullopt : field, *field_id);
    if (!result)
        returnJS(result.error());

    std::vector<uint8_t> const& vec = result.value();

    if (vec.size() > sto->size() + (isErase ? 0 : field->size()))
        returnJS(INTERNAL_ERROR);

    auto out = ToJSIntArray(ctx, vec);
    if (!out.has_value())
        returnJS(INTERNAL_ERROR);

    return *out;

    JS_HOOK_TEARDOWN();
}

/**
 * Remove a field from an sto if the field is present
 */
DEFINE_WASM_FUNCTION(
    int64_t,
    sto_erase,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t field_id)
{
    // proxy only no setup or teardown
    int64_t ret = sto_emplace(
        hookCtx,
        frameCtx,
        write_ptr,
        write_len,
        read_ptr,
        read_len,
        0,
        0,
        field_id);

    if (ret > 0 && ret == read_len)
        return DOESNT_EXIST;

    return ret;
}

DEFINE_JS_FUNCTION(JSValue, sto_erase, JSValue raw_sto, JSValue raw_field)
{
    JS_HOOK_SETUP();

    // Forward deletion call to sto_emplace (field is undefined)
    JSValueConst argv2[] = {argv[0], JS_UNDEFINED, argv[1]};
    auto ret = FORWARD_JS_FUNCTION_CALL(sto_emplace, 3, argv2);

    if (JS_IsObject(ret))
    {
        int64_t raw_sto_len =
            GetLengthOfAlreadyValidatedJSIntArrayOrHexString(ctx, argv[0]);
        int64_t len;
        js_get_length64(ctx, &len, ret);
        if (len == raw_sto_len)
        {
            JS_FreeValue(ctx, ret);
            returnJS(DOESNT_EXIST);
        }
    }
    return ret;
    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    sto_validate,
    uint32_t read_ptr,
    uint32_t read_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    // RH TODO: see if an internal ripple function/class would do this better

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    Bytes data{read_ptr + memory, read_ptr + read_len + memory};
    auto const result = api.sto_validate(data);
    if (!result)
        return result.error();
    return result.value() ? 1 : 0;

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, sto_validate, JSValue raw_sto)
{
    JS_HOOK_SETUP();

    auto sto = FromJSIntArrayOrHexString(ctx, raw_sto, 16 * 1024);

    if (!sto.has_value())
        returnJS(INVALID_ARGUMENT);

    auto const result = api.sto_validate(*sto);
    if (!result)
        returnJS(result.error());
    returnJS(result.value() ? 1 : 0);

    JS_HOOK_TEARDOWN();
}

// Validate either an secp256k1 signature or an ed25519 signature, using the
// XRPLD convention for identifying the key type. Pointer prefixes: d = data, s
// = signature, k = public key.
DEFINE_WASM_FUNCTION(
    int64_t,
    util_verify,
    uint32_t dread_ptr,
    uint32_t dread_len,
    uint32_t sread_ptr,
    uint32_t sread_len,
    uint32_t kread_ptr,
    uint32_t kread_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(dread_ptr, dread_len, memory_length) ||
        NOT_IN_BOUNDS(sread_ptr, sread_len, memory_length) ||
        NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length))
        return OUT_OF_BOUNDS;

    ripple::Slice key{
        reinterpret_cast<const void*>(kread_ptr + memory), kread_len};
    ripple::Slice data{
        reinterpret_cast<const void*>(dread_ptr + memory), dread_len};
    ripple::Slice sig{
        reinterpret_cast<const void*>(sread_ptr + memory), sread_len};

    auto const result = api.util_verify(data, sig, key);
    if (!result)
        return result.error();
    return result.value() ? 1 : 0;

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(
    JSValue,
    util_verify,
    JSValue rawData,
    JSValue rawSig,
    JSValue rawKey)
{
    JS_HOOK_SETUP();

    auto vKey = FromJSIntArrayOrHexString(ctx, rawKey, 33);

    if (!vKey || vKey->size() != 33)
        returnJS(INVALID_KEY);

    auto vData = FromJSIntArrayOrHexString(ctx, rawData, 65536);
    if (!vData || vData->empty())
        returnJS(INVALID_ARGUMENT);

    auto vSig = FromJSIntArrayOrHexString(ctx, rawSig, 1024);
    if (!vSig || vSig->size() < 30)
        returnJS(INVALID_ARGUMENT);

    ripple::Slice keyslice{vKey->data(), vKey->size()};
    ripple::Slice data{vData->data(), vData->size()};
    ripple::Slice sig{vSig->data(), vSig->size()};

    auto const result = api.util_verify(data, sig, keyslice);
    if (!result)
        returnJS(result.error());
    returnJS(result.value() ? 1 : 0);

    JS_HOOK_TEARDOWN();
}

// Return the current fee base of the current ledger (multiplied by a margin)
DEFINE_WASM_FUNCTION(int64_t, fee_base)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    return api.fee_base();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, fee_base)
{
    JS_HOOK_SETUP();

    returnJS(view.fees().base.drops());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    etxn_fee_base,
    uint32_t read_ptr,
    uint32_t read_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;
    ripple::Slice tx{
        reinterpret_cast<const void*>(read_ptr + memory), read_len};
    auto const fee_base = api.etxn_fee_base(tx);
    if (!fee_base)
        return fee_base.error();
    return fee_base.value();
    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, etxn_fee_base, JSValue txblob)
{
    JS_HOOK_SETUP();

    auto blob = FromJSIntArrayOrHexString(ctx, txblob, 65536);
    if (!blob.has_value() || blob->empty())
        returnJS(INVALID_ARGUMENT);

    auto const result = api.etxn_fee_base(Slice(blob->data(), blob->size()));
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    etxn_details,
    uint32_t write_ptr,
    uint32_t write_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    int64_t expected_size = 138U;
    if (!hookCtx.result.hasCallback)
        expected_size -= 22U;

    if (write_len < expected_size)
        return TOO_SMALL;

    auto const result = api.etxn_details(memory + write_ptr);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, etxn_details)
{
    JS_HOOK_SETUP();

    uint8_t out[138];

    auto const result = api.etxn_details(out);
    if (!result)
        returnJS(result.error());

    if (result.value() <= 0)
        returnJS(result.error());

    if (result.value() > 138)
        returnJS(INTERNAL_ERROR);

    auto outjs =
        ToJSIntArray(ctx, std::vector<uint8_t>{out, out + result.value()});

    if (!outjs.has_value())
        returnJS(INTERNAL_ERROR);

    return *outjs;

    JS_HOOK_TEARDOWN();
}

// Guard function... very important. Enforced on SetHook transaction, keeps
// track of how many times a runtime loop iterates and terminates the hook if
// the iteration count rises above a preset number of iterations as determined
// by the hook developer
DEFINE_WASM_FUNCTION(int32_t, _g, uint32_t id, uint32_t maxitr)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (hookCtx.guard_map.find(id) == hookCtx.guard_map.end())
        hookCtx.guard_map[id] = 1;
    else
        hookCtx.guard_map[id]++;

    if (hookCtx.guard_map[id] > maxitr)
    {
        if (id > 0xFFFFU)
        {
            JLOG(j.trace())
                << "HookInfo[" << HC_ACC() << "]: Macro guard violation. "
                << "Src line: " << (id & 0xFFFFU) << " "
                << "Macro line: " << (id >> 16) << " "
                << "Iterations: " << hookCtx.guard_map[id];
        }
        else
        {
            JLOG(j.trace()) << "HookInfo[" << HC_ACC() << "]: Guard violation. "
                            << "Src line: " << id << " "
                            << "Iterations: " << hookCtx.guard_map[id];
        }
        hookCtx.result.exitType = hook_api::ExitType::ROLLBACK;
        hookCtx.result.exitCode = GUARD_VIOLATION;
        return RC_ROLLBACK;
    }
    return 1;

    WASM_HOOK_TEARDOWN();
}

#define RETURN_IF_INVALID_FLOAT(float1)                             \
    {                                                               \
        if (float1 < 0)                                             \
            return hook_api::INVALID_FLOAT;                         \
        if (float1 != 0)                                            \
        {                                                           \
            uint64_t mantissa = get_mantissa(float1);               \
            int32_t exponent = get_exponent(float1);                \
            if (mantissa < minMantissa || mantissa > maxMantissa || \
                exponent > maxExponent || exponent < minExponent)   \
                return INVALID_FLOAT;                               \
        }                                                           \
    }

#define RETURNJS_IF_INVALID_FLOAT(float1)                           \
    {                                                               \
        if (float1 < 0)                                             \
            returnJS(hook_api::INVALID_FLOAT);                      \
        if (float1 != 0)                                            \
        {                                                           \
            uint64_t mantissa = get_mantissa(float1);               \
            int32_t exponent = get_exponent(float1);                \
            if (mantissa < minMantissa || mantissa > maxMantissa || \
                exponent > maxExponent || exponent < minExponent)   \
                returnJS(INVALID_FLOAT);                            \
        }                                                           \
    }

DEFINE_WASM_FUNCTION(
    int64_t,
    trace_float,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t float1)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx
                        // on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (!j.trace())
        return 0;

    if (read_len > 128)
        read_len = 128;

    // omit \0 if present
    if (read_len > 0 &&
        *((const char*)memory + read_ptr + read_len - 1) == '\0')
        read_len--;

    if (float1 == 0)
    {
        j.trace() << "HookTrace[" << HC_ACC() << "]: "
                  << (read_len == 0
                          ? ""
                          : std::string_view(
                                (const char*)memory + read_ptr, read_len))
                  << ": Float 0*10^(0) <ZERO>";
        return 0;
    }

    uint64_t man = get_mantissa(float1);
    int32_t exp = get_exponent(float1);
    bool neg = is_negative(float1);
    if (man < minMantissa || man > maxMantissa || exp < minExponent ||
        exp > maxExponent)
    {
        j.trace() << "HookTrace[" << HC_ACC() << "]:"
                  << (read_len == 0
                          ? ""
                          : std::string_view(
                                (const char*)memory + read_ptr, read_len))
                  << ": Float <INVALID>";
        return 0;
    }

    j.trace() << "HookTrace[" << HC_ACC() << "]:"
              << (read_len == 0 ? ""
                                : std::string_view(
                                      (const char*)memory + read_ptr, read_len))
              << ": Float " << (neg ? "-" : "") << man << "*10^(" << exp << ")";
    return 0;

    WASM_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_set, int32_t exp, int64_t mantissa)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    auto const result = api.float_set(exp, mantissa);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_set, JSValue raw_e, JSValue raw_m)
{
    JS_HOOK_SETUP();

    auto e = FromJSInt(ctx, raw_e);
    auto m = FromJSInt(ctx, raw_m);

    if (!e.has_value() || !m.has_value() || !fits_i32(e))
        returnJS(INVALID_ARGUMENT);

    int64_t mantissa = *m;
    int32_t exp = *e;

    auto const result = api.float_set(exp, mantissa);
    if (!result)
        returnJS(result.error());
    returnJSXFL(result.value());

    if (mantissa == 0)
        returnJSXFL(0);

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    float_int,
    int64_t float1,
    uint32_t decimal_places,
    uint32_t absolute)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result = api.float_int(float1, decimal_places, absolute);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(
    JSValue,
    float_int,
    JSValue raw_float1,
    JSValue raw_dp,
    JSValue raw_abs)
{
    JS_HOOK_SETUP();

    auto f1 = FromJSInt(ctx, raw_float1);
    auto dp = FromJSInt(ctx, raw_dp);
    auto ab = FromJSInt(ctx, raw_abs);

    if (any_missing(f1, dp, ab) || !fits_u32(dp, ab))
        returnJS(INVALID_ARGUMENT);

    RETURNJS_IF_INVALID_FLOAT(*f1);

    // Note! This is the only (?) place where a float/... method has to return
    // `returnJS` instead of `returnJSXFL` as this is where we cast to JS usable
    // Number
    auto const result = api.float_int(*f1, (uint32_t)(*dp), (uint32_t)(*ab));
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_multiply, int64_t float1, int64_t float2)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    auto const result = api.float_multiply(float1, float2);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_multiply, JSValue raw_f1, JSValue raw_f2)
{
    JS_HOOK_SETUP();

    auto f1 = FromJSInt(ctx, raw_f1);
    auto f2 = FromJSInt(ctx, raw_f2);

    if (any_missing(f1, f2))
        returnJS(INVALID_ARGUMENT);

    RETURNJS_IF_INVALID_FLOAT(*f1);
    RETURNJS_IF_INVALID_FLOAT(*f2);

    auto const result = api.float_multiply(*f1, *f2);
    if (!result)
        returnJS(result.error());
    returnJSXFL(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    float_mulratio,
    int64_t float1,
    uint32_t round_up,
    uint32_t numerator,
    uint32_t denominator)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result =
        api.float_mulratio(float1, round_up, numerator, denominator);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(
    JSValue,
    float_mulratio,
    JSValue raw_f1,
    JSValue raw_ru,
    JSValue raw_n,
    JSValue raw_d)
{
    JS_HOOK_SETUP();

    auto [f1, ru, n, d] = FromJSInts(ctx, raw_f1, raw_ru, raw_n, raw_d);

    if (any_missing(f1, ru, n, d) || !fits_u32(ru, n, d))
        returnJS(INVALID_ARGUMENT);

    RETURNJS_IF_INVALID_FLOAT(*f1);

    auto const result = api.float_mulratio(
        *f1, (uint32_t)(*ru), (uint32_t)(*n), (uint32_t)(*d));
    if (!result)
        returnJS(result.error());
    returnJSXFL(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_negate, int64_t float1)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    return api.float_negate(float1);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_negate, JSValue raw_f1)
{
    JS_HOOK_SETUP();

    auto float1 = FromJSInt(ctx, raw_f1);
    if (!float1.has_value())
        returnJS(INVALID_ARGUMENT);

    RETURNJS_IF_INVALID_FLOAT(*float1);

    returnJSXFL(api.float_negate(*float1));

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    float_compare,
    int64_t float1,
    int64_t float2,
    uint32_t mode)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    auto const result = api.float_compare(float1, float2, mode);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(
    JSValue,
    float_compare,
    JSValue raw_f1,
    JSValue raw_f2,
    JSValue raw_mode)
{
    JS_HOOK_SETUP();

    auto [f1, f2, mode] = FromJSInts(ctx, raw_f1, raw_f2, raw_mode);
    if (any_missing(f1, f2, mode) || !fits_u32(mode))
        returnJS(INVALID_ARGUMENT);

    RETURNJS_IF_INVALID_FLOAT(*f1);
    RETURNJS_IF_INVALID_FLOAT(*f2);

    auto const result = api.float_compare(*f1, *f2, *mode);
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_sum, int64_t float1, int64_t float2)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    auto const result = api.float_sum(float1, float2);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_sum, JSValue raw_f1, JSValue raw_f2)
{
    JS_HOOK_SETUP();

    auto [f1, f2] = FromJSInts(ctx, raw_f1, raw_f2);
    if (any_missing(f1, f2))
        returnJS(INVALID_ARGUMENT);

    auto const result = api.float_sum(*f1, *f2);
    if (!result)
        returnJS(result.error());
    returnJSXFL(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    float_sto,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t cread_ptr,
    uint32_t cread_len,
    uint32_t iread_ptr,
    uint32_t iread_len,
    int64_t float1,
    uint32_t field_code)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    std::optional<Currency> currency;
    std::optional<AccountID> issuer;

    // bounds and argument checks
    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (cread_len == 0)
    {
        if (cread_ptr != 0)
            return INVALID_ARGUMENT;
    }
    else
    {
        if (cread_len != 20 && cread_len != 3)
            return INVALID_ARGUMENT;

        if (NOT_IN_BOUNDS(cread_ptr, cread_len, memory_length))
            return OUT_OF_BOUNDS;

        currency = parseCurrency(memory + cread_ptr, cread_len);

        if (!currency)
            return INVALID_ARGUMENT;
    }

    if (iread_len == 0)
    {
        if (iread_ptr != 0)
            return INVALID_ARGUMENT;
    }
    else
    {
        if (iread_len != 20)
            return INVALID_ARGUMENT;

        if (NOT_IN_BOUNDS(iread_ptr, iread_len, memory_length))
            return OUT_OF_BOUNDS;

        issuer = AccountID::fromVoid(memory + iread_ptr);
    }

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result =
        api.float_sto(currency, issuer, float1, field_code, write_len);
    if (!result)
        return result.error();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr,
        write_len,
        (*result).data(),
        (*result).size(),
        memory,
        memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(
    JSValue,
    float_sto,
    JSValue raw_cur,
    JSValue raw_isu,
    JSValue raw_f1,
    JSValue raw_fc)
{
    JS_HOOK_SETUP();

    auto [f1, fc] = FromJSInts(ctx, raw_f1, raw_fc);
    auto cur = FromJSIntArrayOrHexString(ctx, raw_cur, 20);
    auto isu = FromJSIntArrayOrHexString(ctx, raw_isu, 20);

    if (!cur.has_value())
    {
        if (!JS_IsUndefined(raw_cur))
            returnJS(INVALID_ARGUMENT);
    }
    else
    {
        if (cur->size() != 3 && cur->size() != 20)
            returnJS(INVALID_ARGUMENT);
    }

    if (!isu.has_value())
    {
        if (!JS_IsUndefined(raw_isu))
            returnJS(INVALID_ARGUMENT);
    }
    else
    {
        if (isu->size() != 20)
            returnJS(INVALID_ARGUMENT);
    }

    if (any_missing(f1, fc) || !fits_u32(fc))
        returnJS(INVALID_ARGUMENT);

    if (cur.has_value() && cur->size() != 20 && cur->size() != 3)
        returnJS(INVALID_ARGUMENT);

    if (isu.has_value() && isu->size() != 20)
        returnJS(INVALID_ARGUMENT);

    std::optional<Currency> currency;
    std::optional<AccountID> issuer;
    if (cur.has_value())
        currency = parseCurrency(cur->data(), cur->size());
    if (isu.has_value())
        issuer = AccountID::fromVoid(isu->data());

    RETURNJS_IF_INVALID_FLOAT(*f1);

    // RH TODO: 64 is wrong, figure out the longest this can be (< 64)
    auto ret = api.float_sto(currency, issuer, *f1, *fc, 64);

    if (!ret)
        returnJS(ret.error());

    Bytes const& vec = ret.value();
    if (vec.size() > 64)
        returnJS(INTERNAL_ERROR);

    auto out = ToJSIntArray(ctx, vec);
    if (!out.has_value())
        returnJS(INTERNAL_ERROR);
    return *out;

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    float_sto_set,
    uint32_t read_ptr,
    uint32_t read_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    Bytes data{read_ptr + memory, read_ptr + read_len + memory};

    auto const result = api.float_sto_set(data);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_sto_set, JSValue raw_sto)
{
    JS_HOOK_SETUP();

    auto sto = FromJSIntArrayOrHexString(ctx, raw_sto, 0x10000);

    if (!sto.has_value())
        returnJS(INVALID_ARGUMENT);

    auto const result = api.float_sto_set(*sto);
    if (!result)
        returnJS(result.error());
    returnJSXFL(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_divide, int64_t float1, int64_t float2)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    auto const result = api.float_divide(float1, float2);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_divide, JSValue raw_f1, JSValue raw_f2)
{
    JS_HOOK_SETUP();

    auto [f1, f2] = FromJSInts(ctx, raw_f1, raw_f2);
    if (any_missing(f1, f2))
        returnJS(INVALID_ARGUMENT);

    RETURNJS_IF_INVALID_FLOAT(*f1);
    RETURNJS_IF_INVALID_FLOAT(*f2);

    auto const result = api.float_divide(*f1, *f2);
    if (!result)
        returnJS(result.error());
    returnJSXFL(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_one)
{
    return hookCtx.api().float_one();
}

DEFINE_JS_FUNCTION(JSValue, float_one)
{
    JS_HOOK_SETUP();

    returnJSXFL(api.float_one());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_invert, int64_t float1)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result = api.float_invert(float1);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_invert, JSValue raw_f1)
{
    JS_HOOK_SETUP();

    auto f1 = FromJSInt(ctx, raw_f1);
    if (!f1.has_value())
        returnJS(INVALID_ARGUMENT);

    RETURNJS_IF_INVALID_FLOAT(*f1);

    auto const result = api.float_invert(*f1);
    if (!result)
        returnJS(result.error());
    returnJSXFL(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_mantissa, int64_t float1)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result = api.float_mantissa(float1);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_mantissa, JSValue raw_f1)
{
    JS_HOOK_SETUP();

    auto f1 = FromJSInt(ctx, raw_f1);
    if (!f1.has_value())
        returnJS(INVALID_ARGUMENT);

    RETURNJS_IF_INVALID_FLOAT(*f1);

    auto const result = api.float_mantissa(*f1);
    if (!result)
        returnJS(result.error());
    returnJSXFL(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_sign, int64_t float1)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    return api.float_sign(float1);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_sign, JSValue raw_f1)
{
    JS_HOOK_SETUP();

    auto f1 = FromJSInt(ctx, raw_f1);
    if (!f1.has_value())
        returnJS(INVALID_ARGUMENT);

    RETURNJS_IF_INVALID_FLOAT(*f1);

    returnJS(api.float_sign(*f1));

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_log, int64_t float1)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    auto const result = api.float_log(float1);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_log, JSValue raw_f1)
{
    JS_HOOK_SETUP();

    auto f1 = FromJSInt(ctx, raw_f1);
    if (!f1.has_value())
        returnJS(INVALID_ARGUMENT);

    auto const result = api.float_log(*f1);
    if (!result)
        returnJS(result.error());
    returnJSXFL(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_root, int64_t float1, uint32_t n)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result = api.float_root(float1, n);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_root, JSValue raw_f1, JSValue raw_n)
{
    JS_HOOK_SETUP();

    auto [f1, n] = FromJSInts(ctx, raw_f1, raw_n);
    if (any_missing(f1, n) || !fits_u32(n))
        returnJS(INVALID_ARGUMENT);

    auto const result = api.float_root(*f1, (uint32_t)(*n));
    if (!result)
        returnJS(result.error());
    returnJSXFL(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    otxn_param,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    Bytes paramName{read_ptr + memory, read_ptr + read_len + memory};

    auto const result = api.otxn_param(paramName);
    if (!result)
        return result.error();
    auto const& val = result.value();

    if (val.size() > write_len)
        return TOO_SMALL;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, val.data(), val.size(), memory, memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, otxn_param, JSValue param_key)
{
    JS_HOOK_SETUP();

    std::optional<std::vector<uint8_t>> key = FromJSIntArrayOrHexString(
        ctx, param_key, hook::maxHookParameterKeySize());

    if (!key.has_value() || key->empty())
        returnJS(INVALID_ARGUMENT);

    std::vector<uint8_t> const& paramName = *key;

    auto const result = api.otxn_param(paramName);
    if (!result)
        returnJS(result.error());

    auto out = ToJSIntArray(ctx, result.value());
    if (!out.has_value())
        returnJS(INTERNAL_ERROR);
    return *out;

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    hook_param,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    Bytes paramName{read_ptr + memory, read_ptr + read_len + memory};

    auto const result = api.hook_param(paramName);

    if (!result)
        return result.error();

    auto const& val = result.value();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, val.data(), val.size(), memory, memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, hook_param, JSValue raw_name)
{
    JS_HOOK_SETUP();

    auto param_name = FromJSIntArrayOrHexString(
        ctx, raw_name, hook::maxHookParameterKeySize());

    if (!param_name.has_value() || param_name->empty())
        returnJS(INVALID_ARGUMENT);

    auto const result = api.hook_param(*param_name);
    if (!result)
        returnJS(result.error());

    auto out = ToJSIntArray(ctx, result.value());
    if (!out.has_value())
        returnJS(INTERNAL_ERROR);

    return *out;

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    hook_param_set,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t kread_ptr,
    uint32_t kread_len,
    uint32_t hread_ptr,
    uint32_t hread_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length) ||
        NOT_IN_BOUNDS(kread_ptr, kread_len, memory_length) ||
        NOT_IN_BOUNDS(hread_ptr, hread_len, memory_length))
        return OUT_OF_BOUNDS;

    {
        // those checks are also done in the HookAPI
        // but we need to check them here too for backwards compatibility
        if (kread_len < 1)
            return TOO_SMALL;

        if (kread_len > hook::maxHookParameterKeySize())
            return TOO_BIG;

        if (hread_len != 32)
            return INVALID_ARGUMENT;

        if (read_len > hook::maxHookParameterValueSize())
            return TOO_BIG;
    }

    Bytes paramName{kread_ptr + memory, kread_ptr + kread_len + memory};
    Bytes paramValue{read_ptr + memory, read_ptr + read_len + memory};
    ripple::uint256 hash = ripple::uint256::fromVoid(memory + hread_ptr);

    auto const result = api.hook_param_set(hash, paramName, paramValue);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(
    JSValue,
    hook_param_set,
    JSValue val,
    JSValue key,
    JSValue hhash)
{
    JS_HOOK_SETUP();

    auto param_key =
        FromJSIntArrayOrHexString(ctx, key, hook::maxHookParameterKeySize());
    auto param_val =
        FromJSIntArrayOrHexString(ctx, val, hook::maxHookParameterValueSize())
            .value_or(std::vector<uint8_t>{});
    auto param_hash = FromJSIntArrayOrHexString(ctx, hhash, 32);

    if (!param_key.has_value() || param_key->empty() ||
        param_key->size() > hook::maxHookParameterKeySize() ||
        param_val.size() > hook::maxHookParameterValueSize() ||
        !param_hash.has_value() || param_hash->size() != 32)
        returnJS(INVALID_ARGUMENT);

    ripple::uint256 hash = ripple::uint256::fromVoid(param_hash->data());

    if (hookCtx.result.overrideCount >= hook_api::max_params)
        returnJS(TOO_MANY_PARAMS);

    auto const result = api.hook_param_set(hash, *param_key, param_val);

    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    hook_skip,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t flags)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (read_len != 32)
        return INVALID_ARGUMENT;

    ripple::uint256 hash = ripple::uint256::fromVoid(memory + read_ptr);

    auto const result = api.hook_skip(hash, flags);
    if (!result)
        return result.error();
    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, hook_skip, JSValue raw_hhash, JSValue raw_flags)
{
    JS_HOOK_SETUP();

    auto hhash = FromJSIntArrayOrHexString(ctx, raw_hhash, 32);
    auto flags = FromJSInt(ctx, raw_flags);

    if (!hhash.has_value() || hhash->size() != 32 || !flags.has_value())
        returnJS(INVALID_ARGUMENT);

    auto const result =
        api.hook_skip(ripple::uint256::fromVoid(hhash->data()), *flags);
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, hook_pos)
{
    return hookCtx.api().hook_pos();
}

DEFINE_JS_FUNCTION(JSValue, hook_pos)
{
    JS_HOOK_SETUP();

    returnJS(api.hook_pos());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, hook_again)
{
    WASM_HOOK_SETUP();

    auto const result = api.hook_again();

    if (!result)
        return result.error();

    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, hook_again)
{
    JS_HOOK_SETUP();

    auto const result = api.hook_again();
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, meta_slot, uint32_t slot_into)
{
    WASM_HOOK_SETUP();

    auto const result = api.meta_slot(slot_into);
    if (!result)
        return result.error();

    return result.value();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, meta_slot, JSValue raw_slot_into)
{
    JS_HOOK_SETUP();

    auto slot_into = FromJSInt(ctx, raw_slot_into);
    if (!slot_into.has_value() || !fits_u32(slot_into))
        returnJS(INVALID_ARGUMENT);

    auto const result = api.meta_slot((uint32_t)(*slot_into));
    if (!result)
        returnJS(result.error());
    returnJS(result.value());

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(
    int64_t,
    xpop_slot,
    uint32_t slot_into_tx,
    uint32_t slot_into_meta)
{
    WASM_HOOK_SETUP();

    auto const result = api.xpop_slot(slot_into_tx, slot_into_meta);
    if (!result)
        return result.error();

    return std::get<0>(result.value()) << 16U | std::get<1>(result.value());

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(
    JSValue,
    xpop_slot,
    JSValue raw_slot_into_tx,
    JSValue raw_slot_into_meta)
{
    JS_HOOK_SETUP();

    auto slot_into_tx = FromJSInt(ctx, raw_slot_into_tx);
    auto slot_into_meta = FromJSInt(ctx, raw_slot_into_meta);
    if (any_missing(slot_into_tx, slot_into_meta) ||
        !fits_u32(slot_into_tx, slot_into_meta))
        returnJS(INVALID_ARGUMENT);

    auto const result = api.xpop_slot(*slot_into_tx, *slot_into_meta);
    if (!result)
        returnJS(result.error());

    returnJS(std::get<0>(result.value()) << 16U | std::get<1>(result.value()));

    JS_HOOK_TEARDOWN();
}

/*
DEFINE_JS_FUNCTION(
    int64_t,
    xpop_slot,
    uint32_t slot_into_tx,
    uint32_t slot_into_meta)
{
    JS_HOOK_SETUP();

    int64_t result = __xpop_slot(hookCtx, applyCtx, slot_into_tx,
slot_into_meta);

    JS_HOOK_TEARDOWN();
}

*/
