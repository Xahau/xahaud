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

#include <test/jtx/multisign.h>
#include <test/jtx/utility.h>
#include <xrpl/basics/contract.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/jss.h>
#include <optional>
#include <sstream>

namespace ripple {
namespace test {
namespace jtx {

Json::Value
signers(
    Account const& account,
    std::uint32_t quorum,
    std::vector<signer> const& v)
{
    Json::Value jv;
    jv[jss::Account] = account.human();
    jv[jss::TransactionType] = jss::SignerListSet;
    jv[sfSignerQuorum.getJsonName()] = quorum;
    auto& ja = jv[sfSignerEntries.getJsonName()];
    for (std::size_t i = 0; i < v.size(); ++i)
    {
        auto const& e = v[i];
        auto& je = ja[i][sfSignerEntry.getJsonName()];
        je[jss::Account] = e.account.human();
        je[sfSignerWeight.getJsonName()] = e.weight;
        if (e.tag)
            je[sfWalletLocator.getJsonName()] = to_string(*e.tag);
    }
    return jv;
}

Json::Value
signers(Account const& account, none_t)
{
    Json::Value jv;
    jv[jss::Account] = account.human();
    jv[jss::TransactionType] = jss::SignerListSet;
    jv[sfSignerQuorum.getJsonName()] = 0;
    return jv;
}

//------------------------------------------------------------------------------

// Helper function to recursively sort nested signers
void
sortSignersRecursive(std::vector<msig::SignerPtr>& signers)
{
    // Sort current level by account ID
    std::sort(
        signers.begin(),
        signers.end(),
        [](msig::SignerPtr const& lhs, msig::SignerPtr const& rhs) {
            return lhs->id() < rhs->id();
        });

    // Recursively sort nested signers for each signer at this level
    for (auto& signer : signers)
    {
        if (signer->isNested() && !signer->nested.empty())
        {
            sortSignersRecursive(signer->nested);
        }
    }
}

msig::msig(std::vector<msig::SignerPtr> signers_) : signers(std::move(signers_))
{
    // Recursively sort all signers at all nesting levels
    // This ensures account IDs are in strictly ascending order at each level
    sortSignersRecursive(signers);
}

msig::msig(std::vector<msig::Reg> signers_)
{
    // Convert Reg vector to SignerPtr vector for backward compatibility
    signers.reserve(signers_.size());
    for (auto const& s : signers_)
        signers.push_back(s.toSigner());

    // Recursively sort all signers at all nesting levels
    // This ensures account IDs are in strictly ascending order at each level
    sortSignersRecursive(signers);
}

void
msig::operator()(Env& env, JTx& jt) const
{
    auto const mySigners = signers;
    jt.signer = [mySigners, &env](Env&, JTx& jtx) {
        jtx[sfSigningPubKey.getJsonName()] = "";
        std::optional<STObject> st;
        try
        {
            st = parse(jtx.jv);
        }
        catch (parse_error const&)
        {
            env.test.log << pretty(jtx.jv) << std::endl;
            Rethrow();
        }

        // Recursive function to build signer JSON
        std::function<Json::Value(SignerPtr const&)> buildSignerJson;
        buildSignerJson = [&](SignerPtr const& signer) -> Json::Value {
            Json::Value jo;
            jo[jss::Account] = signer->acct.human();

            if (signer->isNested())
            {
                // For nested signers, we use the already-sorted nested vector
                // (sorted during construction via sortSignersRecursive)
                // This ensures account IDs are in strictly ascending order
                auto& subJs = jo[sfSigners.getJsonName()];
                for (std::size_t i = 0; i < signer->nested.size(); ++i)
                {
                    auto& subJo = subJs[i][sfSigner.getJsonName()];
                    subJo = buildSignerJson(signer->nested[i]);
                }
            }
            else
            {
                // This is a leaf signer - add signature
                jo[jss::SigningPubKey] = strHex(signer->sig.pk().slice());

                Serializer ss{buildMultiSigningData(*st, signer->acct.id())};
                auto const sig = ripple::sign(
                    *publicKeyType(signer->sig.pk().slice()),
                    signer->sig.sk(),
                    ss.slice());
                jo[sfTxnSignature.getJsonName()] =
                    strHex(Slice{sig.data(), sig.size()});
            }

            return jo;
        };

        auto& js = jtx[sfSigners.getJsonName()];
        for (std::size_t i = 0; i < mySigners.size(); ++i)
        {
            auto& jo = js[i][sfSigner.getJsonName()];
            jo = buildSignerJson(mySigners[i]);
        }
    };
}

}  // namespace jtx
}  // namespace test
}  // namespace ripple
