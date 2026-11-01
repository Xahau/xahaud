//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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

#include <xrpl/basics/contract.h>
#include <xrpl/protocol/CreditNote.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/JSONTxSignatures.h>
#include <xrpl/protocol/UintTypes.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace ripple {

namespace {

[[noreturn]] void
fail(std::string const& why)
{
    Throw<std::runtime_error>("creditnote: " + why);
}

constexpr bool
digit(char c)
{
    return c >= '0' && c <= '9';
}

constexpr bool
space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// A note's values are strings, bare integers, and - for an issued Amount -
// one object of strings. Nothing else.
struct Val
{
    enum class Kind : std::uint8_t { string, number, object };

    Kind kind = Kind::object;
    std::string text;               // string contents, or the number token
    std::vector<std::string> keys;  // object member names, in source order
    std::vector<Val> items;         // object member values
};

// A reader for the note language only. jsontx_strict has already accepted
// the document - one object, printable ASCII, no escapes, no duplicates,
// bounded integers - so this reader's job is shape, not sanitation; it still
// refuses anything outside that language rather than assume it.
class Reader
{
    std::string_view s_;
    std::size_t p_ = 0;

    void
    skip()
    {
        while (p_ < s_.size() && space(s_[p_]))
            ++p_;
    }

    char
    peek() const
    {
        if (p_ >= s_.size())
            fail("unexpected end of note");
        return s_[p_];
    }

    void
    expect(char c, char const* what)
    {
        if (peek() != c)
            fail(what);
        ++p_;
    }

    std::string
    string()
    {
        expect('"', "expected a string");
        std::size_t const b = p_;
        for (;;)
        {
            if (p_ >= s_.size())
                fail("unterminated string");
            auto const c = static_cast<unsigned char>(s_[p_]);
            if (c == '"')
                break;
            if (c == '\\' || c < 0x20 || c > 0x7E)
                fail("strings are printable ASCII without escapes");
            ++p_;
        }
        std::string r(s_.substr(b, p_ - b));
        ++p_;  // closing quote
        return r;
    }

    std::string
    number()
    {
        std::size_t const b = p_;
        if (peek() == '-')
            ++p_;
        if (!digit(peek()))
            fail("malformed number");
        if (s_[p_] == '0')
        {
            ++p_;
            if (p_ < s_.size() && digit(s_[p_]))
                fail("malformed number");
        }
        else
        {
            while (p_ < s_.size() && digit(s_[p_]))
                ++p_;
        }
        if (p_ < s_.size() && (s_[p_] == '.' || s_[p_] == 'e' || s_[p_] == 'E'))
            fail("numbers are integers");
        return std::string(s_.substr(b, p_ - b));
    }

    Val
    value(int depth)
    {
        skip();
        char const c = peek();
        if (c == '{')
            return object(depth + 1);
        Val v;
        if (c == '"')
        {
            v.kind = Val::Kind::string;
            v.text = string();
            return v;
        }
        if (c == '-' || digit(c))
        {
            v.kind = Val::Kind::number;
            v.text = number();
            return v;
        }
        fail("unexpected token");
    }

    Val
    object(int depth)
    {
        // the note, and at most one object inside it (an issued Amount)
        if (depth > 2)
            fail("nested too deeply");

        Val v;
        v.kind = Val::Kind::object;
        expect('{', "expected an object");
        skip();
        if (peek() == '}')
        {
            ++p_;
            return v;
        }
        for (;;)
        {
            skip();
            auto key = string();
            if (std::find(v.keys.begin(), v.keys.end(), key) != v.keys.end())
                fail("duplicate member " + key);
            skip();
            expect(':', "expected ':'");
            v.items.push_back(value(depth));
            v.keys.push_back(std::move(key));
            skip();
            char const c = peek();
            if (c == ',')
            {
                ++p_;
                continue;
            }
            if (c == '}')
            {
                ++p_;
                return v;
            }
            fail("expected ',' or '}'");
        }
    }

public:
    explicit Reader(std::string_view s) : s_(s)
    {
    }

    Val
    parse()
    {
        skip();
        if (p_ >= s_.size() || s_[p_] != '{')
            fail("a note is a JSON object");
        auto root = object(1);
        skip();
        if (p_ != s_.size())
            fail("trailing data after note");
        return root;
    }
};

Val const&
member(Val const& obj, std::string_view key)
{
    for (std::size_t i = 0; i < obj.keys.size(); ++i)
        if (obj.keys[i] == key)
            return obj.items[i];
    fail("missing " + std::string(key));
}

std::string const&
str(Val const& obj, std::string_view key)
{
    auto const& v = member(obj, key);
    if (v.kind != Val::Kind::string)
        fail(std::string(key) + " must be a string");
    return v.text;
}

// A non-negative bare integer, already known to be well formed and at most
// jsontx_max_int, as a number.
std::uint64_t
unsignedValue(Val const& v, std::string_view key)
{
    if (v.kind != Val::Kind::number)
        fail(std::string(key) + " must be a bare integer");
    if (v.text.empty() || v.text[0] == '-')
        fail(std::string(key) + " must not be negative");
    std::uint64_t n = 0;
    for (char const c : v.text)
        n = n * 10 + static_cast<std::uint64_t>(c - '0');
    return n;
}

}  // namespace

//------------------------------------------------------------------------------

CreditNote
parseCreditNote(std::string_view raw)
{
    if (raw.size() > maxCreditNoteBytes)
        fail("note too large");

    // The note is a JsonTx document first: same character set, integer
    // rules and bounds as a plaintext-signed transaction preimage.
    jsontx_strict(raw);

    auto const root = Reader(raw).parse();

    static constexpr std::array<std::string_view, 5> allowed{
        "NetworkID", "Account", "Amount", "IssueTime", "Expiration"};

    // Exactly the five, each exactly once (the reader refused duplicates).
    // Anything else - including a TransactionType - is not a note.
    if (root.keys.size() != allowed.size())
        fail(
            "a note has exactly NetworkID, Account, Amount, IssueTime and "
            "Expiration");
    for (auto const& k : root.keys)
        if (std::find(allowed.begin(), allowed.end(), k) == allowed.end())
            fail("unknown member " + k);

    CreditNote note;

    {
        auto const n = unsignedValue(member(root, "NetworkID"), "NetworkID");
        if (n > std::numeric_limits<std::uint32_t>::max())
            fail("NetworkID out of range");
        note.networkID = static_cast<std::uint32_t>(n);
    }

    {
        auto const id = parseBase58<AccountID>(str(root, "Account"));
        if (!id || *id == noAccount() || isXRP(*id))
            fail("Account is not an r-address");
        note.account = *id;
    }

    {
        auto const& a = member(root, "Amount");
        if (a.kind == Val::Kind::number)
        {
            // drops of the native currency; at most jsontx_max_int, which
            // is below the total supply
            auto const drops = unsignedValue(a, "Amount");
            note.amount = STAmount{XRPAmount{static_cast<std::int64_t>(drops)}};
        }
        else if (a.kind == Val::Kind::object)
        {
            if (a.keys.size() != 3)
                fail(
                    "an issued Amount has exactly currency, issuer and "
                    "value");

            Currency currency;
            if (!to_currency(currency, str(a, "currency")) || isXRP(currency) ||
                isBadCurrency(currency))
                fail("bad currency");

            auto const issuer = parseBase58<AccountID>(str(a, "issuer"));
            if (!issuer || *issuer == noAccount() || isXRP(*issuer))
                fail("issuer is not an r-address");

            try
            {
                note.amount =
                    amountFromString(Issue{currency, *issuer}, str(a, "value"));
            }
            catch (std::exception const&)
            {
                fail("bad value");
            }
        }
        else
        {
            fail("Amount is drops or an issued amount");
        }

        if (note.amount.signum() <= 0 || !isLegalNet(note.amount))
            fail("Amount must be positive");
    }

    note.issueTime = jsontx_iso(str(root, "IssueTime"));
    note.expiration = jsontx_iso(str(root, "Expiration"));

    if (note.expiration <= note.issueTime)
        fail("Expiration must be after IssueTime");
    if (note.expiration - note.issueTime > creditNoteMaxLifeMs)
        fail(
            "a note lives at most " +
            std::to_string(creditNoteMaxLifeMs / 60000) + " minutes");

    return note;
}

}  // namespace ripple
