//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 Ripple Labs Inc.

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

#include <xrpld/app/main/AmendmentBlocked.h>
#include <xrpld/core/Config.h>
#include <xrpl/basics/FileUtilities.h>

namespace ripple {

boost::filesystem::path
amendmentBlockedFilePath(Config const& config)
{
    return config.CONFIG_DIR / amendmentBlockedFileName;
}

boost::system::error_code
writeAmendmentBlockedFile(Config const& config)
{
    static constexpr char contents[] =
        "XAHAUD STOPPED: UPGRADE REQUIRED\n"
        "\n"
        "This version of xahaud does not support a network amendment.\n"
        "The amendment will activate soon or is already active.\n"
        "Do not start this version of xahaud again.\n"
        "\n"
        "To restart the server:\n"
        "1. Upgrade xahaud to a version that supports the amendment.\n"
        "2. Delete this file.\n"
        "3. Start xahaud again.\n"
        "\n";

    boost::system::error_code ec;
    writeFileContents(ec, amendmentBlockedFilePath(config), contents);
    return ec;
}

}  // namespace ripple
