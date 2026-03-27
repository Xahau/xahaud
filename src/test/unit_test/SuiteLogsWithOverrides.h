//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

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

#ifndef TEST_UNIT_TEST_SUITE_LOGS_WITH_OVERRIDES_H
#define TEST_UNIT_TEST_SUITE_LOGS_WITH_OVERRIDES_H

#include <test/unit_test/SuiteJournal.h>
#include <xrpl/basics/Log.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/beast/utility/Journal.h>
#include <iostream>
#include <mutex>
#include <set>
#include <string>

namespace ripple {
namespace test {

/** A Journal::Sink that writes directly to stderr.
 *
 *  Unlike SuiteJournalSink (which writes to suite_.log and is only
 *  visible when tests fail), this always produces visible output.
 */
class StderrJournalSink : public beast::Journal::Sink
{
    std::string partition_;

public:
    StderrJournalSink(
        std::string const& partition,
        beast::severities::Severity threshold)
        : Sink(threshold, false), partition_(partition)
    {
    }

    bool
    active(beast::severities::Severity level) const override
    {
        return level >= threshold();
    }

    void
    write(beast::severities::Severity level, std::string const& text) override
    {
        if (level >= threshold())
            writeAlways(level, text);
    }

    void
    writeAlways(beast::severities::Severity level, std::string const& text)
        override
    {
        static std::mutex mtx;
        std::lock_guard lock(mtx);
        std::cerr << partition_ << ":" << text << std::endl;
        std::cout << "stdout: " << partition_ << ":" << text << std::endl;
    }
};

/** SuiteLogs with per-partition severity overrides written to stderr.
 *
 *  Overridden partitions write to stderr (always visible).
 *  All other partitions use SuiteJournalSink (suite_.log, only on failure).
 *
 *  Usage:
 *      #include <test/unit_test/SuiteLogsWithOverrides.h>
 *
 *      using Sev = beast::severities::Severity;
 *      Env env{*this, cfg, features,
 *          std::make_unique<SuiteLogsWithOverrides>(
 *              *this,
 *              SuiteLogsWithOverrides::Overrides{
 *                  {"Export",  Sev::kTrace},
 *                  {"TxQ",    Sev::kInfo},
 *                  {"View",   Sev::kDebug},
 *              })};
 */
class SuiteLogsWithOverrides : public Logs
{
    beast::unit_test::suite& suite_;
    std::set<std::string> overridden_;

public:
    using Overrides = std::initializer_list<
        std::pair<std::string, beast::severities::Severity>>;

    SuiteLogsWithOverrides(
        beast::unit_test::suite& suite,
        Overrides overrides,
        beast::severities::Severity defaultThresh = beast::severities::kError)
        : Logs(defaultThresh), suite_(suite)
    {
        for (auto const& [name, sev] : overrides)
        {
            overridden_.insert(name);
            get(name).threshold(sev);
        }
    }

    ~SuiteLogsWithOverrides() override = default;

    std::unique_ptr<beast::Journal::Sink>
    makeSink(
        std::string const& partition,
        beast::severities::Severity threshold) override
    {
        if (overridden_.count(partition))
            return std::make_unique<StderrJournalSink>(partition, threshold);
        return std::make_unique<SuiteJournalSink>(partition, threshold, suite_);
    }
};

}  // namespace test
}  // namespace ripple

#endif
