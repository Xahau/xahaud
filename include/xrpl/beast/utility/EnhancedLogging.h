//------------------------------------------------------------------------------
/*
    This file is part of Beast: https://github.com/vinniefalco/Beast
    Copyright 2013, Vinnie Falco <vinnie.falco@gmail.com>

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

#ifndef BEAST_UTILITY_ENHANCEDLOGGING_H_INCLUDED
#define BEAST_UTILITY_ENHANCEDLOGGING_H_INCLUDED

#include <cstddef>  // for size_t
#include <iosfwd>   // for std::ostream

namespace beast {
namespace detail {

// Check if we should use colors - cached at startup
bool
should_log_use_colors();

// Get the log highlight color - can be overridden via
// LOG_HIGHLIGHT_COLOR
const char*
get_log_highlight_color();

// Strip source root path from __FILE__ at compile time
// IMPORTANT: This MUST stay in the header as constexpr for compile-time
// evaluation!
constexpr const char*
strip_source_root(const char* file)
{
    // Handle relative paths from build/ directory (common with ccache)
    // e.g., "../src/ripple/..." -> "ripple/..."
    if (file && file[0] == '.' && file[1] == '.' && file[2] == '/' &&
        file[3] == 's' && file[4] == 'r' && file[5] == 'c' && file[6] == '/')
    {
        return file + 7;  // skip "../src/"
    }

#ifdef SOURCE_ROOT_PATH
    constexpr const char* sourceRoot = SOURCE_ROOT_PATH;
    constexpr auto strlen_constexpr = [](const char* s) constexpr {
        const char* p = s;
        while (*p)
            ++p;
        return p - s;
    };
    constexpr auto strncmp_constexpr =
        [](const char* a, const char* b, size_t n) constexpr {
            for (size_t i = 0; i < n; ++i)
            {
                if (a[i] != b[i])
                    return a[i] - b[i];
                if (a[i] == '\0')
                    break;
            }
            return 0;
        };
    constexpr size_t sourceRootLen = strlen_constexpr(sourceRoot);
    return (strncmp_constexpr(file, sourceRoot, sourceRootLen) == 0)
        ? file + sourceRootLen
        : file;
#else
    return file;
#endif
}

// Check if location info should be shown - cached at startup
bool
should_show_location();

// Helper to write location string (no leading/trailing space)
void
log_write_location_string(std::ostream& os, const char* file, int line);

}  // namespace detail
}  // namespace beast

#endif
