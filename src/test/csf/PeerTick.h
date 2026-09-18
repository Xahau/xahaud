#ifndef RIPPLE_TEST_CSF_PEERTICK_H_INCLUDED
#define RIPPLE_TEST_CSF_PEERTICK_H_INCLUDED

#include <test/csf/Peer.h>
#include <xrpld/consensus/ConsensusExtensionsTick.h>

template <class Ctx>
ripple::ExtensionTickResult
ripple::test::csf::Peer::Extensions::onTick(Ctx const& ctx)
{
    return ripple::extensionsTick(*this, ctx);
}

#endif
