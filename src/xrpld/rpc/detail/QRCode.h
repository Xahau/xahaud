//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_RPC_QRCODE_H_INCLUDED
#define RIPPLE_RPC_QRCODE_H_INCLUDED

#include <string>
#include <string_view>
#include <vector>

namespace ripple {
namespace qr {

/** The QR code (ISO/IEC 18004) for `text`, row-major, true for dark.

    Byte mode, with any tail of `text` that is alphanumeric (uppercase,
    digits, " $%*+-./:") in alphanumeric mode when that is shorter, so a
    lowercase URL ending in uppercase hex costs little more than the hex.
    The smallest version that holds it at level L, then the strongest level
    that still fits that version. `mask` forces a mask
    pattern (0 to 7) instead of the least penalised one. Empty if `text`
    does not fit in version 40.
*/
std::vector<std::vector<bool>>
encode(std::string_view text, int mask = -1);

/** `code` as UTF-8 lines of half blocks, two module rows per line, inside a
    quiet zone of `quiet` modules. The light modules are drawn, so the code
    scans on a dark background, or the dark ones if `invert`. */
std::vector<std::string>
render(
    std::vector<std::vector<bool>> const& code,
    bool invert = false,
    int quiet = 4);

}  // namespace qr
}  // namespace ripple

#endif
