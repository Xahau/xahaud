#ifndef RIPPLE_OVERLAY_DETAIL_EXPORTSHAREJOB_H_INCLUDED
#define RIPPLE_OVERLAY_DETAIL_EXPORTSHAREJOB_H_INCLUDED

#include <xrpld/core/JobQueue.h>

#include <utility>

namespace ripple::detail {

// Keep the production posting boundary shared with its JobQueue regression.
inline constexpr JobType exportShareJobType = jtEXPORT_SHARES;

template <class Handler>
bool
postExportShareJob(JobQueue& queue, Handler&& handler)
{
    return queue.addJob(
        exportShareJobType, "recvExportShares", std::forward<Handler>(handler));
}

}  // namespace ripple::detail

#endif
