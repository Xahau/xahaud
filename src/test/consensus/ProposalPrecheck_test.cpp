//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpld/app/consensus/ProposalPrecheck.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/digest.h>

#include <cstring>
#include <string_view>
#include <vector>

namespace ripple {
namespace test {

namespace {

uint256
makeHash(char const* label)
{
    return sha512Half(Slice(label, std::strlen(label)));
}

void
setPreviousLedger(protocol::TMProposeSet& set)
{
    auto const prev = makeHash("proposal-precheck-prev");
    set.set_previousledger(prev.data(), prev.size());
}

void
setPosition(protocol::TMProposeSet& set, ExtendedPosition const& position)
{
    Serializer s;
    position.add(s);
    set.set_currenttxhash(s.data(), s.size());
}

}  // namespace

class ProposalPrecheck_test : public beast::unit_test::suite
{
public:
    void
    run() override
    {
        using enum detail::ProposalPrecheckResult;

        //@@start test-peer-proposal-authentication
        testcase("cluster transport does not bypass proposal authentication");
        {
            BEAST_EXPECT(detail::proposalSignatureAccepted(false, true));
            BEAST_EXPECT(detail::proposalSignatureAccepted(true, true));
            BEAST_EXPECT(!detail::proposalSignatureAccepted(false, false));
            BEAST_EXPECT(!detail::proposalSignatureAccepted(true, false));
        }
        //@@end test-peer-proposal-authentication

        //@@start test-proposal-precheck-legacy-ok
        testcase("legacy and extended ok");
        {
            protocol::TMProposeSet set;
            setPreviousLedger(set);
            ExtendedPosition position{makeHash("legacy-position")};
            setPosition(set, position);

            auto const precheck = detail::checkProposalExtensions(set, false);
            BEAST_EXPECT(precheck.result == ok);
            BEAST_EXPECT(precheck.position);
            if (precheck.position)
                BEAST_EXPECT(
                    precheck.position->txSetHash == position.txSetHash);
        }
        //@@end test-proposal-precheck-legacy-ok

        testcase("feature predicates are lazy");
        {
            int entropyChecks = 0;
            auto const entropyEnabled = [&] {
                ++entropyChecks;
                return false;
            };

            protocol::TMProposeSet plainSet;
            setPreviousLedger(plainSet);
            ExtendedPosition plain{makeHash("plain-lazy-position")};
            setPosition(plainSet, plain);
            BEAST_EXPECT(
                detail::checkProposalExtensions(plainSet, entropyEnabled)
                    .result == ok);
            BEAST_EXPECT(entropyChecks == 0);

            protocol::TMProposeSet entropySet;
            setPreviousLedger(entropySet);
            ExtendedPosition entropy{makeHash("entropy-lazy-position")};
            entropy.myCommitment = makeHash("lazy-commitment");
            setPosition(entropySet, entropy);
            BEAST_EXPECT(
                detail::checkProposalExtensions(entropySet, entropyEnabled)
                    .result == entropyDisabled);
            BEAST_EXPECT(entropyChecks == 1);

            // Flag bit 0x10 carries no position field: the parse fails before
            // any feature predicate runs.
            protocol::TMProposeSet unknownBitSet;
            setPreviousLedger(unknownBitSet);
            Serializer unknownBit;
            unknownBit.addBitString(makeHash("unknown-bit-lazy-position"));
            unknownBit.add8(0x10);
            unknownBit.addBitString(makeHash("unknown-bit-lazy-payload"));
            unknownBitSet.set_currenttxhash(
                unknownBit.data(), unknownBit.size());
            BEAST_EXPECT(
                detail::checkProposalExtensions(unknownBitSet, entropyEnabled)
                    .result == badPosition);
            BEAST_EXPECT(entropyChecks == 1);
        }

        testcase("malformed hashes and extended payload");
        {
            protocol::TMProposeSet set;
            setPreviousLedger(set);
            set.set_currenttxhash("short", 5);
            BEAST_EXPECT(
                detail::checkProposalExtensions(set, true).result == badHashes);

            set.clear_currenttxhash();
            std::string malformed(uint256::size(), '\0');
            malformed.push_back(static_cast<char>(0x80));
            set.set_currenttxhash(malformed.data(), malformed.size());
            BEAST_EXPECT(
                detail::checkProposalExtensions(set, true).result ==
                badPosition);

            protocol::TMProposeSet badPrev;
            ExtendedPosition position{makeHash("bad-prev-position")};
            setPosition(badPrev, position);
            badPrev.set_previousledger("short", 5);
            BEAST_EXPECT(
                detail::checkProposalExtensions(badPrev, true).result ==
                badHashes);
        }

        //@@start test-proposal-extension-feature-gating
        testcase("feature gating");
        {
            protocol::TMProposeSet entropySet;
            setPreviousLedger(entropySet);
            ExtendedPosition entropy{makeHash("entropy-position")};
            entropy.myCommitment = makeHash("commitment");
            setPosition(entropySet, entropy);
            BEAST_EXPECT(
                detail::checkProposalExtensions(entropySet, false).result ==
                entropyDisabled);

            protocol::TMProposeSet observedSet;
            setPreviousLedger(observedSet);
            ExtendedPosition observed{makeHash("observed-position")};
            observed.observedParticipantsHash =
                makeHash("observed-participants");
            setPosition(observedSet, observed);
            BEAST_EXPECT(
                detail::checkProposalExtensions(observedSet, true).result ==
                ok);
            BEAST_EXPECT(
                detail::checkProposalExtensions(observedSet, false).result ==
                extensionDiagnosticsDisabled);
        }
        //@@end test-proposal-extension-feature-gating

        testcase("rejection diagnostics");
        {
            BEAST_EXPECT(!detail::proposalPrecheckRejection(ok));

            auto const check = [&](detail::ProposalPrecheckResult result,
                                   char const* logMessage,
                                   char const* feeReason) {
                auto const rejection =
                    detail::proposalPrecheckRejection(result);
                BEAST_EXPECT(rejection);
                if (rejection)
                {
                    BEAST_EXPECT(
                        std::string_view{rejection->logMessage} == logMessage);
                    BEAST_EXPECT(
                        std::string_view{rejection->feeReason} == feeReason);
                }
            };

            check(badHashes, "Proposal: malformed", "bad hashes");
            check(
                badPosition,
                "Proposal: malformed extended position",
                "bad proposal position");
            check(
                extensionDiagnosticsDisabled,
                "Proposal: extension diagnostics while consensus extensions "
                "disabled",
                "extension diagnostics disabled");
            check(
                entropyDisabled,
                "Proposal: entropy fields while featureConsensusEntropy "
                "disabled",
                "entropy fields disabled");
            BEAST_EXPECT(!detail::proposalPrecheckRejection(
                static_cast<detail::ProposalPrecheckResult>(255)));
        }
    }
};

BEAST_DEFINE_TESTSUITE(ProposalPrecheck, consensus, ripple);

}  // namespace test
}  // namespace ripple
