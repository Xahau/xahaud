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
#include <xrpld/app/misc/NegativeUNLVote.h>
#include <xrpld/consensus/ConsensusExtensionsTick.h>
#include <xrpld/consensus/ConsensusParms.h>
#include <xrpl/beast/unit_test.h>

#include <lean/lean.h>

#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>

extern "C" {
//@@start formal-ffi-c-abi-decls
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

std::uint8_t
xahau_strict_intersection_safe(
    std::uint64_t activeView,
    std::uint64_t byzantineUniverse,
    std::uint64_t threshold);

std::uint8_t
xahau_nonvacuous_strict_intersection_safe(
    std::uint64_t activeView,
    std::uint64_t byzantineUniverse,
    std::uint64_t threshold);

std::uint8_t
xahau_participant_band_nonempty(
    std::uint64_t effectiveView,
    std::uint64_t originalView);

std::uint64_t
xahau_export_quorum_overlap_lower_bound(std::uint64_t activeView);

std::uint8_t
xahau_export_quorum_safe_under_nunl_cap(
    std::uint64_t originalView,
    std::uint64_t effectiveView,
    std::uint64_t disabled);

std::uint64_t
xahau_active_aligned_count_mask(
    std::uint64_t count,
    std::uint64_t activeMask,
    std::uint64_t alignedMask);

std::uint8_t
xahau_quorum_aligned_mask(
    std::uint64_t threshold,
    std::uint64_t count,
    std::uint64_t activeMask,
    std::uint64_t alignedMask,
    std::uint64_t localIsMember,
    std::uint64_t localPublished);

std::uint64_t
xahau_naive_sixty_percent_threshold(std::uint64_t count);

std::uint8_t
xahau_naive_sixty_percent_is_safe(std::uint64_t count);
//@@end formal-ffi-c-abi-decls
}

namespace ripple {
namespace test {

namespace {

bool
strictIntersectionSafeCpp(
    std::uint64_t activeView,
    std::uint64_t byzantineUniverse,
    std::uint64_t threshold)
{
    return activeView + byzantineUniverse / 5 < 2 * threshold;
}

bool
participantBandNonemptyCpp(
    std::uint64_t effectiveView,
    std::uint64_t originalView)
{
    return calculateParticipantThreshold(originalView) <
        calculateQuorumThreshold(effectiveView);
}

bool
maskBit(std::uint64_t mask, std::uint64_t peer)
{
    return ((mask >> peer) & 1) != 0;
}

std::uint64_t
activeAlignedCountMaskCpp(
    std::uint64_t count,
    std::uint64_t activeMask,
    std::uint64_t alignedMask)
{
    std::uint64_t result = 0;
    for (std::uint64_t peer = 0; peer < count; ++peer)
    {
        if (maskBit(activeMask, peer) && maskBit(alignedMask, peer))
            ++result;
    }
    return result;
}

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

        auto const max = std::numeric_limits<std::uint64_t>::max();
        for (std::uint64_t count :
             {max / 100 - 1,
              max / 100,
              max / 80 - 1,
              max / 80,
              max / 5 - 1,
              max / 5,
              max / 2 - 1,
              max / 2,
              max - 1,
              max})
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
            auto const expectedQuorum = safeQuorumThreshold(effectiveView);
            BEAST_EXPECT(
                xahau_safe_quorum_threshold(effectiveView) == expectedQuorum);

            for (std::uint64_t originalView = 0; originalView <= 40;
                 ++originalView)
            {
                auto const expectedParticipant =
                    safeParticipantThreshold(originalView);

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
                        auto const expectedAligned =
                            detail::sidecarAlignedParticipants(
                                aligned, localIsMember, localPublished);

                        BEAST_EXPECT(
                            xahau_aligned_participants(
                                aligned,
                                localIsMember ? 1 : 0,
                                localPublished ? 1 : 0) == expectedAligned);
                        BEAST_EXPECT(
                            (xahau_quorum_aligned(
                                 threshold,
                                 aligned,
                                 localIsMember ? 1 : 0,
                                 localPublished ? 1 : 0) != 0) ==
                            detail::sidecarQuorumAligned(
                                aligned,
                                localIsMember,
                                localPublished,
                                threshold));
                    }
                }

                for (bool fullObservation : {false, true})
                {
                    BEAST_EXPECT(
                        (xahau_export_gate_proceed(
                             aligned, threshold, fullObservation ? 1 : 0) !=
                         0) ==
                        detail::exportSigSetQuorumAligned(aligned, threshold));
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
                    detail::sidecarFullObservation(peersSeen, txConverged));
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
            auto const expectedCap =
                NegativeUNLVote::maxNegativeUNLListed(originalView);
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

        auto const max = std::numeric_limits<std::uint64_t>::max();
        for (std::uint64_t originalView :
             {max / 4 - 1, max / 4, max / 2, max - 1, max})
        {
            BEAST_EXPECT(
                xahau_disabled_cap(originalView) ==
                NegativeUNLVote::maxNegativeUNLListed(originalView));
        }
    }

    void
    testViewUniverseDrift()
    {
        testcase("Lean/C++ view-universe safety predicate drift");

        initializeLean();

        for (std::uint64_t effectiveView = 0; effectiveView <= 40;
             ++effectiveView)
        {
            for (std::uint64_t originalView = 0; originalView <= 40;
                 ++originalView)
            {
                auto const threshold =
                    calculateParticipantThreshold(originalView);
                BEAST_EXPECT(
                    (xahau_strict_intersection_safe(
                         effectiveView, originalView, threshold) != 0) ==
                    strictIntersectionSafeCpp(
                        effectiveView, originalView, threshold));
                BEAST_EXPECT(
                    (xahau_nonvacuous_strict_intersection_safe(
                         effectiveView, originalView, threshold) != 0) ==
                    (threshold <= effectiveView &&
                     strictIntersectionSafeCpp(
                         effectiveView, originalView, threshold)));
                BEAST_EXPECT(
                    (xahau_participant_band_nonempty(
                         effectiveView, originalView) != 0) ==
                    participantBandNonemptyCpp(effectiveView, originalView));
            }
        }
    }

    void
    testExportQuorumDrift()
    {
        testcase("Lean/C++ export quorum safety predicate drift");

        initializeLean();

        for (std::uint64_t activeView = 0; activeView <= 64; ++activeView)
        {
            auto const quorum = calculateQuorumThreshold(activeView);
            auto const expectedOverlap =
                2 * quorum > activeView ? 2 * quorum - activeView : 0;
            BEAST_EXPECT(
                xahau_export_quorum_overlap_lower_bound(activeView) ==
                expectedOverlap);
        }

        for (std::uint64_t originalView = 0; originalView <= 64; ++originalView)
        {
            auto const cap =
                NegativeUNLVote::maxNegativeUNLListed(originalView);
            for (std::uint64_t disabled = 0; disabled <= cap + 2; ++disabled)
            {
                auto const effectiveView =
                    disabled > originalView ? 0 : originalView - disabled;
                auto const expected =
                    disabled <= cap && effectiveView > 0 &&
                    strictIntersectionSafeCpp(
                        effectiveView,
                        originalView,
                        calculateQuorumThreshold(effectiveView));
                BEAST_EXPECT(
                    (xahau_export_quorum_safe_under_nunl_cap(
                         originalView, effectiveView, disabled) != 0) ==
                    expected);
            }
        }

        struct NunlPremiseCase
        {
            std::uint64_t originalView;
            std::uint64_t effectiveView;
            std::uint64_t disabled;
        };

        for (auto const& c : {
                 NunlPremiseCase{20, 15, 4},  // effective view should be 16
                 NunlPremiseCase{20, 17, 4},  // effective view should be 16
                 NunlPremiseCase{20, 14, 6},  // disabled is above cap
                 NunlPremiseCase{10, 6, 4},   // disabled is above cap
             })
        {
            BEAST_EXPECT(
                xahau_export_quorum_safe_under_nunl_cap(
                    c.originalView, c.effectiveView, c.disabled) == 0);
        }
    }

    void
    testSidecarMaskDrift()
    {
        testcase("Lean/C++ active-view sidecar mask drift");

        initializeLean();

        struct MaskCase
        {
            std::uint64_t count;
            std::uint64_t activeMask;
            std::uint64_t alignedMask;
        };

        auto const checkMaskCase = [&](MaskCase const& c) {
            auto const aligned =
                activeAlignedCountMaskCpp(c.count, c.activeMask, c.alignedMask);
            BEAST_EXPECT(
                xahau_active_aligned_count_mask(
                    c.count, c.activeMask, c.alignedMask) == aligned);

            for (std::uint64_t threshold = 0; threshold <= 12; ++threshold)
            {
                for (bool localIsMember : {false, true})
                {
                    for (bool localPublished : {false, true})
                    {
                        BEAST_EXPECT(
                            (xahau_quorum_aligned_mask(
                                 threshold,
                                 c.count,
                                 c.activeMask,
                                 c.alignedMask,
                                 localIsMember ? 1 : 0,
                                 localPublished ? 1 : 0) != 0) ==
                            detail::sidecarQuorumAligned(
                                aligned,
                                localIsMember,
                                localPublished,
                                threshold));
                    }
                }
            }
        };

        for (std::uint64_t count = 0; count <= 6; ++count)
        {
            auto const maxMask = std::uint64_t{1} << count;
            for (std::uint64_t activeMask = 0; activeMask < maxMask;
                 ++activeMask)
            {
                for (std::uint64_t alignedMask = 0; alignedMask < maxMask;
                     ++alignedMask)
                {
                    checkMaskCase(MaskCase{count, activeMask, alignedMask});
                }
            }
        }

        for (auto const& c : {
                 MaskCase{8, 0b10110110, 0b11111111},
                 MaskCase{12, 0b101010101010, 0b111100001111},
                 MaskCase{63, std::uint64_t{1} << 62, std::uint64_t{1} << 62},
                 MaskCase{64, std::uint64_t{1} << 63, std::uint64_t{1} << 63},
                 MaskCase{
                     64,
                     (std::uint64_t{1} << 63) | 0b1011,
                     (std::uint64_t{1} << 63) | 0b0110},
             })
        {
            checkMaskCase(c);
        }
    }

    void
    testNaiveSixtyPercentRegression()
    {
        testcase("Lean/C++ naive 60 percent threshold regression anchors");

        initializeLean();

        for (std::uint64_t count = 0; count <= 1024; ++count)
        {
            auto const naive = (count * 60 + 99) / 100;
            BEAST_EXPECT(xahau_naive_sixty_percent_threshold(count) == naive);
            BEAST_EXPECT(
                (xahau_naive_sixty_percent_is_safe(count) != 0) ==
                strictIntersectionSafeCpp(count, count, naive));

            if (count > 0 && count % 5 == 0)
            {
                BEAST_EXPECT(!strictIntersectionSafeCpp(count, count, naive));
                BEAST_EXPECT(strictIntersectionSafeCpp(
                    count, count, calculateParticipantThreshold(count)));
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
        testViewUniverseDrift();
        testExportQuorumDrift();
        testSidecarMaskDrift();
        testNaiveSixtyPercentRegression();
    }
};

BEAST_DEFINE_TESTSUITE(LeanConsensus, consensus, ripple);

}  // namespace test
}  // namespace ripple

#endif
