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

#include <ripple/basics/contract.h>
#include <ripple/protocol/HashPrefix.h>
#include <ripple/protocol/Sign.h>
#include <ripple/protocol/UintTypes.h>
#include <ripple/protocol/jss.h>
#include <optional>
#include <sstream>
#include <test/jtx/multisign.h>
#include <test/jtx/utility.h>

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

msig::msig(std::vector<msig::SignerPtr> signers_) : signers(std::move(signers_))
{
    // Signatures must be applied in sorted order.
    std::sort(
        signers.begin(),
        signers.end(),
        [](SignerPtr const& lhs, SignerPtr const& rhs) {
            return lhs->id() < rhs->id();
        });
}

msig::msig(std::vector<msig::Reg> signers_)
{
    // Convert Reg vector to SignerPtr vector for backward compatibility
    signers.reserve(signers_.size());
    for (auto const& s : signers_)
        signers.push_back(s.toSigner());

    // Sort
    std::sort(
        signers.begin(),
        signers.end(),
        [](SignerPtr const& lhs, SignerPtr const& rhs) {
            return lhs->id() < rhs->id();
        });
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
                // This is a nested signer - add subsigners
                auto sortedNested = signer->nested;
                std::sort(
                    sortedNested.begin(),
                    sortedNested.end(),
                    [](SignerPtr const& lhs, SignerPtr const& rhs) {
                        return lhs->id() < rhs->id();
                    });

                auto& subJs = jo[sfSigners.getJsonName()];
                for (std::size_t i = 0; i < sortedNested.size(); ++i)
                {
                    auto& subJo = subJs[i][sfSigner.getJsonName()];
                    subJo = buildSignerJson(sortedNested[i]);
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
