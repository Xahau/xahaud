#ifndef RIPPLE_APP_MISC_RUNTIMEFAULTRANDOM_H_INCLUDED
#define RIPPLE_APP_MISC_RUNTIMEFAULTRANDOM_H_INCLUDED

#include <xrpld/app/main/Application.h>
#include <xrpld/core/Config.h>
#include <xrpl/basics/random.h>

#include <random>

namespace ripple {

enum class RuntimeFaultDraw {
    peerDrop,
    peerJitter,
    rngClaimDrop,
    rngRevealDrop
};

// Separate specializations preserve the four independent per-thread engines
// outside stepping. In particular, the threaded harness must not draw from
// its bare injected engine on concurrent io/job threads. steppingMode is fixed
// before Application construction and runs both paths on one controller thread.
template <RuntimeFaultDraw Domain>
int
runtimeFaultDraw(Application& app, int upper)
{
    if (app.config().steppingMode)
        return rand_int(app.getPrng(), 0, upper);

    static thread_local std::mt19937 rng{std::random_device{}()};
    return rand_int(rng, 0, upper);
}

}  // namespace ripple

#endif
