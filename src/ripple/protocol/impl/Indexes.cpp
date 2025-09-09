//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/SeqProxy.h>
#include <ripple/protocol/digest.h>
#include <ripple/protocol/nftPageMask.h>
#include <algorithm>
#include <cassert>
#include <chrono>

namespace ripple {

/** Type-specific prefix for calculating ledger indices.

    The identifier for a given object within the ledger is calculated based
    on some object-specific parameters. To ensure that different types of
    objects have different indices, even if they happen to use the same set
    of parameters, we use "tagged hashing" by adding a type-specific prefix.

    @note These values are part of the protocol and *CANNOT* be arbitrarily
          changed. If they were, on-ledger objects may no longer be able to
          be located or addressed.

          Additions to this list are OK, but changing existing entries to
          assign them a different values should never be needed.

          Entries that are removed should be moved to the bottom of the enum
          and marked as [[deprecated]] to prevent accidental reuse.
*/
enum class LedgerNameSpace : std::uint16_t {
    ACCOUNT = 'a',
    DIR_NODE = 'd',
    TRUST_LINE = 'r',
    OFFER = 'o',
    OWNER_DIR = 'O',
    BOOK_DIR = 'B',
    SKIP_LIST = 's',
    ESCROW = 'u',
    AMENDMENTS = 'f',
    FEE_SETTINGS = 'e',
    TICKET = 'T',
    SIGNER_LIST = 'S',
    PAYMENT_CHANNEL = 'x',
    CHECK = 'C',
    DEPOSIT_PREAUTH = 'p',
    NEGATIVE_UNL = 'N',
    HOOK = 'H',
    HOOK_STATE_DIR = 'J',
    HOOK_STATE = 'v',
    HOOK_DEFINITION = 'D',
    EMITTED_TXN = 'E',
    EMITTED_DIR = 'F',
    NFTOKEN_OFFER = 'q',
    NFTOKEN_BUY_OFFERS = 'h',
    NFTOKEN_SELL_OFFERS = 'i',
    URI_TOKEN = 'U',
    IMPORT_VLSEQ = 'I',
    UNL_REPORT = 'R',

    // No longer used or supported. Left here to reserve the space
    // to avoid accidental reuse.
    CONTRACT [[deprecated]] = 'c',
    GENERATOR [[deprecated]] = 'g',
    NICKNAME [[deprecated]] = 'n',
};

template <class... Args>
static uint256
indexHash(hash_options const& opts, LedgerNameSpace space, Args const&... args)
{
    auto start = std::chrono::high_resolution_clock::now();

    // Track by namespace
    getHashStats()
        .indexSha512HalfBySpace[static_cast<std::uint16_t>(space)]
        .fetch_add(1, std::memory_order_relaxed);

    auto result = sha512Half(opts, safe_cast<std::uint16_t>(space), args...);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count();
    getHashStats().indexSha512HalfTimeNs.fetch_add(
        duration, std::memory_order_relaxed);

    return result;
}

uint256
getBookBase(hash_options const& opts, Book const& book)
{
    assert(isConsistent(book));

    auto const index = indexHash(
        opts,
        LedgerNameSpace::BOOK_DIR,
        book.in.currency,
        book.out.currency,
        book.in.account,
        book.out.account);

    // Return with quality 0.
    auto k = keylet::quality({ltDIR_NODE, index}, 0);

    return k.key;
}

uint256
getQualityNext(uint256 const& uBase)
{
    static constexpr uint256 nextq(
        "0000000000000000000000000000000000000000000000010000000000000000");
    return uBase + nextq;
}

std::uint64_t
getQuality(uint256 const& uBase)
{
    // VFALCO [base_uint] This assumes a certain storage format
    return boost::endian::big_to_native(((std::uint64_t*)uBase.end())[-1]);
}

uint256
getTicketIndex(
    hash_options const& opts,
    AccountID const& account,
    std::uint32_t ticketSeq)
{
    return indexHash(
        opts, LedgerNameSpace::TICKET, account, std::uint32_t(ticketSeq));
}

uint256
getTicketIndex(
    hash_options const& opts,
    AccountID const& account,
    SeqProxy ticketSeq)
{
    assert(ticketSeq.isTicket());
    return getTicketIndex(opts, account, ticketSeq.value());
}

//------------------------------------------------------------------------------

namespace keylet {

Keylet
emittedDir(hash_options const& opts) noexcept
{
    return {ltDIR_NODE, indexHash(opts, LedgerNameSpace::EMITTED_DIR)};
}

Keylet
hookStateDir(
    hash_options const& opts,
    AccountID const& id,
    uint256 const& ns) noexcept
{
    return {
        ltDIR_NODE, indexHash(opts, LedgerNameSpace::HOOK_STATE_DIR, id, ns)};
}

Keylet
emittedTxn(hash_options const& opts, uint256 const& id) noexcept
{
    return {ltEMITTED_TXN, indexHash(opts, LedgerNameSpace::EMITTED_TXN, id)};
}

Keylet
hook(hash_options const& opts, AccountID const& id) noexcept
{
    return {ltHOOK, indexHash(opts, LedgerNameSpace::HOOK, id)};
}

Keylet
hookDefinition(hash_options const& opts, uint256 const& hash) noexcept
{
    return {
        ltHOOK_DEFINITION,
        indexHash(opts, LedgerNameSpace::HOOK_DEFINITION, hash)};
}

Keylet
hookState(
    hash_options const& opts,
    AccountID const& id,
    uint256 const& key,
    uint256 const& ns) noexcept
{
    return {
        ltHOOK_STATE,
        indexHash(opts, LedgerNameSpace::HOOK_STATE, id, key, ns)};
}

Keylet
account(hash_options const& opts, AccountID const& id) noexcept
{
    return Keylet{
        ltACCOUNT_ROOT, indexHash(opts, LedgerNameSpace::ACCOUNT, id)};
}

Keylet
child(hash_options const& opts, uint256 const& key) noexcept
{
    // Note: This just wraps an existing key - might not need hash_options
    // but keeping it for consistency
    return {ltCHILD, key};
}

Keylet
skip(hash_options const& opts) noexcept
{
    return {ltLEDGER_HASHES, indexHash(opts, LedgerNameSpace::SKIP_LIST)};
}

Keylet
skip(hash_options const& opts, LedgerIndex ledger) noexcept
{
    return {
        ltLEDGER_HASHES,
        indexHash(
            opts,
            LedgerNameSpace::SKIP_LIST,
            std::uint32_t(static_cast<std::uint32_t>(ledger) >> 16))};
}

Keylet
amendments(hash_options const& opts) noexcept
{
    return {ltAMENDMENTS, indexHash(opts, LedgerNameSpace::AMENDMENTS)};
}

Keylet
import_vlseq(hash_options const& opts, PublicKey const& key) noexcept
{
    return {
        ltIMPORT_VLSEQ, indexHash(opts, LedgerNameSpace::IMPORT_VLSEQ, key)};
}

Keylet
fees(hash_options const& opts) noexcept
{
    return {ltFEE_SETTINGS, indexHash(opts, LedgerNameSpace::FEE_SETTINGS)};
}

Keylet
negativeUNL(hash_options const& opts) noexcept
{
    return {ltNEGATIVE_UNL, indexHash(opts, LedgerNameSpace::NEGATIVE_UNL)};
}

Keylet
UNLReport(hash_options const& opts) noexcept
{
    return {ltUNL_REPORT, indexHash(opts, LedgerNameSpace::UNL_REPORT)};
}

Keylet
book_t::operator()(hash_options const& opts, Book const& b) const
{
    return {ltDIR_NODE, getBookBase(opts, b)};
}

Keylet
line(
    hash_options const& opts,
    AccountID const& id0,
    AccountID const& id1,
    Currency const& currency) noexcept
{
    // There is code in SetTrust that calls us with id0 == id1, to allow users
    // to locate and delete such "weird" trustlines. If we remove that code, we
    // could enable this assert:
    // assert(id0 != id1);

    // A trust line is shared between two accounts; while we typically think
    // of this as an "issuer" and a "holder" the relationship is actually fully
    // bidirectional.
    //
    // So that we can generate a unique ID for a trust line, regardess of which
    // side of the line we're looking at, we define a "canonical" order for the
    // two accounts (smallest then largest)  and hash them in that order:
    auto const accounts = std::minmax(id0, id1);

    return {
        ltRIPPLE_STATE,
        indexHash(
            opts,
            LedgerNameSpace::TRUST_LINE,
            accounts.first,
            accounts.second,
            currency)};
}

Keylet
offer(
    hash_options const& opts,
    AccountID const& id,
    UInt32or256 const& seq) noexcept
{
    return {ltOFFER, indexHash(opts, LedgerNameSpace::OFFER, id, seq)};
}

Keylet
offer(hash_options const& opts, uint256 const& key) noexcept
{
    // Note: This just wraps an existing key - might not need hash_options
    // but keeping it for consistency
    return {ltOFFER, key};
}

Keylet
quality(Keylet const& k, std::uint64_t q) noexcept
{
    assert(k.type == ltDIR_NODE);

    // Indexes are stored in big endian format: they print as hex as stored.
    // Most significant bytes are first and the least significant bytes
    // represent adjacent entries. We place the quality, in big endian format,
    // in the 8 right most bytes; this way, incrementing goes to the next entry
    // for indexes.
    uint256 x = k.key;

    // FIXME This is ugly and we can and should do better...
    ((std::uint64_t*)x.end())[-1] = boost::endian::native_to_big(q);

    return {ltDIR_NODE, x};
}

Keylet
next_t::operator()(Keylet const& k) const
{
    assert(k.type == ltDIR_NODE);
    return {ltDIR_NODE, getQualityNext(k.key)};
}

Keylet
ticket_t::operator()(
    hash_options const& opts,
    AccountID const& id,
    std::uint32_t ticketSeq) const
{
    return {ltTICKET, getTicketIndex(opts, id, ticketSeq)};
}

Keylet
ticket_t::operator()(
    hash_options const& opts,
    AccountID const& id,
    SeqProxy ticketSeq) const
{
    return {ltTICKET, getTicketIndex(opts, id, ticketSeq)};
}

// This function is presently static, since it's never accessed from anywhere
// else. If we ever support multiple pages of signer lists, this would be the
// keylet used to locate them.
static Keylet
signers(
    hash_options const& opts,
    AccountID const& account,
    std::uint32_t page) noexcept
{
    return {
        ltSIGNER_LIST,
        indexHash(opts, LedgerNameSpace::SIGNER_LIST, account, page)};
}

Keylet
signers(hash_options const& opts, AccountID const& account) noexcept
{
    return signers(opts, account, 0);
}

Keylet
check(
    hash_options const& opts,
    AccountID const& id,
    UInt32or256 const& seq) noexcept
{
    return {ltCHECK, indexHash(opts, LedgerNameSpace::CHECK, id, seq)};
}

Keylet
check(hash_options const& opts, uint256 const& key) noexcept
{
    // Note: This just wraps an existing key - might not need hash_options
    // but keeping it for consistency. The ledger_index could be stored
    // in the Keylet for future use.
    return {ltCHECK, key};
}

Keylet
depositPreauth(
    hash_options const& opts,
    AccountID const& owner,
    AccountID const& preauthorized) noexcept
{
    return {
        ltDEPOSIT_PREAUTH,
        indexHash(
            opts, LedgerNameSpace::DEPOSIT_PREAUTH, owner, preauthorized)};
}

//------------------------------------------------------------------------------

Keylet
unchecked(uint256 const& key) noexcept
{
    return {ltANY, key};
}

Keylet
ownerDir(hash_options const& opts, AccountID const& id) noexcept
{
    return {ltDIR_NODE, indexHash(opts, LedgerNameSpace::OWNER_DIR, id)};
}

Keylet
page(hash_options const& opts, uint256 const& key, std::uint64_t index) noexcept
{
    if (index == 0)
        return {ltDIR_NODE, key};

    return {ltDIR_NODE, indexHash(opts, LedgerNameSpace::DIR_NODE, key, index)};
}

Keylet
escrow(
    hash_options const& opts,
    AccountID const& src,
    UInt32or256 const& seq) noexcept
{
    return {ltESCROW, indexHash(opts, LedgerNameSpace::ESCROW, src, seq)};
}

Keylet
payChan(
    hash_options const& opts,
    AccountID const& src,
    AccountID const& dst,
    UInt32or256 const& seq) noexcept
{
    return {
        ltPAYCHAN,
        indexHash(opts, LedgerNameSpace::PAYMENT_CHANNEL, src, dst, seq)};
}

Keylet
nftpage_min(hash_options const& opts, AccountID const& owner)
{
    // Note: This might need to use opts for future hash migration
    std::array<std::uint8_t, 32> buf{};
    std::memcpy(buf.data(), owner.data(), owner.size());
    return {ltNFTOKEN_PAGE, uint256{buf}};
}

Keylet
nftpage_max(hash_options const& opts, AccountID const& owner)
{
    // Note: This might need to use opts for future hash migration
    uint256 id = nft::pageMask;
    std::memcpy(id.data(), owner.data(), owner.size());
    return {ltNFTOKEN_PAGE, id};
}

Keylet
nftpage(Keylet const& k, uint256 const& token)
{
    assert(k.type == ltNFTOKEN_PAGE);
    return {ltNFTOKEN_PAGE, (k.key & ~nft::pageMask) + (token & nft::pageMask)};
}

Keylet
nftoffer(
    hash_options const& opts,
    AccountID const& owner,
    UInt32or256 const& seq)
{
    return {
        ltNFTOKEN_OFFER,
        indexHash(opts, LedgerNameSpace::NFTOKEN_OFFER, owner, seq)};
}

Keylet
nftoffer(hash_options const& opts, uint256 const& offer)
{
    // Note: This just wraps an existing key - might not need hash_options
    // but keeping it for consistency
    return {ltNFTOKEN_OFFER, offer};
}

Keylet
nft_buys(hash_options const& opts, uint256 const& id) noexcept
{
    return {
        ltDIR_NODE, indexHash(opts, LedgerNameSpace::NFTOKEN_BUY_OFFERS, id)};
}

Keylet
nft_sells(hash_options const& opts, uint256 const& id) noexcept
{
    return {
        ltDIR_NODE, indexHash(opts, LedgerNameSpace::NFTOKEN_SELL_OFFERS, id)};
}

Keylet
uritoken(hash_options const& opts, AccountID const& issuer, Blob const& uri)
{
    return {
        ltURI_TOKEN,
        indexHash(
            opts,
            LedgerNameSpace::URI_TOKEN,
            issuer,
            Slice{uri.data(), uri.size()})};
}

}  // namespace keylet

}  // namespace ripple
