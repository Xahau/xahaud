//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/XRPLF/rippled
    Copyright 2026 Xahau

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
    IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpld/app/consensus/ActiveValidatorView.h>

namespace ripple {

namespace {

//@@start active-validator-view-nunl-cap-formula
std::size_t
maxNegativeUNLRemovals(std::size_t originalViewSize)
{
    if (originalViewSize == 0)
        return 0;

    return (originalViewSize - 1) / 4 + 1;
}
//@@end active-validator-view-nunl-cap-formula

}  // namespace

ActiveValidatorView
buildActiveValidatorView(
    ActiveValidatorViewSource const& source,
    ActiveValidatorViewFallback const& fallback)
{
    ActiveValidatorView view;
    view.sourceLedgerHash = source.sourceLedgerHash;

    if (source.unlReportMasterKeys && !source.unlReportMasterKeys->empty())
    {
        for (auto const& masterKey : *source.unlReportMasterKeys)
            view.insertMaster(masterKey);
        view.fromUNLReport = true;
    }
    else
    {
        for (auto const& masterKey : fallback.trustedMasterKeys)
            view.insertMaster(masterKey);
        if (fallback.localMasterKey)
            view.insertMaster(*fallback.localMasterKey);
    }

    // Capture the pre-nUNL denominator before subtracting negatives. size()
    // remains the effective (post-nUNL) count used by the 80% validator-quorum
    // gate; originalViewSize is the original-UNL count that the Tier 2
    // (participant_aligned) intersection floor anchors to, since nUNL can
    // shrink the effective view while leaving faulty nodes in it.
    //@@start active-validator-view-original-size
    view.originalViewSize = view.masterKeys.size();
    //@@end active-validator-view-original-size

    if (source.negativeUNLEnabled)
    {
        //@@start active-validator-view-nunl-candidates
        std::vector<PublicKey> disabledActiveMasters;
        disabledActiveMasters.reserve(
            std::min(source.negativeUNL.size(), view.masterKeys.size()));
        for (auto const& masterKey : source.negativeUNL)
        {
            if (view.containsMaster(masterKey))
                disabledActiveMasters.push_back(masterKey);
        }

        std::sort(disabledActiveMasters.begin(), disabledActiveMasters.end());
        //@@end active-validator-view-nunl-candidates

        //@@start active-validator-view-nunl-cap-apply
        auto const removals = std::min(
            disabledActiveMasters.size(),
            maxNegativeUNLRemovals(view.originalViewSize));
        for (auto it = disabledActiveMasters.begin();
             it != disabledActiveMasters.begin() + removals;
             ++it)
        {
            auto const& masterKey = *it;
            view.eraseMaster(masterKey);
        }
        //@@end active-validator-view-nunl-cap-apply
    }

    view.canonicalizeOrder();

    return view;
}

}  // namespace ripple
