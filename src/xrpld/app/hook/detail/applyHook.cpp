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
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/st.h>
#include <xrpl/protocol/tokens.h>
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
    ripple::uint256 const& hookCanEmit,
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

    auto const sleAccount = view.peek(hookCtx.result.accountKeylet);
    if (!sleAccount && view.rules().enabled(featureExtendedHookState))
        return tefINTERNAL;

    uint16_t const hookStateScale = sleAccount->isFieldPresent(sfHookStateScale)
        ? sleAccount->getFieldU16(sfHookStateScale)
        : 1;

    uint32_t maxSize = hook::maxHookStateDataSize(hookStateScale);
    if (read_len > maxSize)
        return TOO_BIG;

    uint256 ns = nread_len == 0 ? hookCtx.result.hookNamespace
                                : uint256::fromVoid(memory + nread_ptr);

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

    uint256 ns = nread_len == 0 ? hookCtx.result.hookNamespace
                                : uint256::fromVoid(memory + nread_ptr);

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

    HOOK_TEARDOWN();
}

// Return the tt (Transaction Type) numeric code of the originating transaction
DEFINE_HOOK_FUNCTION(int64_t, otxn_type)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    return api.otxn_type();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, otxn_slot, uint32_t slot_into)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.otxn_slot(slot_into);
    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}
// Return the burden of the originating transaction... this will be 1 unless the
// originating transaction was itself an emitted transaction from a previous
// hook invocation
DEFINE_HOOK_FUNCTION(int64_t, otxn_burden)
{
    HOOK_SETUP();
    return api.otxn_burden();
    HOOK_TEARDOWN();
}

// Return the generation of the originating transaction... this will be 1 unless
// the originating transaction was itself an emitted transaction from a previous
// hook invocation
DEFINE_HOOK_FUNCTION(int64_t, otxn_generation)
{
    HOOK_SETUP();
    return api.otxn_generation();
    HOOK_TEARDOWN();
}

// Return the generation of a hypothetically emitted transaction from this hook
DEFINE_HOOK_FUNCTION(int64_t, etxn_generation)
{
    // proxy only, no setup or teardown
    return hookCtx.api().etxn_generation();
}

// Return the current ledger sequence number
DEFINE_HOOK_FUNCTION(int64_t, ledger_seq)
{
    HOOK_SETUP();

    return api.ledger_seq();

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

    auto const hash = api.ledger_last_hash();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, hash.data(), 32, memory, memory_length);

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, ledger_last_time)
{
    HOOK_SETUP();

    return api.ledger_last_time();

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, slot_clear, uint32_t slot_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.slot_clear(slot_no);
    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, slot_count, uint32_t slot_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.slot_count(slot_no);
    if (!result)
        return result.error();

    return result.value();

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

    Bytes data{memory + read_ptr, memory + read_ptr + read_len};
    auto const result = api.slot_set(data, slot_into);
    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, slot_size, uint32_t slot_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.slot_size(slot_no);
    if (!result)
        return result.error();

    return result.value();

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

    auto const result = api.slot_subarray(parent_slot, array_id, new_slot);
    if (!result)
        return result.error();

    return result.value();

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

    auto const result = api.slot_subfield(parent_slot, field_id, new_slot);
    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, slot_type, uint32_t slot_no, uint32_t flags)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, slot_float, uint32_t slot_no)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.slot_float(slot_no);
    if (!result)
        return result.error();

    return result.value();

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    prepare,
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

    auto const result = api.hook_hash(hook_no);
    if (!result)
        return result.error();
    auto const& hash = result.value();

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

    auto const result = api.hook_account();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 20, result.data(), 20, memory, memory_length);

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

    auto const result = api.ledger_nonce();
    if (!result)
        return result.error();
    auto const& hash = result.value();

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

    auto const result = api.ledger_keylet(*klLo, *klHi);
    if (!result)
        return result.error();
    auto kl_out = result.value();

    return serialize_keylet(kl_out, memory, write_ptr, write_len);

    HOOK_TEARDOWN();
}

// Reserve one or more transactions for emission from the running hook
DEFINE_HOOK_FUNCTION(int64_t, etxn_reserve, uint32_t count)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.etxn_reserve(count);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}

// Compute the burden of an emitted transaction based on a number of factors
DEFINE_HOOK_FUNCTION(int64_t, etxn_burden)
{
    HOOK_SETUP();
    auto const burden = api.etxn_burden();
    if (!burden)
        return burden.error();
    return burden.value();
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

    auto const hash =
        api.util_sha512h(ripple::Slice{memory + read_ptr, read_len});

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, 32, hash.data(), 32, memory, memory_length);

    HOOK_TEARDOWN();
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

    Bytes data{memory + read_ptr, memory + read_ptr + read_len};
    auto const result = api.sto_subfield(data, field_id);
    if (!result)
        return result.error();
    auto const& pair = result.value();
    return (uint64_t(pair.first) << 32U) + (uint32_t)pair.second;

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

    Bytes data{memory + read_ptr, memory + read_ptr + read_len};
    auto const result = api.sto_subarray(data, index_id);
    if (!result)
        return result.error();
    auto const& pair = result.value();
    return (uint64_t(pair.first) << 32U) + (uint32_t)pair.second;

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

    auto const result = api.util_accid(raddr);
    if (!result)
        return result.error();
    auto const& accountID = result.value();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, accountID.data(), 20, memory, memory_length);

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

    Bytes data{read_ptr + memory, read_ptr + read_len + memory};
    auto const result = api.sto_validate(data);
    if (!result)
        return result.error();
    return result.value() ? 1 : 0;

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

    HOOK_TEARDOWN();
}

// Return the current fee base of the current ledger (multiplied by a margin)
DEFINE_HOOK_FUNCTION(int64_t, fee_base)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    return api.fee_base();

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
    HOOK_SETUP();
    if (NOT_IN_BOUNDS(read_ptr, read_len, memory_length))
        return OUT_OF_BOUNDS;
    ripple::Slice tx{
        reinterpret_cast<const void*>(read_ptr + memory), read_len};
    auto const fee_base = api.etxn_fee_base(tx);
    if (!fee_base)
        return fee_base.error();
    return fee_base.value();
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

    auto const result = api.etxn_details(memory + write_ptr);
    if (!result)
        return result.error();
    return result.value();

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

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_set, int32_t exp, int64_t mantissa)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    auto const result = api.float_set(exp, mantissa);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
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

    auto const result = api.float_int(float1, decimal_places, absolute);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_multiply, int64_t float1, int64_t float2)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    auto const result = api.float_multiply(float1, float2);
    if (!result)
        return result.error();
    return result.value();

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

    auto const result =
        api.float_mulratio(float1, round_up, numerator, denominator);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_negate, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    return api.float_negate(float1);

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

    auto const result = api.float_compare(float1, float2, mode);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_sum, int64_t float1, int64_t float2)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    auto const result = api.float_sum(float1, float2);
    if (!result)
        return result.error();
    return result.value();

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

    Bytes data{read_ptr + memory, read_ptr + read_len + memory};

    auto const result = api.float_sto_set(data);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_divide, int64_t float1, int64_t float2)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);
    RETURN_IF_INVALID_FLOAT(float2);

    auto const result = api.float_divide(float1, float2);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_one)
{
    return hookCtx.api().float_one();
}

DEFINE_HOOK_FUNCTION(int64_t, float_invert, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result = api.float_invert(float1);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_mantissa, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result = api.float_mantissa(float1);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_sign, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    return api.float_sign(float1);

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_log, int64_t float1)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result = api.float_log(float1);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, float_root, int64_t float1, uint32_t n)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    RETURN_IF_INVALID_FLOAT(float1);

    auto const result = api.float_root(float1, n);
    if (!result)
        return result.error();
    return result.value();

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

    Bytes paramName{read_ptr + memory, read_ptr + read_len + memory};

    auto const result = api.otxn_param(paramName);
    if (!result)
        return result.error();
    auto const& val = result.value();

    if (val.size() > write_len)
        return TOO_SMALL;

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, val.data(), val.size(), memory, memory_length);

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

    Bytes paramName{read_ptr + memory, read_ptr + read_len + memory};

    auto const result = api.hook_param(paramName);

    if (!result)
        return result.error();

    auto const& val = result.value();

    WRITE_WASM_MEMORY_AND_RETURN(
        write_ptr, write_len, val.data(), val.size(), memory, memory_length);

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

    ripple::uint256 hash = ripple::uint256::fromVoid(memory + read_ptr);

    auto const result = api.hook_skip(hash, flags);
    if (!result)
        return result.error();
    return result.value();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, hook_pos)
{
    return hookCtx.api().hook_pos();
}

DEFINE_HOOK_FUNCTION(int64_t, hook_again)
{
    HOOK_SETUP();

    auto const result = api.hook_again();

    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(int64_t, meta_slot, uint32_t slot_into)
{
    HOOK_SETUP();

    auto const result = api.meta_slot(slot_into);
    if (!result)
        return result.error();

    return result.value();

    HOOK_TEARDOWN();
}

DEFINE_HOOK_FUNCTION(
    int64_t,
    xpop_slot,
    uint32_t slot_into_tx,
    uint32_t slot_into_meta)
{
    HOOK_SETUP();

    auto const result = api.xpop_slot(slot_into_tx, slot_into_meta);
    if (!result)
        return result.error();

    return std::get<0>(result.value()) << 16U | std::get<1>(result.value());

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
