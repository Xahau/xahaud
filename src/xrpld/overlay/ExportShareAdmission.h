//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES WHATSOEVER
    RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
    CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
    CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_OVERLAY_EXPORTSHAREADMISSION_H_INCLUDED
#define RIPPLE_OVERLAY_EXPORTSHAREADMISSION_H_INCLUDED

#include <cstdint>
#include <functional>

namespace ripple {

enum class ExportShareDisposition : std::uint8_t {
    accepted,
    deferred,
    duplicate,
    invalid
};

enum class ExportShareCharge : std::uint8_t {
    none,
    invalidData,
    invalidSignature
};

struct ExportShareAdmission
{
    ExportShareDisposition disposition{ExportShareDisposition::invalid};
    ExportShareCharge charge{ExportShareCharge::none};

    bool
    isAccepted() const
    {
        return disposition == ExportShareDisposition::accepted;
    }
};

using ExportShareChargeHandler = std::function<void(ExportShareCharge)>;

}  // namespace ripple

#endif
