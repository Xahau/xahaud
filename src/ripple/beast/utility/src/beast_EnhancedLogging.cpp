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

#include <ripple/beast/utility/EnhancedLogging.h>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <unistd.h>

namespace beast {
namespace detail {

// Check if we should use colors - cached at startup
bool
should_log_use_colors()
{
    static const bool use_colors = []() {
        // Honor NO_COLOR environment variable (standard)
        if (std::getenv("NO_COLOR"))
            return false;

        // Honor FORCE_COLOR to override terminal detection
        if (std::getenv("FORCE_COLOR"))
            return true;

        // Check if stderr is a terminal
        return isatty(STDERR_FILENO) != 0;
    }();
    return use_colors;
}

// Get the log location escape sequence - can be overridden via
// LOG_LOCATION_ESCAPE
const char*
get_log_highlight_escape()
{
    static const char* escape = []() {
        const char* env = std::getenv("LOG_HIGHLIGHT_ESCAPE");
        if (!env)
            return "\033[36m";  // Default: cyan

        // Simple map of color names to escape sequences
        if (std::strcmp(env, "red") == 0)
            return "\033[31m";
        if (std::strcmp(env, "green") == 0)
            return "\033[32m";
        if (std::strcmp(env, "yellow") == 0)
            return "\033[33m";
        if (std::strcmp(env, "blue") == 0)
            return "\033[34m";
        if (std::strcmp(env, "magenta") == 0)
            return "\033[35m";
        if (std::strcmp(env, "cyan") == 0)
            return "\033[36m";
        if (std::strcmp(env, "white") == 0)
            return "\033[37m";
        if (std::strcmp(env, "gray") == 0 || std::strcmp(env, "grey") == 0)
            return "\033[90m";  // Bright black (gray)
        if (std::strcmp(env, "orange") == 0)
            return "\033[93m";  // Bright yellow (appears orange-ish)
        if (std::strcmp(env, "none") == 0)
            return "";

        // Default to cyan if unknown color name
        return "\033[36m";
    }();
    return escape;
}

// Get configured position - cached at startup
LocationPosition
get_log_location_position()
{
    static const LocationPosition position = []() {
        const char* env = std::getenv("LOG_LOCATION_POSITION");
        if (!env)
            return LocationPosition::SUFFIX;  // Default to suffix for better
        // readability

        if (std::strcmp(env, "suffix") == 0 || std::strcmp(env, "end") == 0)
            return LocationPosition::SUFFIX;
        if (std::strcmp(env, "prefix") == 0 || std::strcmp(env, "start") == 0)
            return LocationPosition::PREFIX;
        if (std::strcmp(env, "none") == 0)
            return LocationPosition::NONE;

        return LocationPosition::PREFIX;
    }();
    return position;
}

// Helper to write location string (no leading/trailing space)
void
log_write_location_string(std::ostream& os, const char* file, int line)
{
    if (detail::should_log_use_colors())
    {
        os << detail::get_log_highlight_escape() << "["
           << detail::strip_source_root(file) << ":" << line << "]\033[0m";
    }
    else
    {
        os << "[" << detail::strip_source_root(file) << ":" << line << "]";
    }
}

}  // namespace detail
}  // namespace beast
