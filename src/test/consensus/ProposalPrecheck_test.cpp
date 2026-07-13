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

        //@@start test-proposal-precheck-legacy-ok
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
                BEAST_EXPECT(
                    precheck.position->txSetHash == position.txSetHash);
        }
        //@@end test-proposal-precheck-legacy-ok

        testcase("feature predicates are lazy");
        {
            int entropyChecks = 0;
            int exportChecks = 0;
            auto const entropyEnabled = [&] {
                ++entropyChecks;
                return false;
            };
            auto const exportEnabled = [&] {
                ++exportChecks;
                return false;
            };

            protocol::TMProposeSet plainSet;
            setPreviousLedger(plainSet);
            ExtendedPosition plain{makeHash("plain-lazy-position")};
            setPosition(plainSet, plain);
            BEAST_EXPECT(
                detail::checkProposalExtensions(
                    plainSet, entropyEnabled, exportEnabled)
                    .result == ok);
            BEAST_EXPECT(entropyChecks == 0);
            BEAST_EXPECT(exportChecks == 0);

            protocol::TMProposeSet entropySet;
            setPreviousLedger(entropySet);
            ExtendedPosition entropy{makeHash("entropy-lazy-position")};
            entropy.myCommitment = makeHash("lazy-commitment");
            setPosition(entropySet, entropy);
            BEAST_EXPECT(
                detail::checkProposalExtensions(
                    entropySet, entropyEnabled, exportEnabled)
                    .result == entropyDisabled);
            BEAST_EXPECT(entropyChecks == 1);
            BEAST_EXPECT(exportChecks == 0);

            protocol::TMProposeSet exportSet;
            setPreviousLedger(exportSet);
            ExtendedPosition exportPos{makeHash("export-lazy-position")};
            exportPos.exportSignaturesHash = makeHash("lazy-export-shares");
            setPosition(exportSet, exportPos);
            BEAST_EXPECT(
                detail::checkProposalExtensions(
                    exportSet, entropyEnabled, exportEnabled)
                    .result == exportDisabled);
            BEAST_EXPECT(entropyChecks == 1);
            BEAST_EXPECT(exportChecks == 1);
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

        //@@start test-proposal-extension-feature-gating
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

            protocol::TMProposeSet observedSet;
            setPreviousLedger(observedSet);
            ExtendedPosition observed{makeHash("observed-position")};
            observed.observedParticipantsHash =
                makeHash("observed-participants");
            setPosition(observedSet, observed);
            BEAST_EXPECT(
                detail::checkProposalExtensions(observedSet, false, true)
                    .result == ok);
            BEAST_EXPECT(
                detail::checkProposalExtensions(observedSet, true, false)
                    .result == ok);
            BEAST_EXPECT(
                detail::checkProposalExtensions(observedSet, false, false)
                    .result == extensionDiagnosticsDisabled);

            protocol::TMProposeSet exportSet;
            setPreviousLedger(exportSet);
            ExtendedPosition exportPos{makeHash("export-position")};
            exportPos.exportSignaturesHash = makeHash("export-shares");
            setPosition(exportSet, exportPos);
            BEAST_EXPECT(
                detail::checkProposalExtensions(exportSet, true, false)
                    .result == exportDisabled);
        }
        //@@end test-proposal-extension-feature-gating

        //@@start test-proposal-export-signature-binding
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

            // A single oversized blob is rejected before its bytes are hashed,
            // even though the count is within maxPendingExports. This bounds
            // the pre-auth SHA512 work on the proposal ingress path.
            protocol::TMProposeSet oversized;
            setPreviousLedger(oversized);
            std::string const bigSig(
                ExportLimits::maxExportSignatureBytes + 1, 'x');
            std::vector<std::string> const bigSigs{bigSig};
            ExtendedPosition oversizedPos{
                makeHash("export-oversized-position")};
            oversizedPos.exportSignaturesHash =
                proposalExportSignaturesHash(bigSigs);
            setPosition(oversized, oversizedPos);
            oversized.add_exportsignatures(bigSig);
            BEAST_EXPECT(
                detail::checkProposalExtensions(oversized, true, true).result ==
                oversizedExportSignature);

            // A maximum-size blob is still accepted.
            protocol::TMProposeSet maxSized;
            setPreviousLedger(maxSized);
            std::string const maxSig(
                ExportLimits::maxExportSignatureBytes, 'x');
            std::vector<std::string> const maxSigs{maxSig};
            ExtendedPosition maxPos{makeHash("export-maxsize-position")};
            maxPos.exportSignaturesHash = proposalExportSignaturesHash(maxSigs);
            setPosition(maxSized, maxPos);
            maxSized.add_exportsignatures(maxSig);
            BEAST_EXPECT(
                detail::checkProposalExtensions(maxSized, true, true).result ==
                ok);
        }
        //@@end test-proposal-export-signature-binding

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
            check(
                exportDisabled,
                "Proposal: export fields while featureExport disabled",
                "export fields disabled");
            check(
                tooManyExportSignatures,
                "Proposal: too many export signatures",
                "too many export sigs");
            check(
                oversizedExportSignature,
                "Proposal: oversized export signature",
                "oversized export sig");
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

BEAST_DEFINE_TESTSUITE(ProposalPrecheck, consensus, ripple);

}  // namespace test
}  // namespace ripple
