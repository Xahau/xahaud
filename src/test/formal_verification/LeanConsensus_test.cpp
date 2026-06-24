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
    run() override
    {
        testThresholdFormulaDrift();
    }
};

BEAST_DEFINE_TESTSUITE(LeanConsensus, consensus, ripple);

}  // namespace test
}  // namespace ripple

#endif
