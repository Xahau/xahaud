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

#ifndef RIPPLE_TEST_JTX_MULTISIGN_H_INCLUDED
#define RIPPLE_TEST_JTX_MULTISIGN_H_INCLUDED

#include <test/jtx/Account.h>
#include <test/jtx/amount.h>
#include <test/jtx/owners.h>
#include <test/jtx/tags.h>
#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>

namespace ripple {
namespace test {
namespace jtx {

/** A signer in a SignerList */
struct signer
{
    std::uint32_t weight;
    Account account;
    std::optional<uint256> tag;

    signer(
        Account account_,
        std::uint32_t weight_ = 1,
        std::optional<uint256> tag_ = std::nullopt)
        : weight(weight_), account(std::move(account_)), tag(std::move(tag_))
    {
        if (tag_)
            tag = *tag_;
    }
};

Json::Value
signers(
    Account const& account,
    std::uint32_t quorum,
    std::vector<signer> const& v);

/** Remove a signer list. */
Json::Value
signers(Account const& account, none_t);

//------------------------------------------------------------------------------

/** Set a multisignature on a JTx. */
class msig
{
public:
    // Recursive signer structure
    struct Signer
    {
        Account acct;
        Account sig;  // For leaf signers (same as acct for master key)
        std::vector<std::shared_ptr<Signer>> nested;  // For nested signers

        // Leaf signer constructor (regular signing)
        Signer(Account const& masterSig) : acct(masterSig), sig(masterSig)
        {
        }

        // Leaf signer constructor (with different signing key)
        Signer(Account const& acct_, Account const& regularSig)
            : acct(acct_), sig(regularSig)
        {
        }

        // Nested signer constructor
        Signer(
            Account const& acct_,
            std::vector<std::shared_ptr<Signer>> nested_)
            : acct(acct_), sig(acct_), nested(std::move(nested_))
        {
        }

        bool
        isNested() const
        {
            return !nested.empty();
        }

        AccountID
        id() const
        {
            return acct.id();
        }
    };

    using SignerPtr = std::shared_ptr<Signer>;

    // For backward compatibility
    struct Reg
    {
        Account acct;
        Account sig;

        Reg(Account const& masterSig) : acct(masterSig), sig(masterSig)
        {
        }
        Reg(Account const& acct_, Account const& regularSig)
            : acct(acct_), sig(regularSig)
        {
        }
        Reg(char const* masterSig) : acct(masterSig), sig(masterSig)
        {
        }
        Reg(char const* acct_, char const* regularSig)
            : acct(acct_), sig(regularSig)
        {
        }

        bool
        operator<(Reg const& rhs) const
        {
            return acct < rhs.acct;
        }

        // Convert to Signer
        SignerPtr
        toSigner() const
        {
            return std::make_shared<Signer>(acct, sig);
        }
    };

    std::vector<SignerPtr> signers;

public:
    // Initializer list constructor - resolves brace-init ambiguity
    msig(std::initializer_list<SignerPtr> signers_)
        : msig(std::vector<SignerPtr>(signers_))
    {
        // handled by :
    }

    // Direct constructor with SignerPtr vector
    explicit msig(std::vector<SignerPtr> signers_);

    // Backward compatibility constructor
    msig(std::vector<Reg> signers_);

    // Variadic constructor for backward compatibility
    template <class AccountType, class... Accounts>
        requires std::convertible_to<AccountType, Reg>
    explicit msig(AccountType&& a0, Accounts&&... aN)
        : msig{std::vector<Reg>{
              std::forward<AccountType>(a0),
              std::forward<Accounts>(aN)...}}
    {
    }

    void
    operator()(Env&, JTx& jt) const;
};

// Helper functions to create signers - renamed to avoid conflict with sig()
// transaction modifier
inline msig::SignerPtr
msigner(Account const& acct)
{
    return std::make_shared<msig::Signer>(acct);
}

inline msig::SignerPtr
msigner(Account const& acct, Account const& signingKey)
{
    return std::make_shared<msig::Signer>(acct, signingKey);
}

// Create nested signer with initializer list
template <typename... Args>
inline msig::SignerPtr
msigner(Account const& acct, Args&&... args)
{
    std::vector<msig::SignerPtr> nested;
    (nested.push_back(std::forward<Args>(args)), ...);
    return std::make_shared<msig::Signer>(acct, std::move(nested));
}

//------------------------------------------------------------------------------

/** The number of signer lists matches. */
using siglists = owner_count<ltSIGNER_LIST>;

}  // namespace jtx
}  // namespace test
}  // namespace ripple

#endif
