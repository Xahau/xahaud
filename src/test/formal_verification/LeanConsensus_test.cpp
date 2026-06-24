//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#if defined(XAHAUD_ENABLE_FORMAL_VERIFICATION)

#include <xrpld/app/consensus/ConsensusExtensions.h>
#include <xrpld/consensus/ConsensusExtensionsTick.h>
#include <xrpld/consensus/ConsensusParms.h>
#include <xrpl/beast/unit_test.h>

#include <lean/lean.h>

#include <cstdint>
#include <mutex>
#include <stdexcept>

extern "C" {
void
lean_initialize_runtime_module();

lean_object*
initialize_xahau__consensus_XahauConsensus_FFI(
    uint8_t builtin,
    lean_object* world);

std::uint64_t
xahau_byzantine_bound(std::uint64_t count);

std::uint64_t
xahau_participant_threshold(std::uint64_t count);

std::uint64_t
xahau_quorum_threshold(std::uint64_t count);

std::uint64_t
xahau_safe_quorum_threshold(std::uint64_t count);

std::uint64_t
xahau_safe_participant_threshold(std::uint64_t count);

std::uint64_t
xahau_entropy_gate_threshold_for_view(
    std::uint64_t effectiveView,
    std::uint64_t originalView);

std::uint8_t
xahau_select_entropy_tier(
    std::uint64_t fromUNLReport,
    std::uint64_t participantCount,
    std::uint64_t effectiveView,
    std::uint64_t originalView);

std::uint64_t
xahau_aligned_participants(
    std::uint64_t aligned,
    std::uint64_t localIsMember,
    std::uint64_t localPublished);

std::uint8_t
xahau_quorum_aligned(
    std::uint64_t threshold,
    std::uint64_t aligned,
    std::uint64_t localIsMember,
    std::uint64_t localPublished);

std::uint8_t
xahau_full_observation(std::uint64_t peersSeen, std::uint64_t txConverged);

std::uint8_t
xahau_export_gate_proceed(
    std::uint64_t alignedParticipants,
    std::uint64_t quorumThreshold,
    std::uint64_t fullObservation);

std::uint64_t
xahau_disabled_cap(std::uint64_t originalView);

std::uint64_t
xahau_effective_view(std::uint64_t originalView, std::uint64_t disabled);
}

namespace ripple {
namespace test {

namespace {

void
initializeLean()
{
    static std::once_flag once;
    std::call_once(once, [] {
        lean_initialize_runtime_module();

        auto* result = initialize_xahau__consensus_XahauConsensus_FFI(
            1, lean_io_mk_world());
        if (lean_io_result_is_error(result))
        {
            lean_dec_ref(result);
            throw std::runtime_error{"failed to initialize XahauConsensus.FFI"};
        }
        lean_dec_ref(result);
    });
}

}  // namespace

class LeanConsensus_test : public beast::unit_test::suite
{
public:
    void
    testThresholdFormulaDrift()
    {
        testcase("Lean/C++ consensus threshold formula drift");

        initializeLean();

        for (std::uint64_t count = 0; count <= 1024; ++count)
        {
            BEAST_EXPECT(xahau_byzantine_bound(count) == count / 5);
            BEAST_EXPECT(
                xahau_participant_threshold(count) ==
                calculateParticipantThreshold(count));
            BEAST_EXPECT(
                xahau_quorum_threshold(count) ==
                calculateQuorumThreshold(count));
        }
    }

    void
    testSelectorAndGateDrift()
    {
        testcase("Lean/C++ entropy selector and gate drift");

        initializeLean();

        for (std::uint64_t effectiveView = 0; effectiveView <= 40;
             ++effectiveView)
        {
            auto const expectedQuorum = effectiveView == 0
                ? 1
                : calculateQuorumThreshold(effectiveView);
            BEAST_EXPECT(
                xahau_safe_quorum_threshold(effectiveView) == expectedQuorum);

            for (std::uint64_t originalView = 0; originalView <= 40;
                 ++originalView)
            {
                auto const expectedParticipant = originalView == 0
                    ? 1
                    : calculateParticipantThreshold(originalView);

                BEAST_EXPECT(
                    xahau_safe_participant_threshold(originalView) ==
                    expectedParticipant);
                BEAST_EXPECT(
                    xahau_entropy_gate_threshold_for_view(
                        effectiveView, originalView) ==
                    ConsensusExtensions::entropyGateThresholdForView(
                        effectiveView, originalView));

                for (std::uint64_t participantCount = 0; participantCount <= 45;
                     ++participantCount)
                {
                    for (bool fromUNLReport : {false, true})
                    {
                        BEAST_EXPECT(
                            xahau_select_entropy_tier(
                                fromUNLReport ? 1 : 0,
                                participantCount,
                                effectiveView,
                                originalView) ==
                            static_cast<std::uint8_t>(
                                ConsensusExtensions::selectEntropyTierForView(
                                    fromUNLReport,
                                    participantCount,
                                    effectiveView,
                                    originalView)));
                    }
                }
            }
        }
    }

    void
    testSidecarAndExportGateDrift()
    {
        testcase("Lean/C++ sidecar alignment and export gate drift");

        initializeLean();

        for (std::uint64_t aligned = 0; aligned <= 12; ++aligned)
        {
            for (std::uint64_t threshold = 0; threshold <= 12; ++threshold)
            {
                for (bool localIsMember : {false, true})
                {
                    for (bool localPublished : {false, true})
                    {
                        detail::SidecarPeerAlignment state;
                        state.aligned = aligned;
                        state.localPublished = localIsMember && localPublished;

                        BEAST_EXPECT(
                            xahau_aligned_participants(
                                aligned,
                                localIsMember ? 1 : 0,
                                localPublished ? 1 : 0) ==
                            state.alignedParticipants());
                        BEAST_EXPECT(
                            (xahau_quorum_aligned(
                                 threshold,
                                 aligned,
                                 localIsMember ? 1 : 0,
                                 localPublished ? 1 : 0) != 0) ==
                            state.quorumAligned(threshold));
                    }
                }

                for (bool fullObservation : {false, true})
                {
                    BEAST_EXPECT(
                        (xahau_export_gate_proceed(
                             aligned, threshold, fullObservation ? 1 : 0) !=
                         0) == (aligned >= threshold));
                }
            }
        }

        for (std::uint64_t peersSeen = 0; peersSeen <= 12; ++peersSeen)
        {
            for (std::uint64_t txConverged = 0; txConverged <= 12;
                 ++txConverged)
            {
                detail::SidecarPeerAlignment state;
                state.peersSeen = peersSeen;
                state.txConverged = txConverged;

                BEAST_EXPECT(
                    (xahau_full_observation(peersSeen, txConverged) != 0) ==
                    state.fullObservation());
            }
        }
    }

    void
    testNunlCapDrift()
    {
        testcase("Lean/C++ NegativeUNL cap arithmetic drift");

        initializeLean();

        for (std::uint64_t originalView = 0; originalView <= 1024;
             ++originalView)
        {
            auto const expectedCap = (originalView + 3) / 4;
            BEAST_EXPECT(xahau_disabled_cap(originalView) == expectedCap);

            for (std::uint64_t disabled = 0; disabled <= 16; ++disabled)
            {
                auto const expectedEffective =
                    disabled > originalView ? 0 : originalView - disabled;
                BEAST_EXPECT(
                    xahau_effective_view(originalView, disabled) ==
                    expectedEffective);
            }
        }
    }

    void
    run() override
    {
        testThresholdFormulaDrift();
        testSelectorAndGateDrift();
        testSidecarAndExportGateDrift();
        testNunlCapDrift();
    }
};

BEAST_DEFINE_TESTSUITE(LeanConsensus, consensus, ripple);

}  // namespace test
}  // namespace ripple

#endif
