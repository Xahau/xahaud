//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL Labs

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

#ifndef RIPPLE_TEST_JTX_REMIT_H_INCLUDED
#define RIPPLE_TEST_JTX_REMIT_H_INCLUDED

#include <ripple/protocol/STAmount.h>
#include <test/jtx/Account.h>
#include <test/jtx/Env.h>

namespace ripple {
namespace test {
namespace jtx {

namespace remit {

Json::Value
remit(
    jtx::Account const& account,
    jtx::Account const& dest,
    std::optional<std::uint32_t> const& dstTag = std::nullopt);

/** Sets the optional Amount on a JTx. */
class amts
{
private:
    std::vector<STAmount> amts_;

public:
    explicit amts(std::vector<STAmount> const& amts) : amts_(amts)
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

/** Set the optional "Blob" on a JTx */
class blob
{
private:
    std::string blob_;

public:
    explicit blob(std::string const& blob) : blob_(blob)
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

/** Sets the optional "Inform" on a JTx. */
class inform
{
private:
    jtx::Account inform_;

public:
    explicit inform(jtx::Account const& inform) : inform_(inform)
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

/** Sets the optional "URITokenIDs" on a JTx. */
class token_ids
{
private:
    std::vector<std::string> token_ids_;

public:
    explicit token_ids(std::vector<std::string> const& token_ids)
        : token_ids_(token_ids)
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

/** Set the optional "sfMintURIToken" on a JTx */
class uri
{
private:
    std::string uri_;
    std::optional<std::uint32_t> flags_;

public:
    explicit uri(
        std::string const& uri,
        std::optional<std::uint32_t> const& flags = std::nullopt)
        : uri_(uri), flags_(flags)
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

}  // namespace remit

}  // namespace jtx

}  // namespace test
}  // namespace ripple

#endif  // RIPPLE_TEST_JTX_REMIT_H_INCLUDED