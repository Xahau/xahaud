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

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/contract.h>
#include <xrpl/beast/utility/rngfill.h>
#include <xrpl/crypto/RFC1751.h>
#include <xrpl/crypto/csprng.h>
#include <xrpl/crypto/secure_erase.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/digest.h>
#include <algorithm>
#include <cstring>
#include <iterator>

namespace ripple {

Seed::~Seed()
{
    secure_erase(buf_.data(), buf_.size());
}

Seed
randomSeed()
{
    std::array<std::uint8_t, 16> buffer;
    beast::rngfill(buffer, crypto_prng());
    Seed seed(buffer);
    secure_erase(buffer.data(), buffer.size());
    return seed;
}

Seed
generateSeed(std::string const& passPhrase)
{
    sha512_half_hasher_s h;
    h(passPhrase.data(), passPhrase.size());
    auto const digest = sha512_half_hasher::result_type(h);
    return Seed({digest.data(), 16});
}

template <>
std::optional<Seed>
parseBase58(std::string const& s)
{
    auto const result = decodeBase58Token(s, TokenType::FamilySeed);
    if (result.empty())
        return std::nullopt;
    if (result.size() != 16)
        return std::nullopt;
    return Seed(makeSlice(result));
}

std::optional<Seed>
parseGenericSeed(std::string const& str, bool rfc1751)
{
    if (str.empty())
        return std::nullopt;

    if (parseBase58<AccountID>(str) ||
        parseBase58<PublicKey>(TokenType::NodePublic, str) ||
        parseBase58<PublicKey>(TokenType::AccountPublic, str) ||
        parseBase58<SecretKey>(TokenType::NodePrivate, str) ||
        parseBase58<SecretKey>(TokenType::AccountSecret, str))
    {
        return std::nullopt;
    }

    {
        uint128 seed;

        if (seed.parseHex(str))
            return Seed{Slice(seed.data(), seed.size())};
    }

    if (auto seed = parseBase58<Seed>(str))
        return seed;

    if (rfc1751)
    {
        if (auto key = rfc1751::keyFromEnglish(str))
        {
            std::reverse(key->begin(), key->end());
            return Seed{*key};
        }
    }

    return generateSeed(str);
}

std::string
seedAs1751(Seed const& seed)
{
    std::array<std::uint8_t, 16> key;
    std::reverse_copy(seed.begin(), seed.end(), key.begin());
    return rfc1751::englishFromKey(key).value_or("");
}

}  // namespace ripple
