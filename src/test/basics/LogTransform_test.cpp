//------------------------------------------------------------------------------
/*
    Logs::setTransform is a test hook. Empty path is a null check.
*/
//==============================================================================

#include <xrpl/basics/Log.h>
#include <xrpl/beast/unit_test.h>

#include <string>

namespace ripple {
namespace test {

class LogTransform_test : public beast::unit_test::suite
{
    void
    testIdentityAndRewrite()
    {
        Logs logs{beast::severities::kError};
        BEAST_EXPECT(!logs.hasTransform());
        BEAST_EXPECT(logs.applyTransform("alice") == "alice");
        logs.setTransform(
            [](std::string const& text) { return "Account(" + text + ")"; });
        BEAST_EXPECT(logs.hasTransform());
        BEAST_EXPECT(logs.applyTransform("alice") == "Account(alice)");
        logs.setTransform(nullptr);
        BEAST_EXPECT(!logs.hasTransform());
        BEAST_EXPECT(logs.applyTransform("alice") == "alice");
    }

    void
    run() override
    {
        testcase("identity, rewrite, and clear");
        testIdentityAndRewrite();
    }
};

BEAST_DEFINE_TESTSUITE(LogTransform, ripple_basics, ripple);

}  // namespace test
}  // namespace ripple
