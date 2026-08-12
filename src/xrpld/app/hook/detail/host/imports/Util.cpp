#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/host/imports/HookHostImportHelpers.h>

using namespace ripple;
using hook::Bytes;

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
            // These keylet types are not yet implemented. Their
            // corresponding amendments are not yet supported on the
            // network. Each case needs a full implementation (see
            // above cases for reference) before its amendment can be
            // enabled.
            // featureXChainBridge
            case keylet_code::BRIDGE:
            case keylet_code::XCHAIN_OWNED_CLAIM_ID:
            case keylet_code::XCHAIN_OWNED_CREATE_ACCOUNT_CLAIM_ID:
            // featureMPTokensV1
            case keylet_code::MPTOKEN_ISSUANCE:
            case keylet_code::MPTOKEN:
            // featureCredentials
            case keylet_code::CREDENTIAL:
            // featurePermissionedDomains
            case keylet_code::PERMISSIONED_DOMAIN:
                return INVALID_ARGUMENT;
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
    return result.value() ? 1ULL : 0ULL;

    HOOK_TEARDOWN();
}
