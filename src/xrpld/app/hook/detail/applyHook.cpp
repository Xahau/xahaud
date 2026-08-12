#include <xrpld/app/hook/HookAPI.h>
#include <xrpld/app/hook/HookHostOperations.h>
#include <xrpld/app/hook/HookWasmEngine.h>
#include <xrpld/app/hook/QuickJSHookRuntime.h>
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
#include <xrpl/hook/HookArtifact.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/st.h>
#include <xrpl/protocol/tokens.h>
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

    TxType const& tt = tx.getTxnType();

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

            for (auto const& offer : {bo, so})
            {
                if (offer)
                {
                    ADD_TSH(offer->getAccountID(sfOwner), tshSTRONG);
                    if (offer->isFieldPresent(sfDestination))
                        ADD_TSH(offer->getAccountID(sfDestination), tshSTRONG);
                }
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
            if (tx.isFieldPresent(sfOwner))
                ADD_TSH(tx.getAccountID(sfOwner), tshWEAK);
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

using namespace hook::hook_float;
using hook::Bytes;




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

    auto const artifact = hook::artifact::parse(makeSlice(wasm));
    if (!artifact || artifact->hookApiVersion != hookApiVersion)
    {
        JLOG(j.warn())
            << "HookError[" << HC_ACC()
            << "]: Stored Hook artifact is invalid or disagrees with "
               "sfHookApiVersion"
            << (artifact ? "" : ": ")
            << (artifact ? std::string_view{}
                         : hook::artifact::toString(artifact.error()));
        hookCtx.result.exitType = hook_api::ExitType::WASM_ERROR;
        return hookCtx.result;
    }

    switch (artifact->kind)
    {
        case hook::artifact::Kind::legacyWasm: {
            auto engine = makeHookWasmEngine();
            auto const execution = engine->execute(
                artifact->payload.data(),
                artifact->payload.size(),
                isCallback,
                wasmParam,
                hookCtx,
                j);
            hookCtx.result.instructionCount = execution.instructionCount;
            if (!execution.ok)
            {
                hookCtx.result.exitType = hook_api::ExitType::WASM_ERROR;
                JLOG(j.warn())
                    << "HookError[" << HC_ACC() << "]: "
                    << execution.error.value_or("unknown WASM error");
            }
            break;
        }

        case hook::artifact::Kind::quickJSBytecode: {
            auto runtime = findQuickJSRuntime(*artifact);
            if (!runtime)
            {
                JLOG(j.warn())
                    << "HookError[" << HC_ACC()
                    << "]: QuickJS runtime profile is not registered";
                hookCtx.result.exitType = hook_api::ExitType::WASM_ERROR;
                break;
            }
            executeQuickJSBytecode(
                hookCtx,
                runtime,
                std::span{artifact->payload.data(), artifact->payload.size()},
                isCallback,
                wasmParam,
                j);
            break;
        }
    }

    JLOG(j.trace()) << "HookInfo[" << HC_ACC() << "]: "
                    << (hookCtx.result.exitType == hook_api::ExitType::ROLLBACK
                            ? "ROLLBACK"
                            : "ACCEPT")
                    << " RS: '" << hookCtx.result.exitReason.c_str()
                    << "' RC: " << hookCtx.result.exitCode;

    return hookCtx.result;
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
        meta.setFieldU8(
            sfHookResult, static_cast<uint8_t>(hookResult.exitType));
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
