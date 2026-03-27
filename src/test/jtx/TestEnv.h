#ifndef TEST_JTX_TESTENV_H_INCLUDED
#define TEST_JTX_TESTENV_H_INCLUDED

#include <test/jtx/Env.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/AccountID.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sstream>

namespace ripple {
namespace test {
namespace jtx {

/**
 * TestEnv wraps Env with:
 *   - Named account registry: env.account("alice")
 *   - Auto log transform: replaces r-addresses with Account(name) in log output
 *   - Env-var driven per-partition log levels via TESTENV_LOGGING
 *
 * Usage:
 *   TestEnv env{suite, features};
 *   auto const& alice = env.account("alice");
 *   auto const& bob = env.account("bob");
 *   env.fund(XRP(10000), alice, bob);
 *   // Logs now show Account(alice), Account(bob) instead of r-addresses
 *
 * Log levels via env var:
 *   TESTENV_LOGGING="HooksTrace=trace,View=debug"
 *
 * Valid levels: trace, debug, info, warning, error, fatal
 */
class TestEnv : public Env
{
    std::map<std::string, Account> accounts_;

public:
    TestEnv(
        beast::unit_test::suite& suite,
        FeatureBitset features)
        : Env(suite, features)
    {
        installTransform();
        applyLoggingEnvVar();
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
        applyLoggingEnvVar();
    }

    ~TestEnv()
    {
        app().logs().setTransform(nullptr);
    }

    /// Get or create a named account.
    /// First call creates the Account; subsequent calls return the same one.
    Account const&
    account(std::string const& name)
    {
        auto [it, inserted] = accounts_.try_emplace(name, name);
        return it->second;
    }

private:
    static beast::severities::Severity
    parseSeverity(std::string const& s)
    {
        if (s == "trace")   return beast::severities::kTrace;
        if (s == "debug")   return beast::severities::kDebug;
        if (s == "info")    return beast::severities::kInfo;
        if (s == "warning") return beast::severities::kWarning;
        if (s == "error")   return beast::severities::kError;
        if (s == "fatal")   return beast::severities::kFatal;
        return beast::severities::kError;
    }

    void
    applyLoggingEnvVar()
    {
        // Parse TESTENV_LOGGING="Partition1=level,Partition2=level"
        auto const* envVal = std::getenv("TESTENV_LOGGING");
        if (!envVal || !envVal[0])
            return;

        std::istringstream ss(envVal);
        std::string pair;
        while (std::getline(ss, pair, ','))
        {
            auto eq = pair.find('=');
            if (eq == std::string::npos)
                continue;
            auto partition = pair.substr(0, eq);
            auto level = pair.substr(eq + 1);
            app().logs().get(partition).threshold(parseSeverity(level));
        }
    }

    void
    installTransform()
    {
        app().logs().setTransform([this](std::string const& text) {
            std::string out = text;
            for (auto const& [name, acc] : accounts_)
            {
                auto raddr = toBase58(acc.id());
                std::string::size_type pos = 0;
                std::string replacement = "Account(" + name + ")";
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
