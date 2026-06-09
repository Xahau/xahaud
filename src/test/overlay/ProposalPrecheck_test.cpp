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

#include <xrpld/overlay/detail/ProposalPrecheck.h>
#include <xrpl/beast/unit_test.h>
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

        testcase("legacy and extended ok");
        {
            protocol::TMProposeSet set;
            setPreviousLedger(set);
            ExtendedPosition position{makeHash("legacy-position")};
            setPosition(set, position);

            auto const precheck =
                detail::checkProposalExtensions(set, false, false);
            BEAST_EXPECT(precheck.result == ok);
            BEAST_EXPECT(precheck.position);
            if (precheck.position)
                BEAST_EXPECT(precheck.position->txSetHash == position.txSetHash);
        }

        testcase("malformed hashes and extended payload");
        {
            protocol::TMProposeSet set;
            setPreviousLedger(set);
            set.set_currenttxhash("short", 5);
            BEAST_EXPECT(
                detail::checkProposalExtensions(set, true, true).result ==
                badHashes);

            set.clear_currenttxhash();
            std::string malformed(uint256::size(), '\0');
            malformed.push_back(static_cast<char>(0x80));
            set.set_currenttxhash(malformed.data(), malformed.size());
            BEAST_EXPECT(
                detail::checkProposalExtensions(set, true, true).result ==
                badPosition);

            protocol::TMProposeSet badPrev;
            ExtendedPosition position{makeHash("bad-prev-position")};
            setPosition(badPrev, position);
            badPrev.set_previousledger("short", 5);
            BEAST_EXPECT(
                detail::checkProposalExtensions(badPrev, true, true).result ==
                badHashes);
        }

        testcase("feature gating");
        {
            protocol::TMProposeSet entropySet;
            setPreviousLedger(entropySet);
            ExtendedPosition entropy{makeHash("entropy-position")};
            entropy.myCommitment = makeHash("commitment");
            setPosition(entropySet, entropy);
            BEAST_EXPECT(
                detail::checkProposalExtensions(entropySet, false, true)
                    .result == entropyDisabled);

            protocol::TMProposeSet exportSet;
            setPreviousLedger(exportSet);
            ExtendedPosition exportPos{makeHash("export-position")};
            exportPos.exportSigSetHash = makeHash("export-sidecar");
            setPosition(exportSet, exportPos);
            BEAST_EXPECT(
                detail::checkProposalExtensions(exportSet, true, false)
                    .result == exportDisabled);
        }

        testcase("export signature binding");
        {
            protocol::TMProposeSet tooMany;
            setPreviousLedger(tooMany);
            ExtendedPosition position{makeHash("too-many-position")};
            setPosition(tooMany, position);
            for (std::uint8_t i = 0; i <= ExportLimits::maxPendingExports; ++i)
                tooMany.add_exportsignatures("sig");
            BEAST_EXPECT(
                detail::checkProposalExtensions(tooMany, true, true).result ==
                tooManyExportSignatures);

            protocol::TMProposeSet unsignedSigs;
            setPreviousLedger(unsignedSigs);
            setPosition(unsignedSigs, position);
            unsignedSigs.add_exportsignatures("sig");
            BEAST_EXPECT(
                detail::checkProposalExtensions(unsignedSigs, true, true)
                    .result == unsignedExportSignatures);

            protocol::TMProposeSet mismatch;
            setPreviousLedger(mismatch);
            ExtendedPosition mismatchPos{makeHash("mismatch-position")};
            mismatchPos.exportSignaturesHash =
                proposalExportSignaturesHash(std::vector<std::string>{"one"});
            setPosition(mismatch, mismatchPos);
            mismatch.add_exportsignatures("two");
            BEAST_EXPECT(
                detail::checkProposalExtensions(mismatch, true, true).result ==
                exportSignaturesHashMismatch);

            protocol::TMProposeSet missing;
            setPreviousLedger(missing);
            setPosition(missing, mismatchPos);
            BEAST_EXPECT(
                detail::checkProposalExtensions(missing, true, true).result ==
                missingExportSignatures);

            protocol::TMProposeSet okSet;
            setPreviousLedger(okSet);
            std::vector<std::string> const sigs{"signed-export"};
            ExtendedPosition okPos{makeHash("export-ok-position")};
            okPos.exportSignaturesHash = proposalExportSignaturesHash(sigs);
            setPosition(okSet, okPos);
            okSet.add_exportsignatures(sigs.front());
            BEAST_EXPECT(
                detail::checkProposalExtensions(okSet, true, true).result ==
                ok);
        }

        testcase("rejection diagnostics");
        {
            BEAST_EXPECT(!detail::proposalPrecheckRejection(ok));

            auto const check = [&](detail::ProposalPrecheckResult result,
                                   char const* logMessage,
                                   char const* feeReason) {
                auto const rejection = detail::proposalPrecheckRejection(result);
                BEAST_EXPECT(rejection);
                if (rejection)
                {
                    BEAST_EXPECT(
                        std::string_view{rejection->logMessage} ==
                        logMessage);
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
                entropyDisabled,
                "Proposal: entropy fields while featureConsensusEntropy disabled",
                "entropy fields disabled");
            check(
                exportDisabled,
                "Proposal: export fields while featureExport disabled",
                "export fields disabled");
            check(
                tooManyExportSignatures,
                "Proposal: too many export signatures",
                "too many export sigs");
            check(
                unsignedExportSignatures,
                "Proposal: unsigned export signatures",
                "unsigned export sigs");
            check(
                exportSignaturesHashMismatch,
                "Proposal: export signatures hash mismatch",
                "export sig hash mismatch");
            check(
                missingExportSignatures,
                "Proposal: missing signed export signatures",
                "missing export sigs");
            BEAST_EXPECT(!detail::proposalPrecheckRejection(
                static_cast<detail::ProposalPrecheckResult>(255)));
        }
    }
};

BEAST_DEFINE_TESTSUITE(ProposalPrecheck, overlay, ripple);

}  // namespace test
}  // namespace ripple
