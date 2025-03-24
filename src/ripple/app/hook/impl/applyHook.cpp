#include <ripple/app/hook/applyHook.h>
#include <ripple/app/ledger/OpenLedger.h>
#include <ripple/app/ledger/TransactionMaster.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/app/tx/impl/Import.h>
#include <ripple/app/tx/impl/details/NFTokenUtils.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/Slice.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_value.h>
#include <ripple/json/json_writer.h>
#include <ripple/json/to_string.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/st.h>
#include <ripple/protocol/tokens.h>
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
        case ttOFFER_CREATE:  // this is handled seperately
        {
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

        default:
            return {};
    }

    std::vector<std::pair<AccountID, bool>> ret{tshEntries.size()};
    for (auto& [a, e] : tshEntries)
        ret[e.first] = std::pair<AccountID, bool>{a, e.second};

    return ret;
}

}  // namespace hook

namespace hook_float {

// power of 10 LUT for fast integer math
static int64_t power_of_ten[19] = {
    1LL,
    10LL,
    100LL,
    1000LL,
    10000LL,
    100000LL,
    1000000LL,
    10000000LL,
    100000000LL,
    1000000000LL,
    10000000000LL,
    100000000000LL,
    1000000000000LL,
    10000000000000LL,
    100000000000000LL,
    1000000000000000LL,  // 15
    10000000000000000LL,
    100000000000000000LL,
    1000000000000000000LL,
};

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

/**
 * This function normalizes the mantissa and exponent passed, if it can.
 * It returns the XFL and mutates the supplied manitssa and exponent.
 * If a negative mantissa is provided then the returned XFL has the negative
 * flag set. If there is an overflow error return XFL_OVERFLOW. On underflow
 * returns canonical 0
 */
template <typename T>
inline int64_t
normalize_xfl(T& man, int32_t& exp, bool neg = false)
{
    if (man == 0)
        return 0;

    if (man == std::numeric_limits<int64_t>::min())
        man++;

    constexpr bool sman = std::is_same<T, int64_t>::value;
    static_assert(sman || std::is_same<T, uint64_t>());

    if constexpr (sman)
    {
        if (man < 0)
        {
            man *= -1LL;
            neg = true;
        }
    }

    // mantissa order
    std::feclearexcept(FE_ALL_EXCEPT);
    int32_t mo = log10(man);
    // defensively ensure log10 produces a sane result; we'll borrow the
    // overflow error code if it didn't
    if (std::fetestexcept(FE_INVALID))
        return XFL_OVERFLOW;

    int32_t adjust = 15 - mo;

    if (adjust > 0)
    {
        // defensive check
        if (adjust > 18)
            return 0;
        man *= power_of_ten[adjust];
        exp -= adjust;
    }
    else if (adjust < 0)
    {
        // defensive check
        if (-adjust > 18)
            return XFL_OVERFLOW;
        man /= power_of_ten[-adjust];
        exp -= adjust;
    }

    if (man == 0)
    {
        exp = 0;
        return 0;
    }

    // even after adjustment the mantissa can be outside the range by one place
    // improving the math above would probably alleviate the need for these
    // branches
    if (man < minMantissa)
    {
        if (man == minMantissa - 1LL)
            man += 1LL;
        else
        {
            man *= 10LL;
            exp--;
        }
    }

    if (man > maxMantissa)
    {
        if (man == maxMantissa + 1LL)
            man -= 1LL;
        else
        {
            man /= 10LL;
            exp++;
        }
    }

    if (exp < minExponent)
    {
        man = 0;
        exp = 0;
        return 0;
    }

    if (man == 0)
    {
        exp = 0;
        return 0;
    }

    if (exp > maxExponent)
        return XFL_OVERFLOW;

    int64_t ret = make_float((uint64_t)man, exp, neg);
    if constexpr (sman)
    {
        if (neg)
            man *= -1LL;
    }

    return ret;
}

}  // namespace hook_float
using namespace hook_float;

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
            return {};

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
            return {};

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

uint32_t
hook::computeHookStateOwnerCount(uint32_t hookStateCount)
{
    return hookStateCount;
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
    if (data.size() > hook::maxHookStateDataSize())
        return temHOOK_DATA_TOO_LARGE;

    auto hookStateKeylet = ripple::keylet::hookState(acc, key, ns);
    auto hookStateDirKeylet = ripple::keylet::hookStateDir(acc, ns);

    uint32_t stateCount = sleAccount->getFieldU32(sfHookStateCount);
    uint32_t oldStateReserve = computeHookStateOwnerCount(stateCount);

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
        if (computeHookStateOwnerCount(stateCount) < oldStateReserve)
            adjustOwnerCount(view, sleAccount, -1, j);

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

        if (computeHookStateOwnerCount(stateCount) > oldStateReserve)
        {
            // the hook used its allocated allotment of state entries for its
            // previous ownercount increment ownercount and give it another
            // allotment

            ++ownerCount;
            XRPAmount const newReserve{view.fees().accountReserve(ownerCount)};

            if (STAmount((*sleAccount)[sfBalance]).xrp() < newReserve)
                return tecINSUFFICIENT_RESERVE;

            adjustOwnerCount(view, sleAccount, 1, j);
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
             .accountKeylet = keylet::account(account),
             .ownerDirKeylet = keylet::ownerDir(account),
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

// check the state cache
inline std::optional<
    std::reference_wrapper<std::pair<bool, ripple::Blob> const>>
lookup_state_cache(
    hook::HookContext& hookCtx,
    ripple::AccountID const& acc,
    ripple::uint256 const& ns,
    ripple::uint256 const& key)
{
    auto& stateMap = hookCtx.result.stateMap;
    if (stateMap.find(acc) == stateMap.end())
        return std::nullopt;

    auto& stateMapAcc = std::get<2>(stateMap[acc]);
    if (stateMapAcc.find(ns) == stateMapAcc.end())
        return std::nullopt;

    auto& stateMapNs = stateMapAcc[ns];

    auto const& ret = stateMapNs.find(key);

    if (ret == stateMapNs.end())
        return std::nullopt;

    return std::cref(ret->second);
}

// update the state cache
inline int64_t  // if negative a hook return code, if == 1 then success
set_state_cache(
    hook::HookContext& hookCtx,
    ripple::AccountID const& acc,
    ripple::uint256 const& ns,
    ripple::uint256 const& key,
    ripple::Blob const& data,
    bool modified)
{
    auto& stateMap = hookCtx.result.stateMap;
    auto& view = hookCtx.applyCtx.view();

    if (modified && stateMap.modified_entry_count >= max_state_modifications)
        return TOO_MANY_STATE_MODIFICATIONS;

    bool const createNamespace = view.rules().enabled(fixXahauV1) &&
        !view.exists(keylet::hookStateDir(acc, ns));

    if (stateMap.find(acc) == stateMap.end())
    {
        // if this is the first time this account has been interacted with
        // we will compute how many available reserve positions there are
        auto const& fees = hookCtx.applyCtx.view().fees();

        auto const accSLE = view.read(ripple::keylet::account(acc));

        if (!accSLE)
            return DOESNT_EXIST;

        STAmount bal = accSLE->getFieldAmount(sfBalance);

        int64_t availableForReserves = bal.xrp().drops() -
            fees.accountReserve(accSLE->getFieldU32(sfOwnerCount)).drops();

        int64_t increment = fees.increment.drops();

        if (increment <= 0)
            increment = 1;

        availableForReserves /= increment;

        if (availableForReserves < 1 && modified)
            return RESERVE_INSUFFICIENT;

        int64_t namespaceCount = accSLE->isFieldPresent(sfHookNamespaces)
            ? accSLE->getFieldV256(sfHookNamespaces).size()
            : 0;

        if (createNamespace)
        {
            // overflow should never ever happen but check anyway
            if (namespaceCount + 1 < namespaceCount)
                return INTERNAL_ERROR;

            if (++namespaceCount > hook::maxNamespaces())
                return TOO_MANY_NAMESPACES;
        }

        stateMap.modified_entry_count++;

        stateMap[acc] = {
            availableForReserves - 1,
            namespaceCount,
            {{ns, {{key, {modified, data}}}}}};
        return 1;
    }

    auto& availableForReserves = std::get<0>(stateMap[acc]);
    auto& namespaceCount = std::get<1>(stateMap[acc]);
    auto& stateMapAcc = std::get<2>(stateMap[acc]);
    bool const canReserveNew = availableForReserves > 0;

    if (stateMapAcc.find(ns) == stateMapAcc.end())
    {
        if (modified)
        {
            if (!canReserveNew)
                return RESERVE_INSUFFICIENT;

            if (createNamespace)
            {
                // overflow should never ever happen but check anyway
                if (namespaceCount + 1 < namespaceCount)
                    return INTERNAL_ERROR;

                if (namespaceCount + 1 > hook::maxNamespaces())
                    return TOO_MANY_NAMESPACES;

                namespaceCount++;
            }

            availableForReserves--;
            stateMap.modified_entry_count++;
        }

        stateMapAcc[ns] = {{key, {modified, data}}};

        return 1;
    }

    auto& stateMapNs = stateMapAcc[ns];
    if (stateMapNs.find(key) == stateMapNs.end())
    {
        if (modified)
        {
            if (!canReserveNew)
                return RESERVE_INSUFFICIENT;
            availableForReserves--;
            stateMap.modified_entry_count++;
        }

        stateMapNs[key] = {modified, data};
        hookCtx.result.changedStateCount++;
        return 1;
    }

    if (modified)
    {
        if (!stateMapNs[key].first)
            hookCtx.result.changedStateCount++;

        stateMap.modified_entry_count++;
        stateMapNs[key].first = true;
    }

    stateMapNs[key].second = data;
    return 1;
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

inline int64_t
__state_foreign_set(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    Blob const& data,
    uint256 const& key,
    uint256 const& ns,
    AccountID const& acc)
{
    int64_t aread_len = acc.size();
    int64_t read_len = data.size();

    // local modifications are always allowed
    if (aread_len == 0 || acc == hookCtx.result.account)
    {
        if (int64_t ret = set_state_cache(hookCtx, acc, ns, key, data, true);
            ret < 0)
            return ret;

        return read_len;
    }

    // execution to here means it's actually a foreign set
    if (hookCtx.result.foreignStateSetDisabled)
        return PREVIOUS_FAILURE_PREVENTS_RETRY;

    // first check if we've already modified this state
    auto cacheEntry = lookup_state_cache(hookCtx, acc, ns, key);
    if (cacheEntry && cacheEntry->get().first)
    {
        // if a cache entry already exists and it has already been modified
        // don't check grants again
        if (int64_t ret = set_state_cache(hookCtx, acc, ns, key, data, true);
            ret < 0)
            return ret;

        return read_len;
    }

    // cache miss or cache was present but entry was not marked as previously
    // modified therefore before continuing we need to check grants
    auto const sle = applyCtx.view().read(ripple::keylet::hook(acc));
    if (!sle)
        return INTERNAL_ERROR;

    bool found_auth = false;

    // we do this by iterating the hooks installed on the foreign account and in
    // turn their grants and namespaces
    auto const& hooks = sle->getFieldArray(sfHooks);
    for (auto const& hookObj : hooks)
    {
        // skip blank entries
        if (!hookObj.isFieldPresent(sfHookHash))
            continue;

        if (!hookObj.isFieldPresent(sfHookGrants))
            continue;

        auto const& hookGrants = hookObj.getFieldArray(sfHookGrants);

        if (hookGrants.size() < 1)
            continue;

        // the grant allows the hook to modify the granter's namespace only
        if (hookObj.isFieldPresent(sfHookNamespace))
        {
            if (hookObj.getFieldH256(sfHookNamespace) != ns)
                continue;
        }
        else
        {
            // fetch the hook definition
            auto const def =
                applyCtx.view().read(ripple::keylet::hookDefinition(
                    hookObj.getFieldH256(sfHookHash)));
            if (!def)  // should never happen except in a rare race condition
                continue;
            if (def->getFieldH256(sfHookNamespace) != ns)
                continue;
        }

        // this is expensive search so we'll disallow after one failed attempt
        for (auto const& hookGrantObj : hookGrants)
        {
            bool hasAuthorizedField = hookGrantObj.isFieldPresent(sfAuthorize);

            if (hookGrantObj.getFieldH256(sfHookHash) ==
                    hookCtx.result.hookHash &&
                (!hasAuthorizedField ||
                 hookGrantObj.getAccountID(sfAuthorize) ==
                     hookCtx.result.account))
            {
                found_auth = true;
                break;
            }
        }

        if (found_auth)
            break;
    }

    if (!found_auth)
    {
        // hook only gets one attempt
        hookCtx.result.foreignStateSetDisabled = true;
        return NOT_AUTHORIZED;
    }

    if (int64_t ret = set_state_cache(hookCtx, acc, ns, key, data, true);
        ret < 0)
        return ret;

    return read_len;
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

    uint32_t maxSize = hook::maxHookStateDataSize();
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

    return __state_foreign_set(hookCtx, applyCtx, j, data, *key, ns, acc);

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

    auto val =
        FromJSIntArrayOrHexString(ctx, raw_val, hook::maxHookStateDataSize());
    auto key_in = FromJSIntArrayOrHexString(ctx, raw_key, 32);
    auto ns_in = FromJSIntArrayOrHexString(ctx, raw_ns, 32);
    auto acc_in = FromJSIntArrayOrHexString(ctx, raw_acc, 20);

    // if (!val.has_value() && !JS_IsUndefined(raw_val))
    //     returnJS(INVALID_ARGUMENT);

    // if (!ns_in.has_value() && !JS_IsUndefined(raw_ns))
    //     returnJS(INVALID_ARGUMENT);

    // if (!acc_in.has_value() && !JS_IsUndefined(raw_acc))
    //     returnJS(INVALID_ARGUMENT);

    // val may be populated and empty, this is a delete operation...

    if (!key_in.has_value() || key_in->empty())
        returnJS(INVALID_ARGUMENT);

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

    returnJS(__state_foreign_set(hookCtx, applyCtx, j, data, *key, ns, acc));

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
        for (const auto& nsEntry : std::get<2>(accEntry.second))
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
    if (applyCtx.view().open())
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

    // first check if the requested state was previously cached this session
    auto cacheEntryLookup = lookup_state_cache(hookCtx, acc, ns, *key);
    if (cacheEntryLookup)
    {
        auto const& cacheEntry = cacheEntryLookup->get();

        WRITE_WASM_MEMORY_OR_RETURN_AS_INT64(
            write_ptr,
            write_len,
            cacheEntry.second.data(),
            cacheEntry.second.size(),
            false);
    }

    auto hsSLE = view.peek(keylet::hookState(acc, *key, ns));

    if (!hsSLE)
        return DOESNT_EXIST;

    Blob b = hsSLE->getFieldVL(sfHookStateData);

    // it exists add it to cache and return it
    if (set_state_cache(hookCtx, acc, ns, *key, b, false) < 0)
        return INTERNAL_ERROR;  // should never happen

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

    // first check if the requested state was previously cached this session
    auto cacheEntryLookup = lookup_state_cache(hookCtx, acc, ns, *key);
    if (cacheEntryLookup)
    {
        auto const& cacheEntry = cacheEntryLookup->get();

        auto out = ToJSIntArray(ctx, cacheEntry.second);

        if (!out)
            returnJS(INTERNAL_ERROR);

        return *out;
    }

    auto hsSLE = view.peek(keylet::hookState(acc, *key, ns));

    if (!hsSLE)
        returnJS(DOESNT_EXIST);

    Blob b = hsSLE->getFieldVL(sfHookStateData);

    // it exists add it to cache and return it
    if (set_state_cache(hookCtx, acc, ns, *key, b, false) < 0)
        returnJS(INTERNAL_ERROR);  // should never happen

    auto out = ToJSIntArray(ctx, b);

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

    auto const& txID =
        (hookCtx.emitFailure && !flags
             ? applyCtx.tx.getFieldH256(sfTransactionHash)
             : applyCtx.tx.getTransactionID());

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

    auto const& txID =
        (hookCtx.emitFailure && !flags
             ? applyCtx.tx.getFieldH256(sfTransactionHash)
             : applyCtx.tx.getTransactionID());

    auto out = ToJSIntArray(ctx, Slice{txID.data(), txID.size()});

    if (!out.has_value())
        returnJS(INTERNAL_ERROR);

    return *out;

    JS_HOOK_TEARDOWN();
}

// Return the tt (Transaction Type) numeric code of the originating transaction
DEFINE_WASM_FUNCNARG(int64_t, otxn_type)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (hookCtx.emitFailure)
        return safe_cast<TxType>(
            hookCtx.emitFailure->getFieldU16(sfTransactionType));

    return applyCtx.tx.getTxnType();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, otxn_type)
{
    JS_HOOK_SETUP();

    if (hookCtx.emitFailure)
        returnJS(safe_cast<TxType>(
            hookCtx.emitFailure->getFieldU16(sfTransactionType)));

    returnJS(applyCtx.tx.getTxnType());

    JS_HOOK_TEARDOWN();
}

inline int64_t
__otxn_slot(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint32_t slot_into)
{
    if (slot_into > hook_api::max_slots)
        return INVALID_ARGUMENT;

    // check if we can emplace the object to a slot
    if (slot_into == 0 && no_free_slots(hookCtx))
        return NO_FREE_SLOTS;

    if (slot_into == 0)
    {
        if (auto found = get_free_slot(hookCtx); found)
            slot_into = *found;
        else
            return NO_FREE_SLOTS;
    }

    auto const& st_tx = std::make_shared<ripple::STObject>(
        hookCtx.emitFailure ? *(hookCtx.emitFailure)
                            : const_cast<ripple::STTx&>(applyCtx.tx)
                                  .downcast<ripple::STObject>());

    hookCtx.slot[slot_into] = hook::SlotEntry{.storage = st_tx, .entry = 0};

    hookCtx.slot[slot_into].entry = &(*hookCtx.slot[slot_into].storage);

    return slot_into;
}

DEFINE_WASM_FUNCTION(int64_t, otxn_slot, uint32_t slot_into)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    return __otxn_slot(hookCtx, applyCtx, j, slot_into);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, otxn_slot, JSValue slot_into)
{
    JS_HOOK_SETUP();

    auto si = FromJSInt(ctx, slot_into);
    if (!si.has_value() || *si > 0xFFFFFFFFULL)
        returnJS(INVALID_ARGUMENT);

    returnJS(__otxn_slot(hookCtx, applyCtx, j, (uint32_t)(*si)));

    JS_HOOK_TEARDOWN();
}

// Compute the burden of an emitted transaction based on a number of factors
inline int64_t
__otxn_burden(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j)
{
    if (hookCtx.burden)
        return hookCtx.burden;

    auto const& tx = applyCtx.tx;
    if (!tx.isFieldPresent(sfEmitDetails))
        return 1;  // burden is always 1 if the tx wasn't a emit

    auto const& pd = const_cast<ripple::STTx&>(tx)
                         .getField(sfEmitDetails)
                         .downcast<STObject>();

    if (!pd.isFieldPresent(sfEmitBurden))
    {
        JLOG(j.warn())
            << "HookError[" << HC_ACC()
            << "]: found sfEmitDetails but sfEmitBurden was not present";
        return 1;
    }

    uint64_t burden = pd.getFieldU64(sfEmitBurden);
    burden &=
        ((1ULL << 63) -
         1);  // wipe out the two high bits just in case somehow they are set
    hookCtx.burden = burden;
    return (int64_t)(burden);
}

inline int64_t
__etxn_burden(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j)
{
    if (hookCtx.expected_etxn_count <= -1)
        return PREREQUISITE_NOT_MET;

    // always non-negative so cast is safe
    uint64_t last_burden = (uint64_t)__otxn_burden(hookCtx, applyCtx, j);

    uint64_t burden = last_burden * hookCtx.expected_etxn_count;
    if (burden <
        last_burden)  // this overflow will never happen but handle it anyway
        return FEE_TOO_LARGE;

    return burden;
}

// Return the burden of the originating transaction... this will be 1 unless the
// originating transaction was itself an emitted transaction from a previous
// hook invocation
DEFINE_WASM_FUNCNARG(int64_t, otxn_burden)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    return __otxn_burden(hookCtx, applyCtx, j);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, otxn_burden)
{
    JS_HOOK_SETUP();

    returnJS(__otxn_burden(hookCtx, applyCtx, j));

    JS_HOOK_TEARDOWN();
}

// Return the generation of the originating transaction... this will be 1 unless
// the originating transaction was itself an emitted transaction from a previous
// hook invocation
inline int64_t
__otxn_generation(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j)
{
    // cache the result as it will not change for this hook execution
    if (hookCtx.generation)
        return hookCtx.generation;

    auto const& tx = applyCtx.tx;
    if (!tx.isFieldPresent(sfEmitDetails))
        return 0;  // generation is always 0 if the tx wasn't a emit

    auto const& pd = const_cast<ripple::STTx&>(tx)
                         .getField(sfEmitDetails)
                         .downcast<STObject>();

    if (!pd.isFieldPresent(sfEmitGeneration))
    {
        JLOG(j.warn())
            << "HookError[" << HC_ACC()
            << "]: found sfEmitDetails but sfEmitGeneration was not present";
        return 0;
    }

    hookCtx.generation = pd.getFieldU32(sfEmitGeneration);
    return hookCtx.generation;
}

DEFINE_WASM_FUNCNARG(int64_t, otxn_generation)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    return __otxn_generation(hookCtx, applyCtx, j);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, otxn_generation)
{
    JS_HOOK_SETUP();

    returnJS(__otxn_generation(hookCtx, applyCtx, j));

    JS_HOOK_TEARDOWN();
}

// Return the generation of a hypothetically emitted transaction from this hook
inline int64_t
__etxn_generation(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j)
{
    return __otxn_generation(hookCtx, applyCtx, j) + 1;
}

DEFINE_WASM_FUNCNARG(int64_t, etxn_generation)
{
    WASM_HOOK_SETUP();

    return __etxn_generation(hookCtx, applyCtx, j);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, etxn_generation)
{
    JS_HOOK_SETUP();

    returnJS(__etxn_generation(hookCtx, applyCtx, j));

    JS_HOOK_TEARDOWN();
}

// Return the current ledger sequence number
DEFINE_WASM_FUNCNARG(int64_t, ledger_seq)
{
    WASM_HOOK_SETUP();

    return view.info().seq;

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, ledger_seq)
{
    JS_HOOK_SETUP();

    returnJS(view.info().seq);

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

    uint256 hash = view.info().parentHash;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, hash.data(), 32, memory, memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, ledger_last_hash)
{
    JS_HOOK_SETUP();

    return ToJSHash(ctx, view.info().parentHash);

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCNARG(int64_t, ledger_last_time)
{
    WASM_HOOK_SETUP();

    return std::chrono::duration_cast<std::chrono::seconds>(
               view.info().parentCloseTime.time_since_epoch())
        .count();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, ledger_last_time)
{
    JS_HOOK_SETUP();

    returnJS(std::chrono::duration_cast<std::chrono::seconds>(
                 view.info().parentCloseTime.time_since_epoch())
                 .count());

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

    SField const& fieldType = ripple::SField::getField(field_id);

    if (fieldType == sfInvalid)
        return INVALID_FIELD;

    if (!applyCtx.tx.isFieldPresent(fieldType))
        return DOESNT_EXIST;

    auto const& field = hookCtx.emitFailure
        ? hookCtx.emitFailure->getField(fieldType)
        : const_cast<ripple::STTx&>(applyCtx.tx).getField(fieldType);

    Serializer s;
    field.add(s);

    WRITE_WASM_MEMORY_OR_RETURN_AS_INT64(
        write_ptr,
        write_len,
        s.getDataPtr(),
        s.getDataLength(),
        field.getSType() == STI_ACCOUNT);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, otxn_field, JSValue raw_field_id)
{
    JS_HOOK_SETUP();

    auto field_id = FromJSInt(ctx, raw_field_id);
    if (!field_id.has_value() || *field_id > 0xFFFFFFFFULL)
        returnJS(INVALID_ARGUMENT);

    SField const& fieldType = ripple::SField::getField((uint32_t)(*field_id));

    if (fieldType == sfInvalid)
        returnJS(INVALID_FIELD);

    if (!applyCtx.tx.isFieldPresent(fieldType))
        returnJS(DOESNT_EXIST);

    auto const& field = hookCtx.emitFailure
        ? hookCtx.emitFailure->getField(fieldType)
        : const_cast<ripple::STTx&>(applyCtx.tx).getField(fieldType);

    Serializer s;
    field.add(s);

    uint8_t const* ptr = reinterpret_cast<uint8_t const*>(s.getDataPtr());
    size_t len = s.getDataLength();

    if (field.getSType() == STI_ACCOUNT && len > 1)
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

    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    if (hookCtx.slot[slot_no].entry == 0)
        return INTERNAL_ERROR;

    Serializer s;
    hookCtx.slot[slot_no].entry->add(s);

    WRITE_WASM_MEMORY_OR_RETURN_AS_INT64(
        write_ptr,
        write_len,
        s.getDataPtr(),
        s.getDataLength(),
        hookCtx.slot[slot_no].entry->getSType() == STI_ACCOUNT);

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

    if (hookCtx.slot.find(*slot_no) == hookCtx.slot.end())
        returnJS(DOESNT_EXIST);

    if (hookCtx.slot[*slot_no].entry == 0)
        returnJS(INTERNAL_ERROR);

    Serializer s;
    hookCtx.slot[*slot_no].entry->add(s);

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
        returnJS(data_as_int64(ptr, len));
    }

    return *out;

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, slot_clear, uint32_t slot_no)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    hookCtx.slot.erase(slot_no);
    hookCtx.slot_free.push(slot_no);

    return 1;

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, slot_clear, JSValue raw_slot_no)
{
    JS_HOOK_SETUP();

    auto slot_no = FromJSInt(ctx, raw_slot_no);
    if (!slot_no.has_value())
        returnJS(INVALID_ARGUMENT);

    if (hookCtx.slot.find(*slot_no) == hookCtx.slot.end())
        returnJS(DOESNT_EXIST);

    hookCtx.slot.erase(*slot_no);
    hookCtx.slot_free.push(*slot_no);

    returnJS(1);

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, slot_count, uint32_t slot_no)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    if (hookCtx.slot[slot_no].entry == 0)
        return INTERNAL_ERROR;

    if (hookCtx.slot[slot_no].entry->getSType() != STI_ARRAY)
        return NOT_AN_ARRAY;

    return hookCtx.slot[slot_no].entry->downcast<ripple::STArray>().size();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, slot_count, JSValue raw_slot_no)
{
    JS_HOOK_SETUP();

    auto slot_no = FromJSInt(ctx, raw_slot_no);
    if (!slot_no.has_value())
        returnJS(INVALID_ARGUMENT);

    if (hookCtx.slot.find(*slot_no) == hookCtx.slot.end())
        returnJS(DOESNT_EXIST);

    if (hookCtx.slot[*slot_no].entry == 0)
        returnJS(INTERNAL_ERROR);

    if (hookCtx.slot[*slot_no].entry->getSType() != STI_ARRAY)
        returnJS(NOT_AN_ARRAY);

    returnJS(hookCtx.slot[*slot_no].entry->downcast<ripple::STArray>().size());

    JS_HOOK_TEARDOWN();
}

inline int64_t
__slot_set(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    std::vector<uint8_t> const& slot_key,
    uint32_t slot_into)
{
    size_t read_len = slot_key.size();

    std::optional<std::shared_ptr<const ripple::STObject>> slot_value =
        std::nullopt;

    if (read_len == 34)
    {
        std::optional<ripple::Keylet> kl =
            unserialize_keylet(slot_key.data(), read_len);
        if (!kl)
            return DOESNT_EXIST;

        if (kl->key == beast::zero)
            return DOESNT_EXIST;

        auto const sle = applyCtx.view().read(*kl);
        if (!sle)
            return DOESNT_EXIST;

        slot_value = sle;
    }
    else if (read_len == 32)
    {
        uint256 hash = ripple::base_uint<256>::fromVoid(slot_key.data());

        ripple::error_code_i ec{ripple::error_code_i::rpcUNKNOWN};

        auto hTx = applyCtx.app.getMasterTransaction().fetch(hash, ec);

        if (auto const* p = std::get_if<std::pair<
                std::shared_ptr<ripple::Transaction>,
                std::shared_ptr<ripple::TxMeta>>>(&hTx))
            slot_value = p->first->getSTransaction();
        else
            return DOESNT_EXIST;
    }
    else
        return DOESNT_EXIST;

    if (!slot_value.has_value())
        return DOESNT_EXIST;

    if (slot_into == 0)
    {
        if (auto found = get_free_slot(hookCtx); found)
            slot_into = *found;
        else
            return NO_FREE_SLOTS;
    }

    hookCtx.slot[slot_into] =
        hook::SlotEntry{.storage = *slot_value, .entry = 0};
    hookCtx.slot[slot_into].entry = &(*hookCtx.slot[slot_into].storage);

    return slot_into;
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

    if ((read_len != 32 && read_len != 34) || slot_into > hook_api::max_slots)
        return INVALID_ARGUMENT;

    // check if we can emplace the object to a slot
    if (slot_into == 0 && no_free_slots(hookCtx))
        return NO_FREE_SLOTS;

    std::vector<uint8_t> slot_key{
        memory + read_ptr, memory + read_ptr + read_len};

    return __slot_set(hookCtx, applyCtx, j, slot_key, slot_into);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, slot_set, JSValue raw_key, JSValue raw_slot_into)
{
    JS_HOOK_SETUP();

    auto key = FromJSIntArrayOrHexString(ctx, raw_key, 34);
    auto slot_into = FromJSInt(ctx, raw_slot_into);

    if (!key.has_value() || !slot_into.has_value())
        returnJS(INVALID_ARGUMENT);

    if ((key->size() != 32 && key->size() != 34) || *slot_into < 0 ||
        *slot_into > hook_api::max_slots)
        returnJS(INVALID_ARGUMENT);

    // check if we can emplace the object to a slot
    if (*slot_into == 0 && no_free_slots(hookCtx))
        returnJS(NO_FREE_SLOTS);

    returnJS(__slot_set(hookCtx, applyCtx, j, *key, *slot_into));

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, slot_size, uint32_t slot_no)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    if (hookCtx.slot[slot_no].entry == 0)
        return INTERNAL_ERROR;

    // RH TODO: this is a very expensive way of computing size, cache it
    Serializer s;
    hookCtx.slot[slot_no].entry->add(s);
    return s.getDataLength();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, slot_size, JSValue raw_slot_no)
{
    JS_HOOK_SETUP();

    auto slot_no = FromJSInt(ctx, raw_slot_no);
    if (!slot_no.has_value() || *slot_no < 0 || *slot_no > hook_api::max_slots)
        returnJS(INVALID_ARGUMENT);

    if (hookCtx.slot.find(*slot_no) == hookCtx.slot.end())
        returnJS(DOESNT_EXIST);

    if (hookCtx.slot[*slot_no].entry == 0)
        returnJS(INTERNAL_ERROR);

    // RH TODO: this is a very expensive way of computing size, cache it
    Serializer s;
    hookCtx.slot[*slot_no].entry->add(s);
    returnJS(s.getDataLength());

    JS_HOOK_TEARDOWN();
}

inline int64_t
__slot_subarray(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint32_t parent_slot,
    uint32_t array_id,
    uint32_t new_slot)
{
    if (hookCtx.slot.find(parent_slot) == hookCtx.slot.end())
        return DOESNT_EXIST;

    if (hookCtx.slot[parent_slot].entry == 0)
        return INTERNAL_ERROR;

    if (hookCtx.slot[parent_slot].entry->getSType() != STI_ARRAY)
        return NOT_AN_ARRAY;

    if (new_slot == 0 && no_free_slots(hookCtx))
        return NO_FREE_SLOTS;

    if (new_slot > hook_api::max_slots)
        return INVALID_ARGUMENT;

    bool copied = false;
    try
    {
        ripple::STArray& parent_obj =
            const_cast<ripple::STBase&>(*hookCtx.slot[parent_slot].entry)
                .downcast<ripple::STArray>();

        if (parent_obj.size() <= array_id)
            return DOESNT_EXIST;

        if (new_slot == 0)
        {
            if (auto found = get_free_slot(hookCtx); found)
                new_slot = *found;
            else
                return NO_FREE_SLOTS;
        }

        // copy
        if (new_slot != parent_slot)
        {
            copied = true;
            hookCtx.slot[new_slot] = hookCtx.slot[parent_slot];
        }
        hookCtx.slot[new_slot].entry = &(parent_obj[array_id]);
        return new_slot;
    }
    catch (const std::bad_cast& e)
    {
        if (copied)
        {
            hookCtx.slot.erase(new_slot);
            hookCtx.slot_free.push(new_slot);
        }
        return NOT_AN_ARRAY;
    }
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

    return __slot_subarray(
        hookCtx, applyCtx, j, parent_slot, array_id, new_slot);

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

    returnJS(__slot_subarray(
        hookCtx, applyCtx, j, *parent_slot, *array_id, *new_slot));

    JS_HOOK_TEARDOWN();
}

inline int64_t
__slot_subfield(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint32_t parent_slot,
    uint32_t field_id,
    uint32_t new_slot)
{
    if (hookCtx.slot.find(parent_slot) == hookCtx.slot.end())
        return DOESNT_EXIST;

    if (new_slot == 0 && no_free_slots(hookCtx))
        return NO_FREE_SLOTS;

    if (new_slot > hook_api::max_slots)
        return INVALID_ARGUMENT;

    SField const& fieldCode = ripple::SField::getField(field_id);

    if (fieldCode == sfInvalid)
        return INVALID_FIELD;

    if (hookCtx.slot[parent_slot].entry == 0)
        return INTERNAL_ERROR;

    bool copied = false;

    try
    {
        ripple::STObject& parent_obj =
            const_cast<ripple::STBase&>(*hookCtx.slot[parent_slot].entry)
                .downcast<ripple::STObject>();

        if (!parent_obj.isFieldPresent(fieldCode))
            return DOESNT_EXIST;

        if (new_slot == 0)
        {
            if (auto found = get_free_slot(hookCtx); found)
                new_slot = *found;
            else
                return NO_FREE_SLOTS;
        }

        // copy
        if (new_slot != parent_slot)
        {
            copied = true;
            hookCtx.slot[new_slot] = hookCtx.slot[parent_slot];
        }

        hookCtx.slot[new_slot].entry = &(parent_obj.getField(fieldCode));
        return new_slot;
    }
    catch (const std::bad_cast& e)
    {
        if (copied)
        {
            hookCtx.slot.erase(new_slot);
            hookCtx.slot_free.push(new_slot);
        }
        return NOT_AN_OBJECT;
    }
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

    return __slot_subfield(
        hookCtx, applyCtx, j, parent_slot, field_id, new_slot);

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

    returnJS(__slot_subfield(
        hookCtx, applyCtx, j, *parent_slot, *field_id, *new_slot));

    JS_HOOK_TEARDOWN();
}

inline int64_t
__slot_type(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint32_t slot_no,
    uint32_t flags)
{
    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    if (hookCtx.slot[slot_no].entry == 0)
        return INTERNAL_ERROR;
    try
    {
        ripple::STBase& obj = const_cast<ripple::STBase&>(
            *hookCtx.slot[slot_no].entry);  //.downcast<ripple::STBase>();
        if (flags == 0)
            return obj.getFName().fieldCode;

        // this flag is for use with an amount field to determine if the amount
        // is native (xrp)
        if (flags == 1)
        {
            if (obj.getSType() != STI_AMOUNT)
                return NOT_AN_AMOUNT;
            return const_cast<ripple::STBase&>(*hookCtx.slot[slot_no].entry)
                .downcast<ripple::STAmount>()
                .native();
        }

        return INVALID_ARGUMENT;
    }
    catch (const std::bad_cast& e)
    {
        return INTERNAL_ERROR;
    }
}

DEFINE_WASM_FUNCTION(int64_t, slot_type, uint32_t slot_no, uint32_t flags)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    return __slot_type(hookCtx, applyCtx, j, slot_no, flags);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, slot_type, JSValue raw_slot_no, JSValue raw_flags)
{
    JS_HOOK_SETUP();

    auto slot_no = FromJSInt(ctx, raw_slot_no);
    auto flags = FromJSInt(ctx, raw_flags);

    if (!slot_no.has_value() || !flags.has_value())
        returnJS(INVALID_ARGUMENT);

    returnJS(__slot_type(hookCtx, applyCtx, j, *slot_no, *flags));

    JS_HOOK_TEARDOWN();
}

inline int64_t
__slot_float(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint32_t slot_no)
{
    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    if (hookCtx.slot[slot_no].entry == 0)
        return INTERNAL_ERROR;

    try
    {
        ripple::STAmount& st_amt =
            const_cast<ripple::STBase&>(*hookCtx.slot[slot_no].entry)
                .downcast<ripple::STAmount>();

        int64_t normalized = 0;
        if (st_amt.native())
        {
            ripple::XRPAmount amt = st_amt.xrp();
            int64_t drops = amt.drops();
            int32_t exp = -6;
            // normalize
            normalized = hook_float::normalize_xfl(drops, exp);
        }
        else
        {
            ripple::IOUAmount amt = st_amt.iou();
            normalized = make_float(amt);
        }

        if (normalized ==
            EXPONENT_UNDERSIZED /* exponent undersized (underflow) */)
            return 0;  // return 0 in this case
        return normalized;
    }
    catch (const std::bad_cast& e)
    {
        return NOT_AN_AMOUNT;
    }
}

DEFINE_WASM_FUNCTION(int64_t, slot_float, uint32_t slot_no)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    return __slot_float(hookCtx, applyCtx, j, slot_no);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, slot_float, JSValue raw_slot_no)
{
    JS_HOOK_SETUP();

    auto slot_no = FromJSInt(ctx, raw_slot_no);

    if (!slot_no.has_value())
        returnJS(INVALID_ARGUMENT);

    auto const out = __slot_float(hookCtx, applyCtx, j, *slot_no);
    if (out < 0)
        returnJS(out);

    returnJSXFL(out);

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

    auto const last = keylet_code::LAST_KLTYPE_V1;

    if (!kt.has_value() || *kt == 0 || *kt > last)
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
                        *kt == keylet_code::CHILD
                            ? ripple::keylet::child(id)
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
                            : *kt == keylet_code::FEES
                                ? ripple::keylet::fees()
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
                returnJS(INTERNAL_ERROR);
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

    bool const v1 = applyCtx.view().rules().enabled(featureHooksUpdate1);

    if (keylet_type == 0)
        return INVALID_ARGUMENT;

    auto const last =
        v1 ? keylet_code::LAST_KLTYPE_V1 : keylet_code::LAST_KLTYPE_V0;

    if (keylet_type > last)
        return INVALID_ARGUMENT;

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

                base_uint<256> id =
                    ripple::base_uint<256>::fromVoid(memory + read_ptr);

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
            case keylet_code::HOOK: {
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
                            : ripple::keylet::account(id);

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            // keylets that take 20 byte account id, and 4 byte uint
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
                ripple::Keylet kl = ripple::keylet::page(
                    ripple::base_uint<256>::fromVoid(memory + a), index);
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
                    ripple::base_uint<256>::fromVoid(memory + kread_ptr),
                    ripple::base_uint<256>::fromVoid(memory + nread_ptr));

                return serialize_keylet(kl, memory, write_ptr, write_len);
            }

            case keylet_code::HOOK_STATE_DIR: {
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
                    ripple::base_uint<256>::fromVoid(memory + nread_ptr));

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
                    keylet_type == keylet_code::AMENDMENTS
                        ? cAmendments.data()
                        : keylet_type == keylet_code::FEES
                            ? cFees.data()
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

                uint32_t hi_ptr = a, hi_len = b, lo_ptr = c, lo_len = d,
                         cu_ptr = e, cu_len = f;

                if (NOT_IN_BOUNDS(hi_ptr, hi_len, memory_length) ||
                    NOT_IN_BOUNDS(lo_ptr, lo_len, memory_length) ||
                    NOT_IN_BOUNDS(cu_ptr, cu_len, memory_length))
                    return OUT_OF_BOUNDS;

                if (hi_len != 20 || lo_len != 20)
                    return INVALID_ARGUMENT;

                std::optional<Currency> cur =
                    parseCurrency(memory + cu_ptr, cu_len);
                if (!cur)
                    return INVALID_ARGUMENT;

                auto kl = ripple::keylet::line(
                    AccountID::fromVoid(memory + hi_ptr),
                    AccountID::fromVoid(memory + lo_ptr),
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
        }
    }
    catch (std::exception& e)
    {
        JLOG(j.warn()) << "HookError[" << HC_ACC() << "]: Keylet exception "
                       << e.what();
        return INTERNAL_ERROR;
    }

    return NO_SUCH_KEYLET;

    WASM_HOOK_TEARDOWN();
}

// RH TODO: reorder functions to avoid prototyping if possible
inline int64_t
__etxn_fee_base(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint8_t* read_ptr,
    size_t read_len);

/* Emit a transaction from this hook. Transaction must be in STObject form,
 * fully formed and valid. XRPLD does not modify transactions it only checks
 * them for validity. */
inline std::variant<
    int64_t,                       // populated if error code
    std::shared_ptr<Transaction>>  // otherwise tx itself if tx is valid
__emit(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint8_t* read_ptr,
    uint32_t read_len)
{
    auto& app = applyCtx.app;
    auto& view = applyCtx.view();

    if (hookCtx.expected_etxn_count < 0)
        return PREREQUISITE_NOT_MET;

    if (hookCtx.result.emittedTxn.size() >= hookCtx.expected_etxn_count)
        return TOO_MANY_EMITTED_TXN;

    ripple::Blob blob{read_ptr, read_ptr + read_len};

    std::shared_ptr<STTx const> stpTrans;
    try
    {
        stpTrans = std::make_shared<STTx const>(SerialIter{read_ptr, read_len});
    }
    catch (std::exception& e)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC() << "]: Failed " << e.what()
                        << "\n";
        return EMISSION_FAILURE;
    }

    if (isPseudoTx(*stpTrans))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Attempted to emit pseudo txn.";
        return EMISSION_FAILURE;
    }

    // check the emitted txn is valid
    /* Emitted TXN rules
     * 0. Account must match the hook account
     * 1. Sequence: 0
     * 2. PubSigningKey: 000000000000000
     * 3. sfEmitDetails present and valid
     * 4. No sfTxnSignature
     * 5. LastLedgerSeq > current ledger, > firstledgerseq & LastLedgerSeq < seq
     * + 5
     * 6. FirstLedgerSeq > current ledger
     * 7. Fee must be correctly high
     * 8. The generation cannot be higher than 10
     */

    // rule 0: account must match the hook account
    if (!stpTrans->isFieldPresent(sfAccount) ||
        stpTrans->getAccountID(sfAccount) != hookCtx.result.account)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfAccount does not match hook account";
        return EMISSION_FAILURE;
    }

    // rule 1: sfSequence must be present and 0
    if (!stpTrans->isFieldPresent(sfSequence) ||
        stpTrans->getFieldU32(sfSequence) != 0)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfSequence missing or non-zero";
        return EMISSION_FAILURE;
    }

    // rule 2: sfSigningPubKey must be present and 00...00
    if (!stpTrans->isFieldPresent(sfSigningPubKey))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfSigningPubKey missing";
        return EMISSION_FAILURE;
    }

    auto const pk = stpTrans->getSigningPubKey();
    if (pk.size() != 33 && pk.size() != 0)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfSigningPubKey present but wrong size"
                        << " expecting 33 bytes";
        return EMISSION_FAILURE;
    }

    for (int i = 0; i < pk.size(); ++i)
        if (pk[i] != 0)
        {
            JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                            << "]: sfSigningPubKey present but non-zero.";
            return EMISSION_FAILURE;
        }

    // rule 2.a: no signers
    if (stpTrans->isFieldPresent(sfSigners))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfSigners not allowed in emitted txns.";
        return EMISSION_FAILURE;
    }

    // rule 2.b: ticketseq cannot be used
    if (stpTrans->isFieldPresent(sfTicketSequence))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfTicketSequence not allowed in emitted txns.";
        return EMISSION_FAILURE;
    }

    // rule 2.c sfAccountTxnID not allowed
    if (stpTrans->isFieldPresent(sfAccountTxnID))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfAccountTxnID not allowed in emitted txns.";
        return EMISSION_FAILURE;
    }

    // rule 3: sfEmitDetails must be present and valid
    if (!stpTrans->isFieldPresent(sfEmitDetails))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitDetails missing.";
        return EMISSION_FAILURE;
    }

    auto const& emitDetails = const_cast<ripple::STTx&>(*stpTrans)
                                  .getField(sfEmitDetails)
                                  .downcast<STObject>();

    if (!emitDetails.isFieldPresent(sfEmitGeneration) ||
        !emitDetails.isFieldPresent(sfEmitBurden) ||
        !emitDetails.isFieldPresent(sfEmitParentTxnID) ||
        !emitDetails.isFieldPresent(sfEmitNonce) ||
        !emitDetails.isFieldPresent(sfEmitHookHash))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitDetails malformed.";
        return EMISSION_FAILURE;
    }

    // rule 8: emit generation cannot exceed 10
    if (emitDetails.getFieldU32(sfEmitGeneration) >= 10)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitGeneration was 10 or more.";
        return EMISSION_FAILURE;
    }

    uint32_t gen = emitDetails.getFieldU32(sfEmitGeneration);
    uint64_t bur = emitDetails.getFieldU64(sfEmitBurden);
    ripple::uint256 const& pTxnID = emitDetails.getFieldH256(sfEmitParentTxnID);
    ripple::uint256 const& nonce = emitDetails.getFieldH256(sfEmitNonce);

    std::optional<ripple::AccountID> callback;
    if (emitDetails.isFieldPresent(sfEmitCallback))
        callback = emitDetails.getAccountID(sfEmitCallback);

    auto const& hash = emitDetails.getFieldH256(sfEmitHookHash);

    uint32_t gen_proper = (uint32_t)(__etxn_generation(hookCtx, applyCtx, j));

    if (gen != gen_proper)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitGeneration provided in EmitDetails "
                        << "not correct (" << gen << ") "
                        << "should be " << gen_proper;
        return EMISSION_FAILURE;
    }

    uint64_t bur_proper = (uint32_t)(__etxn_burden(hookCtx, applyCtx, j));
    if (bur != bur_proper)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitBurden provided in EmitDetails "
                        << "was not correct (" << bur << ") "
                        << "should be " << bur_proper;
        return EMISSION_FAILURE;
    }

    if (pTxnID != applyCtx.tx.getTransactionID())
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitParentTxnID provided in EmitDetails "
                        << "was not correct";
        return EMISSION_FAILURE;
    }

    if (hookCtx.nonce_used.find(nonce) == hookCtx.nonce_used.end())
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitNonce provided in EmitDetails "
                        << "was not generated by nonce api";
        return EMISSION_FAILURE;
    }

    if (callback && *callback != hookCtx.result.account)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitCallback account must be the account "
                        << "of the emitting hook";
        return EMISSION_FAILURE;
    }

    if (hash != hookCtx.result.hookHash)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC()
            << "]: sfEmitHookHash must be the hash of the emitting hook";
        return EMISSION_FAILURE;
    }

    // rule 4: sfTxnSignature must be absent
    if (stpTrans->isFieldPresent(sfTxnSignature))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfTxnSignature is present but should not be";
        return EMISSION_FAILURE;
    }

    // rule 5: LastLedgerSeq must be present and after current ledger
    if (!stpTrans->isFieldPresent(sfLastLedgerSequence))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfLastLedgerSequence missing";
        return EMISSION_FAILURE;
    }

    uint32_t tx_lls = stpTrans->getFieldU32(sfLastLedgerSequence);
    uint32_t ledgerSeq = view.info().seq;
    if (tx_lls < ledgerSeq + 1)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC()
            << "]: sfLastLedgerSequence invalid (less than next ledger)";
        return EMISSION_FAILURE;
    }

    if (tx_lls > ledgerSeq + 5)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC()
            << "]: sfLastLedgerSequence cannot be greater than current seq + 5";
        return EMISSION_FAILURE;
    }

    // rule 6
    if (!stpTrans->isFieldPresent(sfFirstLedgerSequence) ||
        stpTrans->getFieldU32(sfFirstLedgerSequence) > tx_lls)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfFirstLedgerSequence must be present and "
                        << "<= LastLedgerSequence";
        return EMISSION_FAILURE;
    }

    // rule 7 check the emitted txn pays the appropriate fee
    int64_t minfee = __etxn_fee_base(hookCtx, applyCtx, j, read_ptr, read_len);

    if (minfee < 0)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Fee could not be calculated";
        return EMISSION_FAILURE;
    }

    if (!stpTrans->isFieldPresent(sfFee))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Fee missing from emitted tx";
        return EMISSION_FAILURE;
    }

    int64_t fee = stpTrans->getFieldAmount(sfFee).xrp().drops();
    if (fee < minfee)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC()
            << "]: Fee on emitted txn is less than the minimum required fee";
        return EMISSION_FAILURE;
    }

    std::string reason;
    auto tpTrans = std::make_shared<Transaction>(stpTrans, reason, app);
    if (tpTrans->getStatus() != NEW)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: tpTrans->getStatus() != NEW";
        return EMISSION_FAILURE;
    }

    // preflight the transaction
    auto preflightResult = ripple::preflight(
        applyCtx.app,
        view.rules(),
        *stpTrans,
        ripple::ApplyFlags::tapPREFLIGHT_EMIT,
        j);

    if (!isTesSuccess(preflightResult.ter))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Transaction preflight failure: "
                        << preflightResult.ter;
        return EMISSION_FAILURE;
    }

    return tpTrans;
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

    auto ret = __emit(hookCtx, applyCtx, j, memory + read_ptr, read_len);

    if (std::holds_alternative<int64_t>(ret))
        return std::get<int64_t>(ret);

    auto const& tpTrans = std::get<std::shared_ptr<Transaction>>(ret);

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

    auto ret = __emit(hookCtx, applyCtx, j, tx->data(), tx->size());

    if (std::holds_alternative<int64_t>(ret))
        returnJS(std::get<int64_t>(ret));

    auto const& tpTrans = std::get<std::shared_ptr<Transaction>>(ret);
    auto const& txID = tpTrans->getID();
    auto out = ToJSIntArray(ctx, txID);

    if (!out.has_value())
        returnJS(INTERNAL_ERROR);

    hookCtx.result.emittedTxn.push(tpTrans);

    return *out;

    JS_HOOK_TEARDOWN();
}

inline int64_t
__etxn_details(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint8_t* out_ptr,
    size_t max_len);

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
        int64_t ret = __etxn_details(hookCtx, applyCtx, j, details, 512);
        if (ret <= 2)
            returnJS(INTERNAL_ERROR);

        // truncate the head and tail (emit details object markers)
        Slice s(reinterpret_cast<void const*>(details + 1), (size_t)(ret - 2));

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
    int64_t fee =
        __etxn_fee_base(hookCtx, applyCtx, j, tx_blob.data(), tx_blob.size());
    if (fee < 0)
        returnJS(INVALID_ARGUMENT);

    json[jss::Fee] = to_string(fee);

    // send it back to the user

    const std::string flat = Json::FastWriter().write(json);

    JSValue out;
    out = JS_ParseJSON(ctx, flat.data(), flat.size(), "<json>");

    if (JS_IsException(out))
        returnJS(INTERNAL_ERROR);

    return out;

    JS_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, otxn_json)
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

    if (hook_no == -1)
    {
        WRITE_WASM_MEMORY_AND_RETURN(
            write_ptr,
            write_len,
            hookCtx.result.hookHash.data(),
            32,
            memory,
            memory_length);
    }

    std::shared_ptr<SLE> hookSLE =
        applyCtx.view().peek(hookCtx.result.hookKeylet);
    if (!hookSLE || !hookSLE->isFieldPresent(sfHooks))
        return INTERNAL_ERROR;

    ripple::STArray const& hooks = hookSLE->getFieldArray(sfHooks);
    if (hook_no >= hooks.size())
        return DOESNT_EXIST;

    auto const& hook = hooks[hook_no];
    if (!hook.isFieldPresent(sfHookHash))
        return DOESNT_EXIST;

    ripple::uint256 const& hash = hook.getFieldH256(sfHookHash);

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

    if (hook_no == -1)
        return ToJSHash(ctx, hookCtx.result.hookHash);

    std::shared_ptr<SLE> hookSLE =
        applyCtx.view().peek(hookCtx.result.hookKeylet);
    if (!hookSLE || !hookSLE->isFieldPresent(sfHooks))
        returnJS(INTERNAL_ERROR);

    ripple::STArray const& hooks = hookSLE->getFieldArray(sfHooks);
    if (hook_no >= hooks.size())
        returnJS(DOESNT_EXIST);

    auto const& hook = hooks[hook_no];
    if (!hook.isFieldPresent(sfHookHash))
        returnJS(DOESNT_EXIST);

    return ToJSHash(ctx, hook.getFieldH256(sfHookHash));

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

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr,
        20,
        hookCtx.result.account.data(),
        20,
        memory,
        memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, hook_account);
{
    JS_HOOK_SETUP();

    auto out = ToJSIntArray(ctx, Slice{hookCtx.result.account.data(), 20u});

    if (!out.has_value())
        returnJS(INTERNAL_ERROR);

    return *out;

    JS_HOOK_TEARDOWN();
}

// Deterministic nonces (can be called multiple times)
inline std::optional<uint256>
__etxn_nonce(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j)
{
    if (hookCtx.emit_nonce_counter > hook_api::max_nonce)
        return {};

    // in some cases the same hook might execute multiple times
    // on one txn, therefore we need to pass this information to the nonce
    uint32_t flags = 0;
    flags |= hookCtx.result.isStrong ? 0b10U : 0;
    flags |= hookCtx.result.isCallback ? 0b01U : 0;
    flags |= (hookCtx.result.hookChainPosition << 2U);

    auto hash = ripple::sha512Half(
        ripple::HashPrefix::emitTxnNonce,
        applyCtx.tx.getTransactionID(),
        hookCtx.emit_nonce_counter++,
        hookCtx.result.account,
        hookCtx.result.hookHash,
        flags);

    hookCtx.nonce_used[hash] = true;

    return hash;
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

    if (write_len < 32)
        return TOO_SMALL;

    auto hash = __etxn_nonce(hookCtx, applyCtx, j);

    if (!hash.has_value())
        return TOO_MANY_NONCES;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 32, hash->data(), 32, memory, memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, etxn_nonce)
{
    JS_HOOK_SETUP();

    auto hash = __etxn_nonce(hookCtx, applyCtx, j);

    if (!hash.has_value())
        returnJS(TOO_MANY_NONCES);

    std::vector<uint8_t> vec{hash->data(), hash->data() + 32};

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

    if (hookCtx.ledger_nonce_counter > hook_api::max_nonce)
        return TOO_MANY_NONCES;

    auto hash = ripple::sha512Half(
        ripple::HashPrefix::hookNonce,
        view.info().seq,
        view.info().parentCloseTime.time_since_epoch().count(),
        view.info().parentHash,
        applyCtx.tx.getTransactionID(),
        hookCtx.ledger_nonce_counter++,
        hookCtx.result.account);

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 32, hash.data(), 32, memory, memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, ledger_nonce)
{
    JS_HOOK_SETUP();

    if (hookCtx.ledger_nonce_counter > hook_api::max_nonce)
        returnJS(TOO_MANY_NONCES);

    auto hash = ripple::sha512Half(
        ripple::HashPrefix::hookNonce,
        view.info().seq,
        view.info().parentCloseTime.time_since_epoch().count(),
        view.info().parentHash,
        applyCtx.tx.getTransactionID(),
        hookCtx.ledger_nonce_counter++,
        hookCtx.result.account);

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

    // keylets must be the same type!
    if ((*klLo).type != (*klHi).type)
        return DOES_NOT_MATCH;

    std::optional<ripple::uint256> found =
        view.succ((*klLo).key, (*klHi).key.next());

    if (!found)
        return DOESNT_EXIST;

    Keylet kl_out{(*klLo).type, *found};

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

    if (klLo->type != klHi->type)
        returnJS(DOES_NOT_MATCH);

    std::optional<ripple::uint256> found =
        view.succ((*klLo).key, (*klHi).key.next());

    if (!found.has_value())
        returnJS(DOESNT_EXIST);

    Keylet kl_found{klLo->type, *found};

    auto out = ToJSIntArray(ctx, serialize_keylet_vec(kl_found));

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

    if (hookCtx.expected_etxn_count > -1)
        return ALREADY_SET;

    if (count < 1)
        return TOO_SMALL;

    if (count > hook_api::max_emit)
        return TOO_BIG;

    hookCtx.expected_etxn_count = count;

    return count;

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, etxn_reserve, JSValue raw_count)
{
    JS_HOOK_SETUP();

    if (hookCtx.expected_etxn_count > -1)
        returnJS(ALREADY_SET);

    auto count = FromJSInt(ctx, raw_count);

    if (!count.has_value() || *count < 1)
        returnJS(TOO_SMALL);

    if (*count > hook_api::max_emit)
        returnJS(TOO_BIG);

    hookCtx.expected_etxn_count = *count;

    returnJS(*count);

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCNARG(int64_t, etxn_burden)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    return __etxn_burden(hookCtx, applyCtx, j);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, etxn_burden)
{
    JS_HOOK_SETUP();

    returnJS(__etxn_burden(hookCtx, applyCtx, j));

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

    auto hash = ripple::sha512Half(ripple::Slice{memory + read_ptr, read_len});

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

    auto hash = ripple::sha512Half(ripple::Slice{vec->data(), vec->size()});

    auto ret = ToJSIntArray(ctx, hash);

    if (!ret)
        returnJS(INTERNAL_ERROR);

    return *ret;

    JS_HOOK_TEARDOWN();
}

// these are only used by get_stobject_length below
enum parse_error : int32_t {
    pe_unexpected_end = -1,
    pe_unknown_type_early = -2,  // detected early
    pe_unknown_type_late = -3,   // end of function
    pe_excessive_nesting = -4,
    pe_excessive_size = -5
};

// RH NOTE this is a light-weight stobject parsing function for drilling into a
// provided serialzied object however it could probably be replaced by an
// existing class or routine or set of routines in XRPLD Returns object length
// including header bytes (and footer bytes in the event of array or object)
// negative indicates error
inline int32_t
get_stobject_length(
    unsigned char* start,   // in - begin iterator
    unsigned char* maxptr,  // in - end iterator
    int& type,              // out - populated by serialized type code
    int& field,             // out - populated by serialized field code
    int& payload_start,  // out - the start of actual payload data for this type
    int& payload_length,  // out - the length of actual payload data for this
                          // type
    int recursion_depth = 0)  // used internally
{
    if (recursion_depth > 10)
        return pe_excessive_nesting;

    unsigned char* end = maxptr;
    unsigned char* upto = start;
    int high = *upto >> 4;
    int low = *upto & 0xF;

    upto++;
    if (upto >= end)
        return pe_unexpected_end;
    if (high > 0 && low > 0)
    {
        // common type common field
        type = high;
        field = low;
    }
    else if (high > 0)
    {
        // common type, uncommon field
        type = high;
        field = *upto++;
    }
    else if (low > 0)
    {
        // common field, uncommon type
        field = low;
        type = *upto++;
    }
    else
    {
        // uncommon type and field
        type = *upto++;
        if (upto >= end)
            return pe_unexpected_end;
        field = *upto++;
    }

    DBG_PRINTF(
        "%d get_st_object found field %d type %d\n",
        recursion_depth,
        field,
        type);

    if (upto >= end)
        return pe_unexpected_end;

    // RH TODO: link this to rippled's internal STObject constants
    // E.g.:
    /*
    int field_code = (safe_cast<int>(type) << 16) | field;
    auto const& fieldObj = ripple::SField::getField;
    */

    if (type < 1 || type > 19 || (type >= 9 && type <= 13))
        return pe_unknown_type_early;

    bool is_vl = (type == 8 /*ACCID*/ || type == 7 || type == 18 || type == 19);

    int length = -1;
    if (is_vl)
    {
        length = *upto++;
        if (upto >= end)
            return pe_unexpected_end;

        if (length < 193)
        {
            // do nothing
        }
        else if (length > 192 && length < 241)
        {
            length -= 193;
            length *= 256;
            length += *upto++ + 193;
            if (upto > end)
                return pe_unexpected_end;
        }
        else
        {
            int b2 = *upto++;
            if (upto >= end)
                return pe_unexpected_end;
            length -= 241;
            length *= 65536;
            length += 12481 + (b2 * 256) + *upto++;
            if (upto >= end)
                return pe_unexpected_end;
        }
    }
    else if ((type >= 1 && type <= 5) || type == 16 || type == 17)
    {
        length =
            (type == 1
                 ? 2
                 : (type == 2
                        ? 4
                        : (type == 3
                               ? 8
                               : (type == 4
                                      ? 16
                                      : (type == 5
                                             ? 32
                                             : (type == 16
                                                    ? 1
                                                    : (type == 17 ? 20
                                                                  : -1)))))));
    }
    else if (type == 6) /* AMOUNT */
    {
        length = (*upto >> 6 == 1) ? 8 : 48;
        if (upto >= end)
            return pe_unexpected_end;
    }

    if (length > -1)
    {
        payload_start = upto - start;
        payload_length = length;
        DBG_PRINTF(
            "%d get_stobject_length field: %d Type: %d VL: %s Len: %d "
            "Payload_Start: %d Payload_Len: %d\n",
            recursion_depth,
            field,
            type,
            (is_vl ? "yes" : "no"),
            length,
            payload_start,
            payload_length);
        return length + (upto - start);
    }

    if (type == 15 || type == 14) /* Object / Array */
    {
        payload_start = upto - start;

        for (int i = 0; i < 1024; ++i)
        {
            int subfield = -1, subtype = -1, payload_start_ = -1,
                payload_length_ = -1;
            int32_t sublength = get_stobject_length(
                upto,
                end,
                subtype,
                subfield,
                payload_start_,
                payload_length_,
                recursion_depth + 1);
            DBG_PRINTF(
                "%d get_stobject_length i %d %d-%d, upto %d sublength %d\n",
                recursion_depth,
                i,
                subtype,
                subfield,
                upto - start,
                sublength);
            if (sublength < 0)
                return pe_unexpected_end;
            upto += sublength;
            if (upto >= end)
                return pe_unexpected_end;

            if ((*upto == 0xE1U && type == 0xEU) ||
                (*upto == 0xF1U && type == 0xFU))
            {
                payload_length = upto - start - payload_start;
                upto++;
                return (upto - start);
            }
        }
        return pe_excessive_size;
    }

    return pe_unknown_type_late;
}

inline int64_t
__sto_subfield(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint8_t* start,
    size_t read_len,
    uint32_t field_id)
{
    if (read_len < 2)
        return TOO_SMALL;

    uint8_t* upto = start;
    uint8_t* end = start + read_len;

    DBG_PRINTF(
        "sto_subfield called, looking for field %u type %u\n",
        field_id & 0xFFFF,
        (field_id >> 16));
    for (int j = -5; j < 5; ++j)
        DBG_PRINTF((j == 0 ? " >%02X< " : "  %02X  "), *(start + j));
    DBG_PRINTF("\n");

    //    if ((*upto & 0xF0) == 0xE0)
    //        upto++;

    for (int i = 0; i < 1024 && upto < end; ++i)
    {
        int type = -1, field = -1, payload_start = -1, payload_length = -1;
        int32_t length = get_stobject_length(
            upto, end, type, field, payload_start, payload_length, 0);
        if (length < 0)
            return PARSE_ERROR;
        if ((type << 16) + field == field_id)
        {
            DBG_PRINTF(
                "sto_subfield returned for field %u type %u\n",
                field_id & 0xFFFF,
                (field_id >> 16));
            for (int j = -5; j < 5; ++j)
                DBG_PRINTF((j == 0 ? " [%02X] " : "  %02X  "), *(upto + j));
            DBG_PRINTF("\n");

            if (type == 0xF)  // we return arrays fully formed
                return (((int64_t)(upto - start))
                        << 32) /* start of the object */
                    + (uint32_t)(length);

            // return pointers to all other objects as payloads
            return (((int64_t)(upto - start + payload_start))
                    << 32U) /* start of the object */
                + (uint32_t)(payload_length);
        }
        upto += length;
    }

    if (upto != end)
        return PARSE_ERROR;

    return DOESNT_EXIST;
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

    unsigned char* start = (unsigned char*)(memory + read_ptr);

    return __sto_subfield(hookCtx, applyCtx, j, start, read_len, field_id);

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

    returnJS(__sto_subfield(
        hookCtx,
        applyCtx,
        j,
        sto_in->data(),
        sto_in->size(),
        (uint32_t)(*field_id)));

    JS_HOOK_TEARDOWN();
}

inline int64_t
__sto_subarray(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint8_t* start,
    size_t read_len,
    uint32_t index_id)
{
    if (read_len < 2)
        return TOO_SMALL;

    unsigned char* upto = start;
    unsigned char* end = start + read_len;

    // unwrap the array if it is wrapped,
    // by removing a byte from the start and end
    if ((*upto & 0xF0U) == 0xF0U)
    {
        upto++;
        end--;
    }

    if (upto >= end)
        return PARSE_ERROR;

    /*
    DBG_PRINTF("sto_subarray called, looking for index %u\n", index_id);
    for (int j = -5; j < 5; ++j)
        printf(( j == 0 ? " >%02X< " : "  %02X  "), *(start + j));
    DBG_PRINTF("\n");
    */
    for (int i = 0; i < 1024 && upto < end; ++i)
    {
        int type = -1, field = -1, payload_start = -1, payload_length = -1;
        int32_t length = get_stobject_length(
            upto, end, type, field, payload_start, payload_length, 0);
        if (length < 0)
            return PARSE_ERROR;

        if (i == index_id)
        {
            DBG_PRINTF("sto_subarray returned for index %u\n", index_id);
            for (int j = -5; j < 5; ++j)
                DBG_PRINTF(
                    (j == 0 ? " [%02X] " : "  %02X  "), *(upto + j + length));
            DBG_PRINTF("\n");

            return (((int64_t)(upto - start)) << 32U) /* start of the object */
                + (int64_t)(length);
        }
        upto += length;
    }

    if (upto != end)
        return PARSE_ERROR;

    return DOESNT_EXIST;
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

    unsigned char* start = (unsigned char*)(memory + read_ptr);

    return __sto_subarray(hookCtx, applyCtx, j, start, read_len, index_id);

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

    returnJS(__sto_subarray(
        hookCtx,
        applyCtx,
        j,
        sto_in->data(),
        sto_in->size(),
        (uint32_t)(*index_id)));

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

    if (read_len != 20)
        return INVALID_ARGUMENT;

    std::string raddr =
        encodeBase58Token(TokenType::AccountID, memory + read_ptr, read_len);

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

    std::string raddr =
        encodeBase58Token(TokenType::AccountID, vec->data(), 20);

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

    auto const result = decodeBase58Token(raddr, TokenType::AccountID);
    if (result.empty())
        return INVALID_ARGUMENT;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, result.data(), 20, memory, memory_length);

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

    auto const result = decodeBase58Token(*str, TokenType::AccountID);

    if (result.size() != 20)
        returnJS(INVALID_ARGUMENT);

    std::vector<uint8_t> vec(result.data(), result.data() + 20);

    if (auto ret = ToJSIntArray(ctx, vec); ret)
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

inline std::variant<int64_t, std::vector<uint8_t>>
__sto_emplace(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint8_t* sread_ptr,
    size_t sread_len,
    uint8_t* fread_ptr,
    size_t fread_len,
    uint32_t field_id)
{
    // RH TODO: put these constants somewhere (votable?)
    if (sread_len > 1024 * 16)
        return TOO_BIG;

    if (sread_len < 2)
        return TOO_SMALL;

    if (fread_len == 0 && fread_ptr == 0)
    {
        // this is a delete operation
    }
    else
    {
        if (fread_len > 4096)
            return TOO_BIG;

        if (fread_len < 2)
            return TOO_SMALL;
    }

    std::vector<uint8_t> out((size_t)(sread_len + fread_len), (uint8_t)0);
    uint8_t* write_ptr = out.data();

    // we must inject the field at the canonical location....
    // so find that location
    uint8_t* start = sread_ptr;
    uint8_t* upto = start;
    uint8_t* end = start + sread_len;
    uint8_t* inject_start = end;
    uint8_t* inject_end = end;

    DBG_PRINTF(
        "sto_emplace called, looking for field %u type %u\n",
        field_id & 0xFFFF,
        (field_id >> 16));
    for (int j = -5; j < 5; ++j)
        DBG_PRINTF((j == 0 ? " >%02X< " : "  %02X  "), *(start + j));
    DBG_PRINTF("\n");

    for (int i = 0; i < 1024 && upto < end; ++i)
    {
        int type = -1, field = -1, payload_start = -1, payload_length = -1;
        int32_t length = get_stobject_length(
            upto, end, type, field, payload_start, payload_length, 0);
        if (length < 0)
            return PARSE_ERROR;
        if ((type << 16) + field == field_id)
        {
            inject_start = upto;
            inject_end = upto + length;
            break;
        }
        else if ((type << 16) + field > field_id)
        {
            inject_start = upto;
            inject_end = upto;
            break;
        }
        upto += length;
    }

    // if the scan loop ends past the end of the source object
    // then the source object is invalid/corrupt, so we must
    // return an error
    if (upto > end)
        return PARSE_ERROR;

    // upto is injection point
    int64_t bytes_written = 0;

    // part 1
    if (inject_start - start > 0)
    {
        size_t len = inject_start - start;
        memcpy(write_ptr, start, len);
        bytes_written += len;
    }

    if (fread_len > 0)
    {
        // write the field (or don't if it's a delete operation)
        memcpy(write_ptr + bytes_written, fread_ptr, fread_len);
        bytes_written += fread_len;
    }

    // part 2
    if (end - inject_end > 0)
    {
        size_t len = end - inject_end;
        memcpy(write_ptr + bytes_written, inject_end, len);
        bytes_written += len;
    }

    out.resize(bytes_written);
    return out;
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

    auto out = __sto_emplace(
        hookCtx,
        applyCtx,
        j,
        memory + sread_ptr,
        sread_len,
        fread_ptr ? memory + fread_ptr : nullptr,
        fread_len,
        field_id);

    if (std::holds_alternative<int64_t>(out))
        return std::get<int64_t>(out);

    // otherwise it's returned a vec<u8>

    std::vector<uint8_t> const& vec = std::get<std::vector<uint8_t>>(out);

    if (vec.size() > write_len)
        return INTERNAL_ERROR;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, vec.data(), vec.size(), memory, memory_length);

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

    auto ret = __sto_emplace(
        hookCtx,
        applyCtx,
        j,
        sto->data(),
        sto->size(),
        isErase ? nullptr : field->data(),
        isErase ? 0 : field->size(),
        *field_id);

    if (std::holds_alternative<int64_t>(ret))
        returnJS(std::get<int64_t>(ret));

    // otherwise it's returned a vec<u8>

    std::vector<uint8_t> const& vec = std::get<std::vector<uint8_t>>(ret);

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

inline int64_t
__sto_validate(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint8_t* read_ptr,
    size_t read_len)
{
    if (read_len < 2)
        return TOO_SMALL;

    unsigned char* start = read_ptr;
    unsigned char* upto = start;
    unsigned char* end = start + read_len;

    for (int i = 0; i < 1024 && upto < end; ++i)
    {
        int type = -1, field = -1, payload_start = -1, payload_length = -1;
        int32_t length = get_stobject_length(
            upto, end, type, field, payload_start, payload_length, 0);
        if (length < 0)
            return 0;
        upto += length;
    }

    return upto == end ? 1 : 0;
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

    return __sto_validate(hookCtx, applyCtx, j, read_ptr + memory, read_len);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, sto_validate, JSValue raw_sto)
{
    JS_HOOK_SETUP();

    auto sto = FromJSIntArrayOrHexString(ctx, raw_sto, 16 * 1024);

    if (!sto.has_value())
        returnJS(INVALID_ARGUMENT);

    returnJS(__sto_validate(hookCtx, applyCtx, j, sto->data(), sto->size()));

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

    if (kread_len != 33)
        return INVALID_KEY;

    if (dread_len == 0)
        return TOO_SMALL;

    if (sread_len < 30)
        return TOO_SMALL;

    ripple::Slice keyslice{
        reinterpret_cast<const void*>(kread_ptr + memory), kread_len};
    ripple::Slice data{
        reinterpret_cast<const void*>(dread_ptr + memory), dread_len};
    ripple::Slice sig{
        reinterpret_cast<const void*>(sread_ptr + memory), sread_len};

    if (!publicKeyType(keyslice))
        return INVALID_KEY;

    ripple::PublicKey key{keyslice};
    return verify(key, data, sig, false) ? 1 : 0;

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

    if (!publicKeyType(keyslice))
        returnJS(INVALID_KEY);

    ripple::PublicKey key{keyslice};
    returnJS(verify(key, data, sig, false) ? 1 : 0);

    JS_HOOK_TEARDOWN();
}

// Return the current fee base of the current ledger (multiplied by a margin)
DEFINE_WASM_FUNCNARG(int64_t, fee_base)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    return view.fees().base.drops();

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, fee_base)
{
    JS_HOOK_SETUP();

    returnJS(view.fees().base.drops());

    JS_HOOK_TEARDOWN();
}

// Return the fee base for a hypothetically emitted transaction from the current
// hook based on byte count
inline int64_t
__etxn_fee_base(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint8_t* read_ptr,
    size_t read_len)
{
    if (hookCtx.expected_etxn_count <= -1)
        return PREREQUISITE_NOT_MET;

    try
    {
        ripple::Slice tx{reinterpret_cast<const void*>(read_ptr), read_len};

        SerialIter sitTrans(tx);

        std::unique_ptr<STTx const> stpTrans;
        stpTrans = std::make_unique<STTx const>(std::ref(sitTrans));

        return Transactor::calculateBaseFee(
                   *(applyCtx.app.openLedger().current()), *stpTrans)
            .drops();
    }
    catch (std::exception& e)
    {
        JLOG(j.trace()) << "HookInfo[" << HC_ACC()
                        << "]: etxn_fee_base exception: " << e.what();
        return INVALID_TXN;
    }
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

    return __etxn_fee_base(hookCtx, applyCtx, j, memory + read_ptr, read_len);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, etxn_fee_base, JSValue txblob)
{
    JS_HOOK_SETUP();

    auto blob = FromJSIntArrayOrHexString(ctx, txblob, 65536);
    if (!blob.has_value() || blob->empty())
        returnJS(INVALID_ARGUMENT);

    returnJS(__etxn_fee_base(hookCtx, applyCtx, j, blob->data(), blob->size()));

    JS_HOOK_TEARDOWN();
}

// Populate an sfEmitDetails field in a soon-to-be emitted transaction
inline int64_t
__etxn_details(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint8_t* out_ptr,
    size_t max_len)
{
    int64_t expected_size = 138U;
    if (!hookCtx.result.hasCallback)
        expected_size -= 22U;

    if (max_len < expected_size)
        return TOO_SMALL;

    if (hookCtx.expected_etxn_count <= -1)
        return PREREQUISITE_NOT_MET;

    uint32_t generation = __etxn_generation(hookCtx, applyCtx, j);

    int64_t burden = __etxn_burden(hookCtx, applyCtx, j);

    if (burden < 1)
        return FEE_TOO_LARGE;

    uint8_t* out = out_ptr;

    *out++ = 0xEDU;  // begin sfEmitDetails                            /* upto =
                     // 0 | size =  1 */
    *out++ = 0x20U;  // sfEmitGeneration preamble                      /* upto =
                     // 1 | size =  6 */
    *out++ = 0x2EU;  // preamble cont
    *out++ = (generation >> 24U) & 0xFFU;
    *out++ = (generation >> 16U) & 0xFFU;
    *out++ = (generation >> 8U) & 0xFFU;
    *out++ = (generation >> 0U) & 0xFFU;
    *out++ = 0x3DU;  // sfEmitBurden preamble                           /* upto
                     // =   7 | size =  9 */
    *out++ = (burden >> 56U) & 0xFFU;
    *out++ = (burden >> 48U) & 0xFFU;
    *out++ = (burden >> 40U) & 0xFFU;
    *out++ = (burden >> 32U) & 0xFFU;
    *out++ = (burden >> 24U) & 0xFFU;
    *out++ = (burden >> 16U) & 0xFFU;
    *out++ = (burden >> 8U) & 0xFFU;
    *out++ = (burden >> 0U) & 0xFFU;
    *out++ = 0x5BU;  // sfEmitParentTxnID preamble                      /* upto
                     // =  16 | size = 33 */
    auto const& txID = applyCtx.tx.getTransactionID();
    memcpy(out, txID.data(), 32);
    out += 32;
    *out++ = 0x5CU;  // sfEmitNonce                                     /* upto
                     // =  49 | size = 33 */

    auto hash = __etxn_nonce(hookCtx, applyCtx, j);
    if (!hash.has_value())
        return INTERNAL_ERROR;

    memcpy(out, hash->data(), 32);

    out += 32;
    *out++ = 0x5DU;  // sfEmitHookHash preamble                          /* upto
                     // =  82 | size = 33 */
    for (int i = 0; i < 32; ++i)
        *out++ = hookCtx.result.hookHash.data()[i];

    if (hookCtx.result.hasCallback)
    {
        *out++ = 0x8AU;  // sfEmitCallback preamble                         /*
                         // upto = 115 | size = 22 */
        *out++ = 0x14U;  // preamble cont

        memcpy(out, hookCtx.result.account.data(), 20);

        out += 20;
    }
    *out++ = 0xE1U;  // end object (sfEmitDetails)                     /* upto =
                     // 137 | size =  1 */
                     /* upto = 138 | --------- */
    int64_t outlen = out - out_ptr;

    DBG_PRINTF("emitdetails size = %d\n", outlen);
    return outlen;
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

    return __etxn_details(hookCtx, applyCtx, j, memory + write_ptr, write_len);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, etxn_details)
{
    JS_HOOK_SETUP();

    uint8_t out[138];

    int64_t retval = __etxn_details(hookCtx, applyCtx, j, out, 138);

    if (retval <= 0)
        returnJS(retval);

    if (retval > 138)
        returnJS(INTERNAL_ERROR);

    auto outjs = ToJSIntArray(ctx, std::vector<uint8_t>{out, out + retval});

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

    if (mantissa == 0)
        return 0;

    int64_t normalized = hook_float::normalize_xfl(mantissa, exp);

    // the above function will underflow into a canonical 0
    // but this api must report that underflow
    if (normalized == 0 || normalized == XFL_OVERFLOW)
        return INVALID_FLOAT;

    return normalized;

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

    if (mantissa == 0)
        returnJS(0);

    int64_t normalized = hook_float::normalize_xfl(mantissa, exp);

    // the above function will underflow into a canonical 0
    // but this api must report that underflow
    if (normalized == 0 || normalized == XFL_OVERFLOW)
        returnJS(INVALID_FLOAT);

    returnJSXFL(normalized);

    JS_HOOK_TEARDOWN();
}

inline int64_t
mulratio_internal(
    int64_t& man1,
    int32_t& exp1,
    bool round_up,
    uint32_t numerator,
    uint32_t denominator)
{
    try
    {
        ripple::IOUAmount amt{man1, exp1};
        ripple::IOUAmount out = ripple::mulRatio(
            amt, numerator, denominator, round_up != 0);  // already normalized
        man1 = out.mantissa();
        exp1 = out.exponent();
        return 1;
    }
    catch (std::overflow_error& e)
    {
        return XFL_OVERFLOW;
    }
}

inline int64_t
float_multiply_internal_parts(
    uint64_t man1,
    int32_t exp1,
    bool neg1,
    uint64_t man2,
    int32_t exp2,
    bool neg2)
{
    using namespace boost::multiprecision;
    cpp_int mult = cpp_int(man1) * cpp_int(man2);
    mult /= power_of_ten[15];
    uint64_t man_out = static_cast<uint64_t>(mult);
    if (mult > man_out)
        return XFL_OVERFLOW;

    int32_t exp_out = exp1 + exp2 + 15;
    bool neg_out = (neg1 && !neg2) || (!neg1 && neg2);
    int64_t ret = normalize_xfl(man_out, exp_out, neg_out);

    if (ret == EXPONENT_UNDERSIZED)
        return 0;
    if (ret == EXPONENT_OVERSIZED)
        return XFL_OVERFLOW;
    return ret;
}

inline int64_t
__float_int(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    int64_t float1,
    uint32_t decimal_places,
    uint32_t absolute)
{
    RETURN_IF_INVALID_FLOAT(float1);
    if (float1 == 0)
        return 0;
    uint64_t man1 = get_mantissa(float1);
    int32_t exp1 = get_exponent(float1);
    bool neg1 = is_negative(float1);

    if (decimal_places > 15)
        return INVALID_ARGUMENT;

    if (neg1)
    {
        if (!absolute)
            return CANT_RETURN_NEGATIVE;
    }

    int32_t shift = -(exp1 + decimal_places);

    if (shift > 15)
        return 0;

    if (shift < 0)
        return TOO_BIG;

    if (shift > 0)
        man1 /= power_of_ten[shift];

    return man1;
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
    return __float_int(hookCtx, applyCtx, j, float1, decimal_places, absolute);

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

    // Note! This is the only (?) place where a float/... method has to return
    // `returnJS` instead of `returnJSXFL` as this is where we cast to JS usable
    // Number
    returnJS(__float_int(
        hookCtx, applyCtx, j, *f1, (uint32_t)(*dp), (uint32_t)(*ab)));

    JS_HOOK_TEARDOWN();
}

inline int64_t
__float_multiply(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    int64_t float1,
    int64_t float2)
{
    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    if (float1 == 0 || float2 == 0)
        return 0;

    uint64_t man1 = get_mantissa(float1);
    int32_t exp1 = get_exponent(float1);
    bool neg1 = is_negative(float1);
    uint64_t man2 = get_mantissa(float2);
    int32_t exp2 = get_exponent(float2);
    bool neg2 = is_negative(float2);

    return float_multiply_internal_parts(man1, exp1, neg1, man2, exp2, neg2);
}

DEFINE_WASM_FUNCTION(int64_t, float_multiply, int64_t float1, int64_t float2)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    return __float_multiply(hookCtx, applyCtx, j, float1, float2);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_multiply, JSValue raw_f1, JSValue raw_f2)
{
    JS_HOOK_SETUP();

    auto f1 = FromJSInt(ctx, raw_f1);
    auto f2 = FromJSInt(ctx, raw_f2);

    if (any_missing(f1, f2))
        returnJS(INVALID_ARGUMENT);

    int64_t const out = __float_multiply(hookCtx, applyCtx, j, *f1, *f2);
    if (out < 0)
        returnJS(out);

    returnJSXFL(out);

    JS_HOOK_TEARDOWN();
}

inline int64_t
__float_mulratio(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    int64_t float1,
    uint32_t round_up,
    uint32_t numerator,
    uint32_t denominator)
{
    RETURN_IF_INVALID_FLOAT(float1);
    if (float1 == 0)
        return 0;
    if (denominator == 0)
        return DIVISION_BY_ZERO;

    int64_t man1 = (int64_t)get_mantissa(float1);
    int32_t exp1 = get_exponent(float1);

    if (mulratio_internal(man1, exp1, round_up > 0, numerator, denominator) < 0)
        return XFL_OVERFLOW;

    // defensive check
    if (man1 < 0)
        man1 *= -1LL;

    return make_float((uint64_t)man1, exp1, is_negative(float1));
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

    return __float_mulratio(
        hookCtx, applyCtx, j, float1, round_up, numerator, denominator);

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

    int64_t const out = __float_mulratio(
        hookCtx,
        applyCtx,
        j,
        *f1,
        (uint32_t)(*ru),
        (uint32_t)(*n),
        (uint32_t)(*d));
    if (out < 0)
        returnJS(out);

    returnJSXFL(out);

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_negate, int64_t float1)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (float1 == 0)
        return 0;
    RETURN_IF_INVALID_FLOAT(float1);
    return hook_float::invert_sign(float1);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_negate, JSValue raw_f1)
{
    JS_HOOK_SETUP();

    auto float1 = FromJSInt(ctx, raw_f1);
    if (!float1.has_value())
        returnJS(INVALID_ARGUMENT);

    if (*float1 == 0)
        returnJS(0);
    RETURNJS_IF_INVALID_FLOAT(*float1);
    returnJSXFL(hook_float::invert_sign(*float1));

    JS_HOOK_TEARDOWN();
}

inline int64_t
__float_compare(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    int64_t float1,
    int64_t float2,
    uint32_t mode)
{
    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    bool equal_flag = mode & compare_mode::EQUAL;
    bool less_flag = mode & compare_mode::LESS;
    bool greater_flag = mode & compare_mode::GREATER;
    bool not_equal = less_flag && greater_flag;

    if ((equal_flag && less_flag && greater_flag) || mode == 0)
        return INVALID_ARGUMENT;

    if (mode & (~0b111UL))
        return INVALID_ARGUMENT;

    try
    {
        int64_t man1 = (int64_t)(get_mantissa(float1)) *
            (is_negative(float1) ? -1LL : 1LL);
        int32_t exp1 = get_exponent(float1);
        ripple::IOUAmount amt1{man1, exp1};
        int64_t man2 = (int64_t)(get_mantissa(float2)) *
            (is_negative(float2) ? -1LL : 1LL);
        int32_t exp2 = get_exponent(float2);
        ripple::IOUAmount amt2{man2, exp2};

        if (not_equal && amt1 != amt2)
            return 1;

        if (equal_flag && amt1 == amt2)
            return 1;

        if (greater_flag && amt1 > amt2)
            return 1;

        if (less_flag && amt1 < amt2)
            return 1;

        return 0;
    }
    catch (std::overflow_error& e)
    {
        return XFL_OVERFLOW;
    }
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
    return __float_compare(hookCtx, applyCtx, j, float1, float2, mode);

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

    returnJS(
        __float_compare(hookCtx, applyCtx, j, *f1, *f2, (uint32_t)(*mode)));

    JS_HOOK_TEARDOWN();
}

inline int64_t
__float_sum(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    int64_t float1,
    int64_t float2)
{
    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    if (float1 == 0)
        return float2;
    if (float2 == 0)
        return float1;

    int64_t man1 =
        (int64_t)(get_mantissa(float1)) * (is_negative(float1) ? -1LL : 1LL);
    int32_t exp1 = get_exponent(float1);
    int64_t man2 =
        (int64_t)(get_mantissa(float2)) * (is_negative(float2) ? -1LL : 1LL);
    int32_t exp2 = get_exponent(float2);

    try
    {
        ripple::IOUAmount amt1{man1, exp1};
        ripple::IOUAmount amt2{man2, exp2};
        amt1 += amt2;
        int64_t result = make_float(amt1);
        if (result == EXPONENT_UNDERSIZED)
        {
            // this is an underflow e.g. as a result of subtracting an xfl from
            // itself and thus not an error, just return canonical 0
            return 0;
        }
        return result;
    }
    catch (std::overflow_error& e)
    {
        return XFL_OVERFLOW;
    }
}

DEFINE_WASM_FUNCTION(int64_t, float_sum, int64_t float1, int64_t float2)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack
    return __float_sum(hookCtx, applyCtx, j, float1, float2);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_sum, JSValue raw_f1, JSValue raw_f2)
{
    JS_HOOK_SETUP();

    auto [f1, f2] = FromJSInts(ctx, raw_f1, raw_f2);
    if (any_missing(f1, f2))
        returnJS(INVALID_ARGUMENT);

    returnJSXFL(__float_sum(hookCtx, applyCtx, j, *f1, *f2));

    JS_HOOK_TEARDOWN();
}

inline std::variant<int64_t, std::vector<uint8_t>>
__float_sto(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint32_t write_len,
    std::optional<Currency> currency,
    std::optional<AccountID> issuer,
    int64_t float1,
    uint32_t field_code)
{
    RETURN_IF_INVALID_FLOAT(float1);

    uint16_t field = field_code & 0xFFFFU;
    uint16_t type = field_code >> 16U;

    bool is_xrp = field_code == 0;
    bool is_short =
        field_code == 0xFFFFFFFFU;  // non-xrp value but do not output header or
                                    // tail, just amount

    int bytes_needed = 8 +
        (field == 0 && type == 0
             ? 0
             : (field == 0xFFFFU && type == 0xFFFFU
                    ? 0
                    : (field < 16 && type < 16
                           ? 1
                           : (field >= 16 && type < 16
                                  ? 2
                                  : (field < 16 && type >= 16 ? 2 : 3)))));

    int64_t bytes_written = 0;

    if (issuer && !currency)
        return INVALID_ARGUMENT;

    if (!issuer && currency)
        return INVALID_ARGUMENT;

    if (issuer)
    {
        if (is_xrp)
            return INVALID_ARGUMENT;
        if (is_short)
            return INVALID_ARGUMENT;

        bytes_needed += 40;
    }
    else if (!is_xrp && !is_short)
        return INVALID_ARGUMENT;

    if (bytes_needed > write_len)
        return TOO_SMALL;

    std::vector<uint8_t> vec(bytes_needed);
    uint8_t* write_ptr = vec.data();

    if (is_xrp || is_short)
    {
        // do nothing
    }
    else if (field < 16 && type < 16)
    {
        *write_ptr++ = (((uint8_t)type) << 4U) + ((uint8_t)field);
    }
    else if (field >= 16 && type < 16)
    {
        *write_ptr++ = (((uint8_t)type) << 4U);
        *write_ptr++ = ((uint8_t)field);
    }
    else if (field < 16 && type >= 16)
    {
        *write_ptr++ = (((uint8_t)field) << 4U);
        *write_ptr++ = ((uint8_t)type);
    }
    else
    {
        *write_ptr++ = 0;
        *write_ptr++ = ((uint8_t)type);
        *write_ptr++ = ((uint8_t)field);
    }

    uint64_t man = get_mantissa(float1);
    int32_t exp = get_exponent(float1);
    bool neg = is_negative(float1);
    uint8_t out[8];
    if (is_xrp)
    {
        int32_t shift = -(exp);

        if (shift > 15)
            return 0;

        if (shift < 0)
            return XFL_OVERFLOW;

        if (shift > 0)
            man /= power_of_ten[shift];

        out[0] = (neg ? 0b00000000U : 0b01000000U);
        out[0] += (uint8_t)((man >> 56U) & 0b111111U);
        out[1] = (uint8_t)((man >> 48U) & 0xFF);
        out[2] = (uint8_t)((man >> 40U) & 0xFF);
        out[3] = (uint8_t)((man >> 32U) & 0xFF);
        out[4] = (uint8_t)((man >> 24U) & 0xFF);
        out[5] = (uint8_t)((man >> 16U) & 0xFF);
        out[6] = (uint8_t)((man >> 8U) & 0xFF);
        out[7] = (uint8_t)((man >> 0U) & 0xFF);
    }
    else if (man == 0)
    {
        out[0] = 0b10000000U;
        for (int i = 1; i < 8; ++i)
            out[i] = 0;
    }
    else
    {
        exp += 97;

        /// encode the rippled floating point sto format

        out[0] = (neg ? 0b10000000U : 0b11000000U);
        out[0] += (uint8_t)(exp >> 2U);
        out[1] = ((uint8_t)(exp & 0b11U)) << 6U;
        out[1] += (((uint8_t)(man >> 48U)) & 0b111111U);
        out[2] = (uint8_t)((man >> 40U) & 0xFFU);
        out[3] = (uint8_t)((man >> 32U) & 0xFFU);
        out[4] = (uint8_t)((man >> 24U) & 0xFFU);
        out[5] = (uint8_t)((man >> 16U) & 0xFFU);
        out[6] = (uint8_t)((man >> 8U) & 0xFFU);
        out[7] = (uint8_t)((man >> 0U) & 0xFFU);
    }

    std::memcpy(write_ptr, out, 8);
    write_ptr += 8;

    if (!is_xrp && !is_short)
    {
        std::memcpy(write_ptr, currency->data(), 20);
        write_ptr += 20;
        std::memcpy(write_ptr, issuer->data(), 20);
    }

    return vec;
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

    // error checking preserved here, in the same order to avoid a fork
    // condition

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

    auto ret = __float_sto(
        hookCtx, applyCtx, j, write_len, currency, issuer, float1, field_code);

    if (std::holds_alternative<int64_t>(ret))
        return std::get<int64_t>(ret);

    std::vector<uint8_t> const& vec = std::get<std::vector<uint8_t>>(ret);
    if (vec.size() > write_len)
        return TOO_SMALL;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, vec.data(), vec.size(), memory, memory_length);

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

    if (!cur.has_value() && !JS_IsUndefined(raw_cur))
        returnJS(INVALID_ARGUMENT);

    if (!isu.has_value() && !JS_IsUndefined(raw_isu))
        returnJS(INVALID_ARGUMENT);

    if (any_missing(f1, fc) || !fits_u32(fc))
        returnJS(INVALID_ARGUMENT);

    std::optional<Currency> currency;
    std::optional<AccountID> issuer;
    if (cur.has_value())
        currency = parseCurrency(cur->data(), cur->size());
    if (isu.has_value())
        issuer = AccountID::fromVoid(isu->data());

    // RH TODO: 64 is wrong, figure out the longest this can be (< 64)
    auto ret =
        __float_sto(hookCtx, applyCtx, j, 64, currency, issuer, *f1, *fc);

    if (std::holds_alternative<int64_t>(ret))
        returnJS(std::get<int64_t>(ret));

    std::vector<uint8_t> const& vec = std::get<std::vector<uint8_t>>(ret);
    if (vec.size() > 64)
        returnJS(INTERNAL_ERROR);

    auto out = ToJSIntArray(ctx, vec);
    if (!out.has_value())
        returnJS(INTERNAL_ERROR);
    return *out;

    JS_HOOK_TEARDOWN();
}

inline int64_t
__float_sto_set(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint8_t* read_ptr,
    uint32_t read_len)
{
    if (read_len < 8)
        return NOT_AN_OBJECT;

    uint8_t* upto = read_ptr;

    if (read_len > 8)
    {
        uint8_t hi = *upto >> 4U;
        uint8_t lo = *upto & 0xFU;

        if (hi == 0 && lo == 0)
        {
            // typecode >= 16 && fieldcode >= 16
            if (read_len < 11)
                return NOT_AN_OBJECT;
            upto += 3;
            read_len -= 3;
        }
        else if (hi == 0 || lo == 0)
        {
            // typecode >= 16 && fieldcode < 16
            if (read_len < 10)
                return NOT_AN_OBJECT;
            upto += 2;
            read_len -= 2;
        }
        else
        {
            // typecode < 16 && fieldcode < 16
            upto++;
            read_len--;
        }
    }

    if (read_len < 8)
        return NOT_AN_OBJECT;

    bool is_xrp = (((*upto) & 0b10000000U) == 0);
    bool is_negative = (((*upto) & 0b01000000U) == 0);

    int32_t exponent = 0;

    if (is_xrp)
    {
        // exponent remains 0
        upto++;
    }
    else
    {
        exponent = (((*upto++) & 0b00111111U)) << 2U;
        exponent += ((*upto) >> 6U);
        exponent -= 97;
    }

    uint64_t mantissa = (((uint64_t)(*upto++)) & 0b00111111U) << 48U;
    mantissa += ((uint64_t)*upto++) << 40U;
    mantissa += ((uint64_t)*upto++) << 32U;
    mantissa += ((uint64_t)*upto++) << 24U;
    mantissa += ((uint64_t)*upto++) << 16U;
    mantissa += ((uint64_t)*upto++) << 8U;
    mantissa += ((uint64_t)*upto++);

    if (mantissa == 0)
        return 0;

    return hook_float::normalize_xfl(mantissa, exponent, is_negative);
}

DEFINE_WASM_FUNCTION(
    int64_t,
    float_sto_set,
    uint32_t read_ptr,
    uint32_t read_len)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (read_len < 8)
        return NOT_AN_OBJECT;

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    return __float_sto_set(hookCtx, applyCtx, j, memory + read_ptr, read_len);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_sto_set, JSValue raw_sto)
{
    JS_HOOK_SETUP();

    auto sto = FromJSIntArrayOrHexString(ctx, raw_sto, 0x10000);

    if (!sto.has_value())
        returnJS(INVALID_ARGUMENT);

    int64_t const out =
        __float_sto_set(hookCtx, applyCtx, j, sto->data(), sto->size());
    if (out < 0)
        returnJS(out);

    returnJSXFL(out);

    JS_HOOK_TEARDOWN();
}

const int64_t float_one_internal = make_float(1000000000000000ull, -15, false);

inline int64_t
float_divide_internal(int64_t float1, int64_t float2, bool hasFix)
{
    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);
    if (float2 == 0)
        return DIVISION_BY_ZERO;
    if (float1 == 0)
        return 0;

    // special case: division by 1
    // RH TODO: add more special cases (division by power of 10)
    if (float2 == float_one_internal)
        return float1;

    uint64_t man1 = get_mantissa(float1);
    int32_t exp1 = get_exponent(float1);
    bool neg1 = is_negative(float1);
    uint64_t man2 = get_mantissa(float2);
    int32_t exp2 = get_exponent(float2);
    bool neg2 = is_negative(float2);

    int64_t tmp1 = normalize_xfl(man1, exp1);
    int64_t tmp2 = normalize_xfl(man2, exp2);

    if (tmp1 < 0 || tmp2 < 0)
        return INVALID_FLOAT;

    if (tmp1 == 0)
        return 0;

    while (man2 > man1)
    {
        man2 /= 10;
        exp2++;
    }

    if (man2 == 0)
        return DIVISION_BY_ZERO;

    while (man2 < man1)
    {
        if (man2 * 10 > man1)
            break;
        man2 *= 10;
        exp2--;
    }

    uint64_t man3 = 0;
    int32_t exp3 = exp1 - exp2;

    while (man2 > 0)
    {
        int i = 0;
        if (hasFix)
        {
            for (; man1 >= man2; man1 -= man2, ++i)
                ;
        }
        else
        {
            for (; man1 > man2; man1 -= man2, ++i)
                ;
        }

        man3 *= 10;
        man3 += i;
        man2 /= 10;
        if (man2 == 0)
            break;
        exp3--;
    }

    bool neg3 = !((neg1 && neg2) || (!neg1 && !neg2));

    return normalize_xfl(man3, exp3, neg3);
}

DEFINE_WASM_FUNCTION(int64_t, float_divide, int64_t float1, int64_t float2)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    bool const hasFix = view.rules().enabled(fixFloatDivide);
    return float_divide_internal(float1, float2, hasFix);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_divide, JSValue raw_f1, JSValue raw_f2)
{
    JS_HOOK_SETUP();

    auto [f1, f2] = FromJSInts(ctx, raw_f1, raw_f2);
    if (any_missing(f1, f2))
        returnJS(INVALID_ARGUMENT);

    bool const fixV3 = view.rules().enabled(fixFloatDivide);
    int64_t const out = float_divide_internal(*f1, *f2, fixV3);
    if (out < 0)
        returnJS(out);

    returnJSXFL(out);

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCNARG(int64_t, float_one)
{
    return float_one_internal;
}

DEFINE_JS_FUNCNARG(JSValue, float_one)
{
    JS_HOOK_SETUP();

    returnJSXFL(float_one_internal);

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_invert, int64_t float1)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    if (float1 == 0)
        return DIVISION_BY_ZERO;
    if (float1 == float_one_internal)
        return float_one_internal;

    bool const fixV3 = view.rules().enabled(fixFloatDivide);
    return float_divide_internal(float_one_internal, float1, fixV3);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_invert, JSValue raw_f1)
{
    JS_HOOK_SETUP();

    auto f1 = FromJSInt(ctx, raw_f1);
    if (!f1.has_value())
        returnJS(INVALID_ARGUMENT);

    if (*f1 == 0)
        returnJS(DIVISION_BY_ZERO);
    if (*f1 == float_one_internal)
        returnJSXFL(float_one_internal);

    bool const fixV3 = view.rules().enabled(fixFloatDivide);
    int64_t const out = float_divide_internal(float_one_internal, *f1, fixV3);
    if (out < 0)
        returnJS(out);

    returnJSXFL(out);

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_mantissa, int64_t float1)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);
    if (float1 == 0)
        return 0;
    return get_mantissa(float1);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_mantissa, JSValue raw_f1)
{
    JS_HOOK_SETUP();

    auto f1 = FromJSInt(ctx, raw_f1);
    if (!f1.has_value())
        returnJS(INVALID_ARGUMENT);

    RETURNJS_IF_INVALID_FLOAT(*f1);

    if (*f1 == 0)
        returnJS(0);

    int64_t const out = get_mantissa(*f1);
    if (out < 0)
        returnJS(out);

    returnJSXFL(out);

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCTION(int64_t, float_sign, int64_t float1)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);
    if (float1 == 0)
        return 0;
    return is_negative(float1);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_sign, JSValue raw_f1)
{
    JS_HOOK_SETUP();

    auto f1 = FromJSInt(ctx, raw_f1);
    if (!f1.has_value())
        returnJS(INVALID_ARGUMENT);

    RETURNJS_IF_INVALID_FLOAT(*f1);

    if (*f1 == 0)
        returnJS(0);

    returnJS(is_negative(*f1));

    JS_HOOK_TEARDOWN();
}

inline int64_t
double_to_xfl(double x)
{
    if ((x) == 0)
        return 0;
    bool neg = x < 0;
    double absresult = neg ? -x : x;

    // first compute the base 10 order of the float
    int32_t exp_out = (int32_t)log10(absresult);

    // next adjust it into the valid mantissa range (this means dividing by its
    // order and multiplying by 10**15)
    absresult *= pow(10, -exp_out + 15);

    // after adjustment the value may still fall below the minMantissa
    int64_t result = (int64_t)absresult;
    if (result < minMantissa)
    {
        if (result == minMantissa - 1LL)
            result += 1LL;
        else
        {
            result *= 10LL;
            exp_out--;
        }
    }

    // likewise the value can fall above the maxMantissa
    if (result > maxMantissa)
    {
        if (result == maxMantissa + 1LL)
            result -= 1LL;
        else
        {
            result /= 10LL;
            exp_out++;
        }
    }

    exp_out -= 15;
    int64_t ret = make_float(result, exp_out, neg);

    if (ret == EXPONENT_UNDERSIZED)
        return 0;

    return ret;
}

inline int64_t
__float_log(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    int64_t float1)
{
    RETURN_IF_INVALID_FLOAT(float1);

    if (float1 == 0)
        return INVALID_ARGUMENT;

    uint64_t man1 = get_mantissa(float1);
    int32_t exp1 = get_exponent(float1);
    if (is_negative(float1))
        return COMPLEX_NOT_SUPPORTED;

    double inp = (double)(man1);
    double result = log10(inp) + exp1;

    return double_to_xfl(result);
}

DEFINE_WASM_FUNCTION(int64_t, float_log, int64_t float1)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    return __float_log(hookCtx, applyCtx, j, float1);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_log, JSValue raw_f1)
{
    JS_HOOK_SETUP();

    auto f1 = FromJSInt(ctx, raw_f1);
    if (!f1.has_value())
        returnJS(INVALID_ARGUMENT);

    int64_t const out = __float_log(hookCtx, applyCtx, j, *f1);
    if (out < 0)
        returnJS(out);

    returnJSXFL(out);

    JS_HOOK_TEARDOWN();
}

inline int64_t
__float_root(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    int64_t float1,
    uint32_t n)
{
    RETURN_IF_INVALID_FLOAT(float1);
    if (float1 == 0)
        return 0;

    if (n < 2)
        return INVALID_ARGUMENT;

    uint64_t man1 = get_mantissa(float1);
    int32_t exp1 = get_exponent(float1);
    if (is_negative(float1))
        return COMPLEX_NOT_SUPPORTED;

    double inp = (double)(man1)*pow(10, exp1);
    double result = pow(inp, ((double)1.0f) / ((double)(n)));

    return double_to_xfl(result);
}

DEFINE_WASM_FUNCTION(int64_t, float_root, int64_t float1, uint32_t n)
{
    WASM_HOOK_SETUP();  // populates memory_ctx, memory, memory_length,
                        // applyCtx, hookCtx on current stack

    return __float_root(hookCtx, applyCtx, j, float1, n);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, float_root, JSValue raw_f1, JSValue raw_n)
{
    JS_HOOK_SETUP();

    auto [f1, n] = FromJSInts(ctx, raw_f1, raw_n);
    if (any_missing(f1, n) || !fits_u32(n))
        returnJS(INVALID_ARGUMENT);

    int64_t out = __float_root(hookCtx, applyCtx, j, *f1, (uint32_t)(*n));
    if (out < 0)
        returnJS(out);

    returnJSXFL(out);

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

    if (read_len < 1)
        return TOO_SMALL;

    if (read_len > 32)
        return TOO_BIG;

    if (!applyCtx.tx.isFieldPresent(sfHookParameters))
        return DOESNT_EXIST;

    std::vector<uint8_t> paramName{
        read_ptr + memory, read_ptr + read_len + memory};

    auto const& params = applyCtx.tx.getFieldArray(sfHookParameters);

    for (auto const& param : params)
    {
        if (!param.isFieldPresent(sfHookParameterName) ||
            param.getFieldVL(sfHookParameterName) != paramName)
            continue;

        if (!param.isFieldPresent(sfHookParameterValue))
            return DOESNT_EXIST;

        auto const& val = param.getFieldVL(sfHookParameterValue);
        if (val.empty())
            return DOESNT_EXIST;

        if (val.size() > write_len)
            return TOO_SMALL;

        WRITE_WASM_MEMORY_AND_RETURN(
            write_ptr,
            write_len,
            val.data(),
            val.size(),
            memory,
            memory_length);
    }

    return DOESNT_EXIST;

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

    auto const& params = applyCtx.tx.getFieldArray(sfHookParameters);

    for (auto const& param : params)
    {
        if (!param.isFieldPresent(sfHookParameterName) ||
            param.getFieldVL(sfHookParameterName) != paramName)
            continue;

        if (!param.isFieldPresent(sfHookParameterValue))
            returnJS(DOESNT_EXIST);

        auto const& val = param.getFieldVL(sfHookParameterValue);
        if (val.empty())
            returnJS(DOESNT_EXIST);

        if (val.size() > hook::maxHookParameterValueSize())
            returnJS(TOO_BIG);

        auto out = ToJSIntArray(ctx, val);
        if (!out.has_value())
            returnJS(INTERNAL_ERROR);

        return *out;
    }

    returnJS(DOESNT_EXIST);

    JS_HOOK_TEARDOWN();
}

inline std::optional<std::vector<uint8_t>>
__hook_param(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    std::vector<uint8_t> const& paramName)
{
    // first check for overrides set by prior hooks in the chain
    auto const& overrides = hookCtx.result.hookParamOverrides;
    if (overrides.find(hookCtx.result.hookHash) != overrides.end())
    {
        auto const& params = overrides.at(hookCtx.result.hookHash);
        if (params.find(paramName) != params.end())
        {
            auto const& param = params.at(paramName);
            if (param.size() == 0)
                return {};  // allow overrides to "delete" parameters

            return std::vector<uint8_t>{
                param.data(), param.data() + param.size()};
        }
    }

    // next check if there's a param set on this hook
    auto const& params = hookCtx.result.hookParams;
    if (params.find(paramName) != params.end())
    {
        auto const& param = params.at(paramName);
        if (param.size() == 0)
            return {};

        return std::vector<uint8_t>{param.data(), param.data() + param.size()};
    }

    return {};
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

    if (read_len < 1)
        return TOO_SMALL;

    if (read_len > hook::maxHookParameterKeySize())
        return TOO_BIG;

    std::vector<uint8_t> paramName{
        read_ptr + memory, read_ptr + read_len + memory};

    auto param = __hook_param(hookCtx, applyCtx, j, paramName);

    if (!param.has_value())
        return DOESNT_EXIST;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr,
        write_len,
        param->data(),
        param->size(),
        memory,
        memory_length);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, hook_param, JSValue raw_name)
{
    JS_HOOK_SETUP();

    auto param_name = FromJSIntArrayOrHexString(
        ctx, raw_name, hook::maxHookParameterKeySize());

    if (!param_name.has_value() || param_name->empty())
        returnJS(INVALID_ARGUMENT);

    auto param = __hook_param(hookCtx, applyCtx, j, *param_name);

    if (!param.has_value())
        returnJS(DOESNT_EXIST);

    auto out = ToJSIntArray(ctx, *param);

    if (!out.has_value())
        returnJS(INTERNAL_ERROR);

    return *out;

    JS_HOOK_TEARDOWN();
}

inline int64_t
__hook_param_set(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint256 const& hash,
    std::vector<uint8_t> const& paramName,
    std::vector<uint8_t> const& paramValue)
{
    if (hookCtx.result.overrideCount >= hook_api::max_params)
        return TOO_MANY_PARAMS;

    hookCtx.result.overrideCount++;

    auto& overrides = hookCtx.result.hookParamOverrides;
    if (overrides.find(hash) == overrides.end())
    {
        overrides[hash] = std::map<std::vector<uint8_t>, std::vector<uint8_t>>{
            {std::move(paramName), std::move(paramValue)}};
    }
    else
        overrides[hash][std::move(paramName)] = std::move(paramValue);

    return paramValue.size();
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

    if (kread_len < 1)
        return TOO_SMALL;

    if (kread_len > hook::maxHookParameterKeySize())
        return TOO_BIG;

    if (hread_len != 32)
        return INVALID_ARGUMENT;

    if (read_len > hook::maxHookParameterValueSize())
        return TOO_BIG;

    std::vector<uint8_t> paramName{
        kread_ptr + memory, kread_ptr + kread_len + memory};
    std::vector<uint8_t> paramValue{
        read_ptr + memory, read_ptr + read_len + memory};

    ripple::uint256 hash = ripple::uint256::fromVoid(memory + hread_ptr);

    return __hook_param_set(hookCtx, applyCtx, j, hash, paramName, paramValue);

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
        FromJSIntArrayOrHexString(ctx, val, hook::maxHookParameterValueSize());
    auto param_hash = FromJSIntArrayOrHexString(ctx, hhash, 32);

    if (!param_key.has_value() || param_key->empty() ||
        param_key->size() > hook::maxHookParameterKeySize() ||
        !param_val.has_value() ||
        param_val->size() > hook::maxHookParameterValueSize() ||
        !param_hash.has_value() || param_hash->size() != 32)
        returnJS(INVALID_ARGUMENT);

    ripple::uint256 hash = ripple::uint256::fromVoid(param_hash->data());

    if (hookCtx.result.overrideCount >= hook_api::max_params)
        returnJS(TOO_MANY_PARAMS);

    returnJS(
        __hook_param_set(hookCtx, applyCtx, j, hash, *param_key, *param_val));

    JS_HOOK_TEARDOWN();
}

inline int64_t
__hook_skip(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint256 const& hash,
    uint32_t flags)
{
    auto& skips = hookCtx.result.hookSkips;

    if (flags == 1)
    {
        // delete flag
        if (skips.find(hash) == skips.end())
            return DOESNT_EXIST;
        skips.erase(hash);
        return 1;
    }

    // first check if it's already in the skips set
    if (skips.find(hash) != skips.end())
        return 1;

    // next check if it's even in this chain
    std::shared_ptr<SLE> hookSLE =
        applyCtx.view().peek(hookCtx.result.hookKeylet);

    if (!hookSLE || !hookSLE->isFieldPresent(sfHooks))
        return INTERNAL_ERROR;

    ripple::STArray const& hooks = hookSLE->getFieldArray(sfHooks);
    bool found = false;
    for (auto const& hookObj : hooks)
    {
        if (hookObj.isFieldPresent(sfHookHash))
        {
            if (hookObj.getFieldH256(sfHookHash) == hash)
            {
                found = true;
                break;
            }
        }
    }

    if (!found)
        return DOESNT_EXIST;

    // finally add it to the skips list
    hookCtx.result.hookSkips.emplace(hash);
    return 1;
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

    if (flags != 0 && flags != 1)
        return INVALID_ARGUMENT;

    ripple::uint256 hash = ripple::uint256::fromVoid(memory + read_ptr);

    return __hook_skip(hookCtx, applyCtx, j, hash, flags);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, hook_skip, JSValue raw_hhash, JSValue raw_flags)
{
    JS_HOOK_SETUP();

    auto hhash = FromJSIntArrayOrHexString(ctx, raw_hhash, 32);
    auto flags = FromJSInt(ctx, raw_flags);

    if (!hhash.has_value() || hhash->size() != 32 || !flags.has_value() ||
        *flags > 1 || *flags < 0)
        returnJS(INVALID_ARGUMENT);

    ripple::uint256 hash = ripple::uint256::fromVoid(hhash->data());

    returnJS(__hook_skip(hookCtx, applyCtx, j, hash, *flags));

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCNARG(int64_t, hook_pos)
{
    return hookCtx.result.hookChainPosition;
}

DEFINE_JS_FUNCNARG(JSValue, hook_pos)
{
    JS_HOOK_SETUP();

    returnJS(hookCtx.result.hookChainPosition);

    JS_HOOK_TEARDOWN();
}

DEFINE_WASM_FUNCNARG(int64_t, hook_again)
{
    WASM_HOOK_SETUP();

    if (hookCtx.result.executeAgainAsWeak)
        return ALREADY_SET;

    if (hookCtx.result.isStrong)
    {
        hookCtx.result.executeAgainAsWeak = true;
        return 1;
    }

    return PREREQUISITE_NOT_MET;

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCNARG(JSValue, hook_again)
{
    JS_HOOK_SETUP();

    if (hookCtx.result.executeAgainAsWeak)
        returnJS(ALREADY_SET);

    if (hookCtx.result.isStrong)
    {
        hookCtx.result.executeAgainAsWeak = true;
        returnJS(1);
    }

    returnJS(PREREQUISITE_NOT_MET);

    JS_HOOK_TEARDOWN();
}

inline int64_t
__meta_slot(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint32_t slot_into)
{
    if (!hookCtx.result.provisionalMeta)
        return PREREQUISITE_NOT_MET;

    if (slot_into > hook_api::max_slots)
        return INVALID_ARGUMENT;

    // check if we can emplace the object to a slot
    if (slot_into == 0 && no_free_slots(hookCtx))
        return NO_FREE_SLOTS;

    if (slot_into == 0)
    {
        if (auto found = get_free_slot(hookCtx); found)
            slot_into = *found;
        else
            return NO_FREE_SLOTS;
    }

    hookCtx.slot[slot_into] =
        hook::SlotEntry{.storage = hookCtx.result.provisionalMeta, .entry = 0};

    hookCtx.slot[slot_into].entry = &(*hookCtx.slot[slot_into].storage);

    return slot_into;
}

DEFINE_WASM_FUNCTION(int64_t, meta_slot, uint32_t slot_into)
{
    WASM_HOOK_SETUP();

    return __meta_slot(hookCtx, applyCtx, j, slot_into);

    WASM_HOOK_TEARDOWN();
}

DEFINE_JS_FUNCTION(JSValue, meta_slot, JSValue raw_slot_into)
{
    JS_HOOK_SETUP();

    auto slot_into = FromJSInt(ctx, raw_slot_into);
    if (!slot_into.has_value() || !fits_u32(slot_into))
        returnJS(INVALID_ARGUMENT);

    returnJS(__meta_slot(hookCtx, applyCtx, j, (uint32_t)(*slot_into)));

    JS_HOOK_TEARDOWN();
}

inline int64_t
__xpop_slot(
    hook::HookContext& hookCtx,
    ApplyContext& applyCtx,
    beast::Journal& j,
    uint32_t slot_into_tx,
    uint32_t slot_into_meta)
{
    if (applyCtx.tx.getFieldU16(sfTransactionType) != ttIMPORT)
        return PREREQUISITE_NOT_MET;

    if (slot_into_tx > hook_api::max_slots ||
        slot_into_meta > hook_api::max_slots)
        return INVALID_ARGUMENT;

    size_t free_count = hook_api::max_slots - hookCtx.slot.size();

    size_t needed_count = slot_into_tx == 0 && slot_into_meta == 0
        ? 2
        : slot_into_tx != 0 && slot_into_meta != 0 ? 0 : 1;

    if (free_count < needed_count)
        return NO_FREE_SLOTS;

    // if they supply the same slot number for both (other than 0)
    // they will produce a collision
    if (needed_count == 0 && slot_into_tx == slot_into_meta)
        return INVALID_ARGUMENT;

    if (slot_into_tx == 0)
    {
        if (no_free_slots(hookCtx))
            return NO_FREE_SLOTS;

        if (auto found = get_free_slot(hookCtx); found)
            slot_into_tx = *found;
        else
            return NO_FREE_SLOTS;
    }

    if (slot_into_meta == 0)
    {
        if (no_free_slots(hookCtx))
            return NO_FREE_SLOTS;

        if (auto found = get_free_slot(hookCtx); found)
            slot_into_meta = *found;
        else
            return NO_FREE_SLOTS;
    }

    auto [tx, meta] = Import::getInnerTxn(applyCtx.tx, j);

    if (!tx || !meta)
        return INVALID_TXN;

    hookCtx.slot[slot_into_tx] =
        hook::SlotEntry{.storage = std::move(tx), .entry = 0};

    hookCtx.slot[slot_into_tx].entry = &(*hookCtx.slot[slot_into_tx].storage);

    hookCtx.slot[slot_into_meta] =
        hook::SlotEntry{.storage = std::move(meta), .entry = 0};

    hookCtx.slot[slot_into_meta].entry =
        &(*hookCtx.slot[slot_into_meta].storage);

    return (slot_into_tx << 16U) + slot_into_meta;
}

DEFINE_WASM_FUNCTION(
    int64_t,
    xpop_slot,
    uint32_t slot_into_tx,
    uint32_t slot_into_meta)
{
    WASM_HOOK_SETUP();

    return __xpop_slot(hookCtx, applyCtx, j, slot_into_tx, slot_into_meta);

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

    returnJS(__xpop_slot(
        hookCtx,
        applyCtx,
        j,
        (uint32_t)(*slot_into_tx),
        (uint32_t)(*slot_into_meta)));

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
