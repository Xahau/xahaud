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

#include <ripple/beast/utility/Journal.h>
#include <cassert>
#ifdef LOG_LINE_NUMBERS
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#endif

namespace beast {

//------------------------------------------------------------------------------

// A Sink that does nothing.
class NullJournalSink : public Journal::Sink
{
public:
    NullJournalSink() : Sink(severities::kDisabled, false)
    {
    }

    ~NullJournalSink() override = default;

    bool active(severities::Severity) const override
    {
        return false;
    }

    bool
    console() const override
    {
        return false;
    }

    void
    console(bool) override
    {
    }

    severities::Severity
    threshold() const override
    {
        return severities::kDisabled;
    }

    void threshold(severities::Severity) override
    {
    }

    void
    write(severities::Severity, std::string const&) override
    {
    }
};

//------------------------------------------------------------------------------

Journal::Sink&
Journal::getNullSink()
{
    static NullJournalSink sink;
    return sink;
}

//------------------------------------------------------------------------------

Journal::Sink::Sink(Severity thresh, bool console)
    : thresh_(thresh), m_console(console)
{
}

Journal::Sink::~Sink() = default;

bool
Journal::Sink::active(Severity level) const
{
    return level >= thresh_;
}

bool
Journal::Sink::console() const
{
    return m_console;
}

void
Journal::Sink::console(bool output)
{
    m_console = output;
}

severities::Severity
Journal::Sink::threshold() const
{
    return thresh_;
}

void
Journal::Sink::threshold(Severity thresh)
{
    thresh_ = thresh;
}

//------------------------------------------------------------------------------

Journal::ScopedStream::ScopedStream(Sink& sink, Severity level)
    : m_sink(sink), m_level(level)
{
    // Modifiers applied from all ctors
    m_ostream << std::boolalpha << std::showbase;
}

Journal::ScopedStream::ScopedStream(
    Stream const& stream,
    std::ostream& manip(std::ostream&))
    : ScopedStream(stream.sink(), stream.level())
{
    m_ostream << manip;
}

#ifdef LOG_LINE_NUMBERS
//------------------------------------------------------------------------------

namespace detail {

// Location position enum
enum class LocationPosition { PREFIX, SUFFIX, NONE };

// Get configured position - cached at startup
LocationPosition
getLocationPosition()
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
writeLocationString(std::ostream& os, const char* file, int line)
{
    if (detail::shouldUseColors())
    {
        os << detail::getLocationEscape() << "["
           << detail::stripSourceRoot(file) << ":" << line << "]\033[0m";
    }
    else
    {
        os << "[" << detail::stripSourceRoot(file) << ":" << line << "]";
    }
}

// Check if we should use colors - cached at startup
bool
shouldUseColors()
{
    static const bool useColors = []() {
        // Honor NO_COLOR environment variable (standard)
        if (std::getenv("NO_COLOR"))
            return false;

        // Honor FORCE_COLOR to override terminal detection
        if (std::getenv("FORCE_COLOR"))
            return true;

        // Check if stderr is a terminal
        return isatty(STDERR_FILENO) != 0;
    }();
    return useColors;
}

// Get the location escape sequence - can be overridden via LOG_LOCATION_ESCAPE
const char*
getLocationEscape()
{
    static const char* escape = []() {
        const char* env = std::getenv("LOG_LOCATION_ESCAPE");
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

}  // namespace detail

#endif

#ifdef LOG_LINE_NUMBERS
Journal::ScopedStream::ScopedStream(
    Sink& sink,
    Severity level,
    const char* file,
    int line)
    : m_sink(sink), m_level(level), file_(file), line_(line)
{
    // Modifiers applied from all ctors
    m_ostream << std::boolalpha << std::showbase;

    // Write prefix if configured
    if (file_ &&
        detail::getLocationPosition() == detail::LocationPosition::PREFIX)
    {
        detail::writeLocationString(m_ostream, file_, line_);
        m_ostream << " ";
    }
}
#endif

Journal::ScopedStream::~ScopedStream()
{
    std::string s(m_ostream.str());

#ifdef LOG_LINE_NUMBERS
    // Add suffix if configured
    if (file_ &&
        detail::getLocationPosition() == detail::LocationPosition::SUFFIX &&
        !s.empty() && s != "\n")
    {
        std::ostringstream combined;
        combined << s;
        if (!s.empty() && s.back() != ' ')
            combined << " ";
        detail::writeLocationString(combined, file_, line_);
        s = combined.str();
    }
#endif

    if (!s.empty())
    {
        if (s == "\n")
            m_sink.write(m_level, "");
        else
            m_sink.write(m_level, s);
    }
}

std::ostream&
Journal::ScopedStream::operator<<(std::ostream& manip(std::ostream&)) const
{
    return m_ostream << manip;
}

//------------------------------------------------------------------------------

Journal::ScopedStream
Journal::Stream::operator<<(std::ostream& manip(std::ostream&)) const
{
    return ScopedStream(*this, manip);
}

#ifdef LOG_LINE_NUMBERS

// Implementation moved to use new constructor
Journal::ScopedStream
Journal::StreamWithLocation::operator<<(
    std::ostream& manip(std::ostream&)) const
{
    // Create a ScopedStream with location info
    ScopedStream scoped(stream_.sink(), stream_.level(), file_, line_);
    scoped.ostream() << manip;
    return scoped;
}
#endif

}  // namespace beast
