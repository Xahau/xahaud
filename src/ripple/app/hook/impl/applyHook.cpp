#include <ripple/app/hook/applyHook.h>
#include <ripple/app/ledger/OpenLedger.h>
#include <ripple/app/ledger/TransactionMaster.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/Slice.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/tokens.h>
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <any>
#include <cfenv>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <wasmedge/wasmedge.h>

using namespace ripple;

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
                    ADD_TSH(bo->getAccountID(sfDestination), tshWEAK);
            }

            if (so)
            {
                ADD_TSH(so->getAccountID(sfOwner), tshSTRONG);
                if (so->isFieldPresent(sfDestination))
                    ADD_TSH(so->getAccountID(sfDestination), tshWEAK);
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
                    ADD_TSH(offer->getAccountID(sfOwner), tshSTRONG);
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

std::optional<ripple::Keylet>
unserialize_keylet(uint8_t* ptr, uint32_t len)
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
    ripple::uint256 const& hookNamespace,
    ripple::Blob const& wasm,
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
    uint32_t wasmParam,
    uint8_t hookChainPosition,
    std::shared_ptr<STObject const> const& provisionalMeta)
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
             .exitType =
                 hook_api::ExitType::ROLLBACK,  // default is to rollback unless
                                                // hook calls accept()
             .exitReason = std::string(""),
             .exitCode = -1,
             .hasCallback = hasCallback,
             .isCallback = isCallback,
             .isStrong = isStrong,
             .wasmParam = wasmParam,
             .hookChainPosition = hookChainPosition,
             .foreignStateSetDisabled = false,
             .provisionalMeta = provisionalMeta},
        .emitFailure = isCallback && wasmParam & 1
            ? std::optional<ripple::STObject>(
                  (*(applyCtx.view().peek(keylet::emittedTxn(
                       applyCtx.tx.getFieldH256(sfTransactionHash)))))
                      .downcast<STObject>())
            : std::optional<ripple::STObject>()};

    auto const& j = applyCtx.app.journal("View");

    HookExecutor executor{hookCtx};

    executor.executeWasm(
        wasm.data(), (size_t)wasm.size(), isCallback, wasmParam, j);

    JLOG(j.trace()) << "HookInfo[" << HC_ACC() << "]: "
                    << (hookCtx.result.exitType == hook_api::ExitType::ROLLBACK
                            ? "ROLLBACK"
                            : "ACCEPT")
                    << " RS: '" << hookCtx.result.exitReason.c_str()
                    << "' RC: " << hookCtx.result.exitCode;

    return hookCtx.result;
}

/* If XRPLD is running with trace log level hooks may produce debugging output
 * to the trace log specifying both a string and an integer to output */
DEFINE_HOOK_FUNCTION(
    int64_t,
    trace_num,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t number)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx on
                   // current stack
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
    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    trace,
    uint32_t mread_ptr,
    uint32_t mread_len,
    uint32_t dread_ptr,
    uint32_t dread_len,
    uint32_t as_hex)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx on
                   // current stack
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
    HOOK_TEARDOWN();
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
    ripple::Blob& data,
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

DEFINE_HOOK_FUNCTION(
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
// update or create a hook state object
// read_ptr = data to set, kread_ptr = key
// RH NOTE passing 0 size causes a delete operation which is as-intended
/*
    uint32_t write_ptr, uint32_t write_len,
    uint32_t kread_ptr, uint32_t kread_len,         // key
    uint32_t nread_ptr, uint32_t nread_len,         // namespace
    uint32_t aread_ptr, uint32_t aread_len )        // account
 */
DEFINE_HOOK_FUNCTION(
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
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    // local modifications are always allowed
    if (aread_len == 0 || acc == hookCtx.result.account)
    {
        if (int64_t ret = set_state_cache(hookCtx, acc, ns, *key, data, true);
            ret < 0)
            return ret;

        return read_len;
    }

    // execution to here means it's actually a foreign set
    if (hookCtx.result.foreignStateSetDisabled)
        return PREVIOUS_FAILURE_PREVENTS_RETRY;

    // first check if we've already modified this state
    auto cacheEntry = lookup_state_cache(hookCtx, acc, ns, *key);
    if (cacheEntry && cacheEntry->get().first)
    {
        // if a cache entry already exists and it has already been modified
        // don't check grants again
        if (int64_t ret = set_state_cache(hookCtx, acc, ns, *key, data, true);
            ret < 0)
            return ret;

        return read_len;
    }

    // cache miss or cache was present but entry was not marked as previously
    // modified therefore before continuing we need to check grants
    auto const sle = view.read(ripple::keylet::hook(acc));
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
            auto const def = view.read(ripple::keylet::hookDefinition(
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

    if (int64_t ret = set_state_cache(hookCtx, acc, ns, *key, data, true);
        ret < 0)
        return ret;

    return read_len;
    HOOK_TEARDOWN();
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

                    if (result != tesSUCCESS)
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
DEFINE_HOOK_FUNCTION(
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
DEFINE_HOOK_FUNCTION(
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
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

// Cause the originating transaction to go through, save state changes and emit
// emitted tx, exit hook
DEFINE_HOOK_FUNCTION(
    int64_t,
    accept,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t error_code)
{
    HOOK_SETUP();
    HOOK_EXIT(read_ptr, read_len, error_code, hook_api::ExitType::ACCEPT);
    HOOK_TEARDOWN();
}

// Cause the originating transaction to be rejected, discard state changes and
// discard emitted tx, exit hook
DEFINE_HOOK_FUNCTION(
    int64_t,
    rollback,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t error_code)
{
    HOOK_SETUP();
    HOOK_EXIT(read_ptr, read_len, error_code, hook_api::ExitType::ROLLBACK);
    HOOK_TEARDOWN();
}

// Write the TxnID of the originating transaction into the write_ptr
DEFINE_HOOK_FUNCTION(
    int64_t,
    otxn_id,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t flags)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

// Return the tt (Transaction Type) numeric code of the originating transaction
DEFINE_HOOK_FUNCNARG(int64_t, otxn_type)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (hookCtx.emitFailure)
        return safe_cast<TxType>(
            hookCtx.emitFailure->getFieldU16(sfTransactionType));

    return applyCtx.tx.getTxnType();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, otxn_slot, uint32_t slot_into)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}
// Return the burden of the originating transaction... this will be 1 unless the
// originating transaction was itself an emitted transaction from a previous
// hook invocation
DEFINE_HOOK_FUNCNARG(int64_t, otxn_burden)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

// Return the generation of the originating transaction... this will be 1 unless
// the originating transaction was itself an emitted transaction from a previous
// hook invocation
DEFINE_HOOK_FUNCNARG(int64_t, otxn_generation)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

// Return the generation of a hypothetically emitted transaction from this hook
DEFINE_HOOK_FUNCNARG(int64_t, etxn_generation)
{
    // proxy only, no setup or teardown
    return otxn_generation(hookCtx, frameCtx) + 1;
}

// Return the current ledger sequence number
DEFINE_HOOK_FUNCNARG(int64_t, ledger_seq)
{
    HOOK_SETUP();

    return view.info().seq;

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    ledger_last_hash,
    uint32_t write_ptr,
    uint32_t write_len)
{
    HOOK_SETUP();

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;
    if (write_len < 32)
        return TOO_SMALL;

    uint256 hash = view.info().parentHash;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, hash.data(), 32, memory, memory_length);

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCNARG(int64_t, ledger_last_time)
{
    HOOK_SETUP();

    return std::chrono::duration_cast<std::chrono::seconds>(
               view.info().parentCloseTime.time_since_epoch())
        .count();

    HOOK_TEARDOWN();
}

// Dump a field from the originating transaction into the hook's memory
DEFINE_HOOK_FUNCTION(
    int64_t,
    otxn_field,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t field_id)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    slot,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t slot_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, slot_clear, uint32_t slot_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    hookCtx.slot.erase(slot_no);
    hookCtx.slot_free.push(slot_no);

    return 1;

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, slot_count, uint32_t slot_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    if (hookCtx.slot[slot_no].entry == 0)
        return INTERNAL_ERROR;

    if (hookCtx.slot[slot_no].entry->getSType() != STI_ARRAY)
        return NOT_AN_ARRAY;

    return hookCtx.slot[slot_no].entry->downcast<ripple::STArray>().size();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_set,
    uint32_t read_ptr,
    uint32_t read_len,  // readptr is a keylet
    uint32_t slot_into /* providing 0 allocates a slot to you */)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if ((read_len != 32 && read_len != 34) || slot_into > hook_api::max_slots)
        return INVALID_ARGUMENT;

    // check if we can emplace the object to a slot
    if (slot_into == 0 && no_free_slots(hookCtx))
        return NO_FREE_SLOTS;

    std::vector<uint8_t> slot_key{
        memory + read_ptr, memory + read_ptr + read_len};
    std::optional<std::shared_ptr<const ripple::STObject>> slot_value =
        std::nullopt;

    if (read_len == 34)
    {
        std::optional<ripple::Keylet> kl =
            unserialize_keylet(memory + read_ptr, read_len);
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
        uint256 hash = ripple::base_uint<256>::fromVoid(memory + read_ptr);

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, slot_size, uint32_t slot_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return DOESNT_EXIST;

    if (hookCtx.slot[slot_no].entry == 0)
        return INTERNAL_ERROR;

    // RH TODO: this is a very expensive way of computing size, cache it
    Serializer s;
    hookCtx.slot[slot_no].entry->add(s);
    return s.getDataLength();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_subarray,
    uint32_t parent_slot,
    uint32_t array_id,
    uint32_t new_slot)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    slot_subfield,
    uint32_t parent_slot,
    uint32_t field_id,
    uint32_t new_slot)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, slot_type, uint32_t slot_no, uint32_t flags)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, slot_float, uint32_t slot_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
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
    HOOK_SETUP();

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

    HOOK_TEARDOWN();
}

/* Emit a transaction from this hook. Transaction must be in STObject form,
 * fully formed and valid. XRPLD does not modify transactions it only checks
 * them for validity. */
DEFINE_HOOK_FUNCTION(
    int64_t,
    emit,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (write_len < 32)
        return TOO_SMALL;

    auto& app = hookCtx.applyCtx.app;

    if (hookCtx.expected_etxn_count < 0)
        return PREREQUISITE_NOT_MET;

    if (hookCtx.result.emittedTxn.size() >= hookCtx.expected_etxn_count)
        return TOO_MANY_EMITTED_TXN;

    ripple::Blob blob{memory + read_ptr, memory + read_ptr + read_len};

    std::shared_ptr<STTx const> stpTrans;
    try
    {
        stpTrans = std::make_shared<STTx const>(
            SerialIter{memory + read_ptr, read_len});
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

    uint32_t gen_proper = etxn_generation(hookCtx, frameCtx);

    if (gen != gen_proper)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitGeneration provided in EmitDetails "
                        << "not correct (" << gen << ") "
                        << "should be " << gen_proper;
        return EMISSION_FAILURE;
    }

    uint64_t bur_proper = etxn_burden(hookCtx, frameCtx);
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
    int64_t minfee = etxn_fee_base(hookCtx, frameCtx, read_ptr, read_len);

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
        applyCtx.view().rules(),
        *stpTrans,
        ripple::ApplyFlags::tapPREFLIGHT_EMIT,
        j);

    if (preflightResult.ter != tesSUCCESS)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Transaction preflight failure: "
                        << preflightResult.ter;
        return EMISSION_FAILURE;
    }

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
    HOOK_TEARDOWN();
}

// When implemented will return the hash of the current hook
DEFINE_HOOK_FUNCTION(
    int64_t,
    hook_hash,
    uint32_t write_ptr,
    uint32_t write_len,
    int32_t hook_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

// Write the account id that the running hook is installed on into write_ptr
DEFINE_HOOK_FUNCTION(
    int64_t,
    hook_account,
    uint32_t write_ptr,
    uint32_t ptr_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

// Deterministic nonces (can be called multiple times)
// Writes nonce into the write_ptr
DEFINE_HOOK_FUNCTION(
    int64_t,
    etxn_nonce,
    uint32_t write_ptr,
    uint32_t write_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx, view on current stack

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (hookCtx.emit_nonce_counter > hook_api::max_nonce)
        return TOO_MANY_NONCES;

    if (write_len < 32)
        return TOO_SMALL;

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

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 32, hash.data(), 32, memory, memory_length);

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    ledger_nonce,
    uint32_t write_ptr,
    uint32_t write_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx, view on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    ledger_keylet,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t lread_ptr,
    uint32_t lread_len,
    uint32_t hread_ptr,
    uint32_t hread_len)
{
    HOOK_SETUP();

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

    HOOK_TEARDOWN();
}

// Reserve one or more transactions for emission from the running hook
DEFINE_HOOK_FUNCTION(int64_t, etxn_reserve, uint32_t count)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (hookCtx.expected_etxn_count > -1)
        return ALREADY_SET;

    if (count < 1)
        return TOO_SMALL;

    if (count > hook_api::max_emit)
        return TOO_BIG;

    hookCtx.expected_etxn_count = count;

    return count;

    HOOK_TEARDOWN();
}

// Compute the burden of an emitted transaction based on a number of factors
DEFINE_HOOK_FUNCNARG(int64_t, etxn_burden)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (hookCtx.expected_etxn_count <= -1)
        return PREREQUISITE_NOT_MET;

    uint64_t last_burden = (uint64_t)otxn_burden(
        hookCtx, frameCtx);  // always non-negative so cast is safe

    uint64_t burden = last_burden * hookCtx.expected_etxn_count;
    if (burden <
        last_burden)  // this overflow will never happen but handle it anyway
        return FEE_TOO_LARGE;

    return burden;

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    util_sha512h,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx, view on current stack

    if (write_len < 32)
        return TOO_SMALL;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length) ||
        NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    auto hash = ripple::sha512Half(ripple::Slice{memory + read_ptr, read_len});

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 32, hash.data(), 32, memory, memory_length);

    HOOK_TEARDOWN();
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

// Given an serialized object in memory locate and return the offset and length
// of the payload of a subfield of that object. Arrays are returned fully
// formed. If successful returns offset and length joined as int64_t. Use
// SUB_OFFSET and SUB_LENGTH to extract.
DEFINE_HOOK_FUNCTION(
    int64_t,
    sto_subfield,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t field_id)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (read_len < 2)
        return TOO_SMALL;

    unsigned char* start = (unsigned char*)(memory + read_ptr);
    unsigned char* upto = start;
    unsigned char* end = start + read_len;

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

    HOOK_TEARDOWN();
}

// Same as subfield but indexes into a serialized array
DEFINE_HOOK_FUNCTION(
    int64_t,
    sto_subarray,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t index_id)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (read_len < 2)
        return TOO_SMALL;

    unsigned char* start = (unsigned char*)(memory + read_ptr);
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

    HOOK_TEARDOWN();
}

// Convert an account ID into a base58-check encoded r-address
DEFINE_HOOK_FUNCTION(
    int64_t,
    util_raddr,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

// Convert a base58-check encoded r-address into a 20 byte account id
DEFINE_HOOK_FUNCTION(
    int64_t,
    util_accid,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
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
DEFINE_HOOK_FUNCTION(
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
    HOOK_SETUP();

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

    // we must inject the field at the canonical location....
    // so find that location
    unsigned char* start = (unsigned char*)(memory + sread_ptr);
    unsigned char* upto = start;
    unsigned char* end = start + sread_len;
    unsigned char* inject_start = end;
    unsigned char* inject_end = end;

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
        WRITE_WASM_MEMORY(
            bytes_written,
            write_ptr,
            write_len,
            start,
            (inject_start - start),
            memory,
            memory_length);
    }

    if (fread_len > 0)
    {
        // write the field (or don't if it's a delete operation)
        WRITE_WASM_MEMORY(
            bytes_written,
            (write_ptr + bytes_written),
            (write_len - bytes_written),
            memory + fread_ptr,
            fread_len,
            memory,
            memory_length);
    }

    // part 2
    if (end - inject_end > 0)
    {
        WRITE_WASM_MEMORY(
            bytes_written,
            (write_ptr + bytes_written),
            (write_len - bytes_written),
            inject_end,
            (end - inject_end),
            memory,
            memory_length);
    }
    return bytes_written;

    HOOK_TEARDOWN();
}

/**
 * Remove a field from an sto if the field is present
 */
DEFINE_HOOK_FUNCTION(
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

DEFINE_HOOK_FUNCTION(
    int64_t,
    sto_validate,
    uint32_t read_ptr,
    uint32_t read_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    // RH TODO: see if an internal ripple function/class would do this better

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (read_len < 2)
        return TOO_SMALL;

    unsigned char* start = (unsigned char*)(memory + read_ptr);
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

    HOOK_TEARDOWN();
}

// Validate either an secp256k1 signature or an ed25519 signature, using the
// XRPLD convention for identifying the key type. Pointer prefixes: d = data, s
// = signature, k = public key.
DEFINE_HOOK_FUNCTION(
    int64_t,
    util_verify,
    uint32_t dread_ptr,
    uint32_t dread_len,
    uint32_t sread_ptr,
    uint32_t sread_len,
    uint32_t kread_ptr,
    uint32_t kread_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

// Return the current fee base of the current ledger (multiplied by a margin)
DEFINE_HOOK_FUNCNARG(int64_t, fee_base)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    return view.fees().base.drops();

    HOOK_TEARDOWN();
}

// Return the fee base for a hypothetically emitted transaction from the current
// hook based on byte count
DEFINE_HOOK_FUNCTION(
    int64_t,
    etxn_fee_base,
    uint32_t read_ptr,
    uint32_t read_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (hookCtx.expected_etxn_count <= -1)
        return PREREQUISITE_NOT_MET;

    try
    {
        ripple::Slice tx{
            reinterpret_cast<const void*>(read_ptr + memory), read_len};

        SerialIter sitTrans(tx);

        std::unique_ptr<STTx const> stpTrans;
        stpTrans = std::make_unique<STTx const>(std::ref(sitTrans));

        return Transactor::calculateBaseFee(
                   *(applyCtx.app.openLedger().current()), *stpTrans)
            .drops();
    }
    catch (std::exception& e)
    {
        return INVALID_TXN;
    }

    HOOK_TEARDOWN();
}

// Populate an sfEmitDetails field in a soon-to-be emitted transaction
DEFINE_HOOK_FUNCTION(
    int64_t,
    etxn_details,
    uint32_t write_ptr,
    uint32_t write_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    int64_t expected_size = 138U;
    if (!hookCtx.result.hasCallback)
        expected_size -= 22U;

    if (write_len < expected_size)
        return TOO_SMALL;

    if (hookCtx.expected_etxn_count <= -1)
        return PREREQUISITE_NOT_MET;

    uint32_t generation = (uint32_t)(etxn_generation(
        hookCtx, frameCtx));  // always non-negative so cast is safe

    int64_t burden = etxn_burden(hookCtx, frameCtx);
    if (burden < 1)
        return FEE_TOO_LARGE;

    unsigned char* out = memory + write_ptr;

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
    if (otxn_id(hookCtx, frameCtx, out - memory, 32, 1) != 32)
        return INTERNAL_ERROR;
    out += 32;
    *out++ = 0x5CU;  // sfEmitNonce                                     /* upto
                     // =  49 | size = 33 */
    if (etxn_nonce(hookCtx, frameCtx, out - memory, 32) != 32)
        return INTERNAL_ERROR;
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
        if (hook_account(hookCtx, frameCtx, out - memory, 20) != 20)
            return INTERNAL_ERROR;
        out += 20;
    }
    *out++ = 0xE1U;  // end object (sfEmitDetails)                     /* upto =
                     // 137 | size =  1 */
                     /* upto = 138 | --------- */
    int64_t outlen = out - memory - write_ptr;

    DBG_PRINTF("emitdetails size = %d\n", outlen);
    return outlen;

    HOOK_TEARDOWN();
}

// Guard function... very important. Enforced on SetHook transaction, keeps
// track of how many times a runtime loop iterates and terminates the hook if
// the iteration count rises above a preset number of iterations as determined
// by the hook developer
DEFINE_HOOK_FUNCTION(int32_t, _g, uint32_t id, uint32_t maxitr)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
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

DEFINE_HOOK_FUNCTION(
    int64_t,
    trace_float,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx on
                   // current stack

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
        j.trace() << "HookTrace[" << HC_ACC() << "]:"
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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_set, int32_t exp, int64_t mantissa)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (mantissa == 0)
        return 0;

    int64_t normalized = hook_float::normalize_xfl(mantissa, exp);

    // the above function will underflow into a canonical 0
    // but this api must report that underflow
    if (normalized == 0 || normalized == XFL_OVERFLOW)
        return INVALID_FLOAT;

    return normalized;

    HOOK_TEARDOWN();
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

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_int,
    int64_t float1,
    uint32_t decimal_places,
    uint32_t absolute)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_multiply, int64_t float1, int64_t float2)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_mulratio,
    int64_t float1,
    uint32_t round_up,
    uint32_t numerator,
    uint32_t denominator)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_negate, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (float1 == 0)
        return 0;
    RETURN_IF_INVALID_FLOAT(float1);
    return hook_float::invert_sign(float1);

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_compare,
    int64_t float1,
    int64_t float2,
    uint32_t mode)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_sum, int64_t float1, int64_t float2)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
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
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    if (is_xrp || is_short)
    {
        // do nothing
    }
    else if (field < 16 && type < 16)
    {
        *(memory + write_ptr) = (((uint8_t)type) << 4U) + ((uint8_t)field);
        bytes_written++;
    }
    else if (field >= 16 && type < 16)
    {
        *(memory + write_ptr) = (((uint8_t)type) << 4U);
        *(memory + write_ptr + 1) = ((uint8_t)field);
        bytes_written += 2;
    }
    else if (field < 16 && type >= 16)
    {
        *(memory + write_ptr) = (((uint8_t)field) << 4U);
        *(memory + write_ptr + 1) = ((uint8_t)type);
        bytes_written += 2;
    }
    else
    {
        *(memory + write_ptr) = 0;
        *(memory + write_ptr + 1) = ((uint8_t)type);
        *(memory + write_ptr + 2) = ((uint8_t)field);
        bytes_written += 3;
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

    WRITE_WASM_MEMORY(
        bytes_written,
        write_ptr + bytes_written,
        write_len - bytes_written,
        out,
        8,
        memory,
        memory_length);

    if (!is_xrp && !is_short)
    {
        WRITE_WASM_MEMORY(
            bytes_written,
            write_ptr + bytes_written,
            write_len - bytes_written,
            (*currency).data(),
            20,
            memory,
            memory_length);

        WRITE_WASM_MEMORY(
            bytes_written,
            write_ptr + bytes_written,
            write_len - bytes_written,
            (*issuer).data(),
            20,
            memory,
            memory_length);
    }

    return bytes_written;

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    float_sto_set,
    uint32_t read_ptr,
    uint32_t read_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (read_len < 8)
        return NOT_AN_OBJECT;

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    uint8_t* upto = memory + read_ptr;

    if (read_len > 8)
    {
        uint8_t hi = memory[read_ptr] >> 4U;
        uint8_t lo = memory[read_ptr] & 0xFU;

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

    HOOK_TEARDOWN();
}

const int64_t float_one_internal = make_float(1000000000000000ull, -15, false);

inline int64_t
float_divide_internal(int64_t float1, int64_t float2)
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
        for (; man1 > man2; man1 -= man2, ++i)
            ;

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

DEFINE_HOOK_FUNCTION(int64_t, float_divide, int64_t float1, int64_t float2)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    return float_divide_internal(float1, float2);

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCNARG(int64_t, float_one)
{
    return float_one_internal;
}

DEFINE_HOOK_FUNCTION(int64_t, float_invert, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (float1 == 0)
        return DIVISION_BY_ZERO;
    if (float1 == float_one_internal)
        return float_one_internal;
    return float_divide_internal(float_one_internal, float1);

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_mantissa, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);
    if (float1 == 0)
        return 0;
    return get_mantissa(float1);

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_sign, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);
    if (float1 == 0)
        return 0;
    return is_negative(float1);

    HOOK_TEARDOWN();
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

DEFINE_HOOK_FUNCTION(int64_t, float_log, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_root, int64_t float1, uint32_t n)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    otxn_param,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    hook_param,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    if (read_len < 1)
        return TOO_SMALL;

    if (read_len > 32)
        return TOO_BIG;

    std::vector<uint8_t> paramName{
        read_ptr + memory, read_ptr + read_len + memory};

    // first check for overrides set by prior hooks in the chain
    auto const& overrides = hookCtx.result.hookParamOverrides;
    if (overrides.find(hookCtx.result.hookHash) != overrides.end())
    {
        auto const& params = overrides.at(hookCtx.result.hookHash);
        if (params.find(paramName) != params.end())
        {
            auto const& param = params.at(paramName);
            if (param.size() == 0)
                return DOESNT_EXIST;  // allow overrides to "delete" parameters

            WRITE_WASM_MEMORY_AND_RETURN(
                write_ptr,
                write_len,
                param.data(),
                param.size(),
                memory,
                memory_length);
        }
    }

    // next check if there's a param set on this hook
    auto const& params = hookCtx.result.hookParams;
    if (params.find(paramName) != params.end())
    {
        auto const& param = params.at(paramName);
        if (param.size() == 0)
            return DOESNT_EXIST;

        WRITE_WASM_MEMORY_AND_RETURN(
            write_ptr,
            write_len,
            param.data(),
            param.size(),
            memory,
            memory_length);
    }

    return DOESNT_EXIST;

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    hook_param_set,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t kread_ptr,
    uint32_t kread_len,
    uint32_t hread_ptr,
    uint32_t hread_len)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    return read_len;

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    hook_skip,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t flags)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (read_len != 32)
        return INVALID_ARGUMENT;

    if (flags != 0 && flags != 1)
        return INVALID_ARGUMENT;

    auto& skips = hookCtx.result.hookSkips;
    ripple::uint256 hash = ripple::uint256::fromVoid(memory + read_ptr);

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCNARG(int64_t, hook_pos)
{
    return hookCtx.result.hookChainPosition;
}

DEFINE_HOOK_FUNCNARG(int64_t, hook_again)
{
    HOOK_SETUP();

    if (hookCtx.result.executeAgainAsWeak)
        return ALREADY_SET;

    if (hookCtx.result.isStrong)
    {
        hookCtx.result.executeAgainAsWeak = true;
        return 1;
    }

    return PREREQUISITE_NOT_MET;

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, meta_slot, uint32_t slot_into)
{
    HOOK_SETUP();

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    xpop_slot,
    uint32_t slot_into_tx,
    uint32_t slot_into_meta)
{
    HOOK_SETUP();

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

    HOOK_TEARDOWN();
}
/*

DEFINE_HOOK_FUNCTION(
    int64_t,
    str_find,
    uint32_t hread_ptr, uint32_t hread_len,
    uint32_t nread_ptr, uint32_t nread_len,
    uint32_t mode,      uint32_t n)
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx,
hookCtx on current stack

    if (NOT_IN_BOUNDS(hread_ptr, hread_len, memory_length) ||
        NOT_IN_BOUNDS(nread_ptr, nread_len, memory_length))
        return OUT_OF_BOUNDS;

    if (hread_len > 32*1024)
        return TOO_BIG;

    if (nread_len > 256)
        return TOO_BIG;

    if (hread_len == 0)
        return TOO_SMALL;

    if (mode > 3)
        return INVALID_ARGUMENT;

    if (n >= hread_len)
        return INVALID_ARGUMENT;

    // overload for str_len
    if (nread_ptr == 0)
    {
        if (nread_len != 0)
            return INVALID_ARGUMENT;

        return strnlen((const char*)(hread_ptr + memory), hread_len);
    }

    bool insensitive = mode % 2 == 1;

    // just the haystack based on where to start search from
    hread_ptr += n;
    hread_len -= n;

    if (NOT_IN_BOUNDS(hread_ptr, hread_len, memory_length))
        return OUT_OF_BOUNDS;

    std::string_view haystack{(const char*)(memory + hread_ptr), hread_len};
    if (mode < 2)
    {
        // plain string mode: 0 == case sensitive

        std::string_view needle{(const char*)(memory + nread_ptr), nread_len};

        auto found = std::search(
            haystack.begin(), haystack.end(),
            needle.begin(),   needle.end(),
            insensitive
            ?   [](char ch1, char ch2)
                {
                    return std::toupper(ch1) == std::toupper(ch2);
                }
            :   [](char ch1, char ch2)
                {
                    return ch1 == ch2;
                }
        );

        if (found == haystack.end())
            return DOESNT_EXIST;
        return found - haystack.begin();
    }
    else
    {
        // regex mode mode: 2 == case sensitive

        return NOT_IMPLEMENTED;

    }
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    str_replace,
    uint32_t write_ptr, uint32_t write_len,
    uint32_t hread_ptr, uint32_t hread_len,
    uint32_t nread_ptr, uint32_t nread_len,
    uint32_t rread_ptr, uint32_t rread_len,
    uint32_t mode,      uint32_t n)
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx,
hookCtx on current stack

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length) ||
        NOT_IN_BOUNDS(hread_ptr, hread_len, memory_length) ||
        NOT_IN_BOUNDS(nread_ptr, nread_len, memory_length) ||
        NOT_IN_BOUNDS(rread_ptr, rread_len, memory_length))
        return OUT_OF_BOUNDS;

    if (hread_len > 32*1024)
        return TOO_BIG;

    if (nread_len > 256)
        return TOO_BIG;

    if (hread_len == 0)
        return TOO_SMALL;

    if (nread_len == 0)
        return TOO_SMALL;

    return NOT_IMPLEMENTED;
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    str_compare,
    uint32_t fread_ptr, uint32_t fread_len,
    uint32_t sread_ptr, uint32_t sread_len,
    uint32_t mode)
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx,
hookCtx on current stack

    if (NOT_IN_BOUNDS(fread_ptr, fread_len, memory_length) ||
        NOT_IN_BOUNDS(sread_ptr, sread_len, memory_length))
        return OUT_OF_BOUNDS;

    if (mode > 1)
        return INVALID_ARGUMENT;

    if (fread_len > 255 || sread_len > 255)
        return TOO_BIG;

    if (fread_len == 0 || sread_len == 0)
        return TOO_SMALL;

    bool insensitive = mode == 1;

    const char* it1 = (const char*)(memory + fread_ptr);
    const char* it2 = (const char*)(memory + sread_ptr);
    const char* end1 = it1 + fread_len;
    const char* end2 = it2 + sread_len;

    if (insensitive)
    for(; it1 < end1 && it2 < end2; ++it1, ++it2)
    {
        if (*it1 < *it2)
            return 0;
        if (*it1 > *it2)
            return 2;
    }
    else
    for(; it1 < end1 && it2 < end2; ++it1, ++it2)
    {
        if (std::tolower(*it1) < std::tolower(*it2))
            return 0;
        if (std::tolower(*it1) > std::tolower(*it2))
            return 2;
    }
    return 1;
}


inline
ssize_t
findNul(const void* vptr, size_t len)
{
    const char* ptr = (const char*)vptr;
    ssize_t found = -1;
    for (size_t i = 0; i < len; ++i)
    if (ptr[i] == '\0')
    {
        found = i;
        break;
    }
    return found;
}

//    Overloaded API:
//    If operand_type == 0:
//        Copy read_ptr/len to write_ptr/len, do nothing else.
//    If operand_type >  0:
//        Copy read_ptr/len to write_ptr/len up to nul terminator, then
//        If operand_type == 1:
//            Concatenate operand as an i32 to the end of the string in
write_ptr
//        If operand_type == 2:
//            Concatenate operand as an u32 to the end of the string in
write_ptr
//        If operand_type == 3/4:
//            As above with i/u64
//        If operand_type == 5:
//            As above with operand interpreted as an XFL. Top 4 bits of
operand_type are
//            precision for this type.
//        If operand_type == 6:
//            Interpret the four most significant bytes of operand as a ptr, and
the
//            four least significant bytes as a length.
//            Write the bytes at this location to the end of write_ptr.
//        Finally:
//            Add a nul terminator to the end of write_ptr.
DEFINE_HOOK_FUNCTION(
    int64_t,
    str_concat,
    uint32_t write_ptr, uint32_t write_len,
    uint32_t read_ptr,  uint32_t read_len,
    uint64_t operand,   uint32_t operand_type)
{
    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx,
hookCtx on current stack

    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length) ||
        NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;

    if (write_len > 1024 || read_len > 1024)
        return TOO_BIG;
    if (write_len == 0 || read_len == 0)
        return TOO_SMALL;
    if (write_len < read_len)
        return TOO_SMALL;

    uint8_t precision = (uint8_t)((operand_type & 0xF000U) >> 28U);
    operand_type &= 0xFU;

    if (operand_type > 6)
        return INVALID_ARGUMENT;


    //copy operation
    if (operand_type == 0)
    {
        size_t bytecount = std::min(write_len, read_len);
        memcpy(memory + write_ptr, memory + read_ptr, bytecount);
        return bytecount;
    }

    ssize_t nuloffset =
        findNul(memory + read_ptr, read_len);

    if (nuloffset < 0)
        return NOT_A_STRING;
    else
    if (write_len <= nuloffset)
        return TOO_SMALL;

    uint32_t write_start = write_ptr;


    // copy the lhs into the write buffer
    if (write_ptr != read_ptr)
    {
        size_t bytecount = std::min(write_len, std::min(read_len,
(uint32_t)nuloffset)); memcpy(memory + write_ptr, memory + read_ptr, bytecount);
        write_ptr += bytecount;
        write_len -= bytecount;
    }
    else
    {
        write_ptr += nuloffset;
        write_len -= nuloffset;
    }

    if (write_len == 0)
        return TOO_SMALL;

    const ssize_t lhscount = write_ptr - write_start;

    // defensive check
    if (NOT_IN_BOUNDS(write_ptr, write_len, memory_length))
        return OUT_OF_BOUNDS;

    auto write_num = [&]<typename T>(T i, const char* fmt) -> ssize_t
    {
        char buf[128];
        int result = snprintf(buf, 128, fmt, i);
        if (result < 0)
            return TOO_BIG;
        if (result + 1 > write_len)
            return TOO_SMALL;
        // defensive
        size_t bytecount = std::min((uint32_t)result, std::min(127U, write_len -
1)); memcpy(memory + write_ptr, buf, bytecount);
        *(memory + write_ptr + bytecount) = '\0';
        return bytecount + 1 + lhscount;
    };

    // rhs
    switch (operand_type)
    {
        case 1:
            return write_num(( int32_t)operand, "%d");
        case 2:
            return write_num((uint32_t)operand, "%u");
        case 3:
            return write_num(( int64_t)operand, "%lld");
        case 4:
            return write_num((uint64_t)operand, "%llu");
        case 5:
        {
            // XFL
            int32_t   e = get_exponent((int64_t)operand);
            uint64_t  m = get_mantissa((int64_t)operand);
            bool    neg =  is_negative((int64_t)operand);
            double out = ((double)m) * pow(10, e);
            if (neg)
                out *= -1.0f;

            if (precision > 0)
            {
                char fmtstr[10];
                fmtstr[0] = '%';
                fmtstr[1] = '.';
                snprintf(fmtstr+2, 8, "%dg", precision);
                return write_num(out, fmtstr);
            }
            return write_num(out, "%g");
        }
        case 6:
        {
            // STR
            uint32_t ptr = (operand) >> 32U;
            uint32_t len = (operand) & 0xFFFFFFFFU;

            if (NOT_IN_BOUNDS(ptr, len, memory_length))
                return OUT_OF_BOUNDS;

            ssize_t nul = findNul(memory + ptr, len);
            if (nul < 0)
                return NOT_A_STRING;

            if (nul > write_len - 1)
                return TOO_SMALL;

            // defensive
            size_t bytecount = std::min((uint32_t)nul, std::min(len, write_len -
1)); memcpy(memory + write_ptr, memory + ptr, bytecount);
            *(memory + write_ptr + bytecount) = '\0';
            return bytecount + 1 + lhscount;
        }
        default:
            return INVALID_ARGUMENT;
    }
}
*/
