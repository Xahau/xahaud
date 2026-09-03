#ifndef TEST_JTX_TESTENV_H_INCLUDED
#define TEST_JTX_TESTENV_H_INCLUDED

#include <test/jtx/Env.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/AccountID.h>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace ripple {
namespace test {
namespace jtx {

/** Env with named accounts, log rewriting, and per-partition levels.

    Named accounts: env.account("alice") is stable for the life of the Env.

    Log lines replace r-addresses with Account(name). setPrefix() tags a
    phase so traces from one step are easy to find.

    Partition levels, comma-separated:

        XAHAU_TEST_LOG=HooksTrace=trace,View=debug

    TESTENV_LOGGING is accepted as an alias. Levels: trace, debug, info,
    warning, error, fatal.
*/
class TestEnv : public Env
{
    // Logs owns the rewriter until it dies after the app thread joins.
    // Capturing this table (not TestEnv) keeps late log lines from UAF.
    struct Rewrite
    {
        std::string prefix;
        std::map<std::string, Account> accounts;
    };
    std::shared_ptr<Rewrite> rewrite_ = std::make_shared<Rewrite>();

public:
    TestEnv(beast::unit_test::suite& suite, FeatureBitset features)
        : Env(suite, features)
    {
        installTransform();
        applyLoggingEnv();
    }

    TestEnv(
        beast::unit_test::suite& suite,
        std::unique_ptr<Config> config,
        FeatureBitset features,
        std::unique_ptr<Logs> logs = nullptr,
        beast::severities::Severity thresh = beast::severities::kError)
        : Env(suite, std::move(config), features, std::move(logs), thresh)
    {
        installTransform();
        applyLoggingEnv();
    }

    Account const&
    account(std::string const& name)
    {
        return rewrite_->accounts.try_emplace(name, name).first->second;
    }

    void
    setPrefix(std::string const& prefix)
    {
        rewrite_->prefix = prefix.empty() ? "" : "[" + prefix + "] ";
    }

private:
    static beast::severities::Severity
    parseSeverity(std::string const& s)
    {
        if (s == "trace")
            return beast::severities::kTrace;
        if (s == "debug")
            return beast::severities::kDebug;
        if (s == "info")
            return beast::severities::kInfo;
        if (s == "warning")
            return beast::severities::kWarning;
        if (s == "error")
            return beast::severities::kError;
        if (s == "fatal")
            return beast::severities::kFatal;
        return beast::severities::kError;
    }

    void
    applyLoggingEnv()
    {
        auto const* envVal = std::getenv("XAHAU_TEST_LOG");
        if (!envVal || !envVal[0])
            envVal = std::getenv("TESTENV_LOGGING");
        if (!envVal || !envVal[0])
            return;

        std::istringstream ss(envVal);
        std::string pair;
        while (std::getline(ss, pair, ','))
        {
            auto const eq = pair.find('=');
            if (eq == std::string::npos)
                continue;
            app()
                .logs()
                .get(pair.substr(0, eq))
                .threshold(parseSeverity(pair.substr(eq + 1)));
        }
    }

    void
    installTransform()
    {
        app().logs().setTransform(
            [rewrite = rewrite_](std::string const& text) {
                std::string out = rewrite->prefix + text;
                for (auto const& [name, acc] : rewrite->accounts)
                {
                    auto const raddr = toBase58(acc.id());
                    std::string::size_type pos = 0;
                    std::string const replacement = "Account(" + name + ")";
                    while ((pos = out.find(raddr, pos)) != std::string::npos)
                    {
                        out.replace(pos, raddr.size(), replacement);
                        pos += replacement.size();
                    }
                }
                return out;
            });
    }
};

}  // namespace jtx
}  // namespace test
}  // namespace ripple

#endif
