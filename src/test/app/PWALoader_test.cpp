//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 XRPL Labs

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

#include <test/jtx.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/protocol/AppLoader.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/jss.h>

#include <string>

namespace ripple {
namespace test {

struct PWALoader_test : public beast::unit_test::suite
{
    // A minimal, realistic loader: enough markup to mount the app, no more.
    static std::string
    goodDoc()
    {
        return "<!DOCTYPE html>\n"
               "<html lang=\"en\">\n"
               "<head><meta charset=\"utf-8\"><title>xapp</title></head>\n"
               "<body><div id=\"root\"></div>"
               "<script src=\"/app.js\"></script></body>\n"
               "</html>\n";
    }

    static appLoader::Result
    check(std::string const& s)
    {
        return appLoader::validate(
            reinterpret_cast<std::uint8_t const*>(s.data()), s.size());
    }

    // Build a valid document padded out to exactly `len` bytes.
    static std::string
    docOfSize(std::size_t len)
    {
        std::string const head = "<!DOCTYPE html><html><body>";
        std::string const tail = "</body></html>";
        if (len < head.size() + tail.size())
            return head + tail;
        return head + std::string(len - head.size() - tail.size(), 'x') + tail;
    }

    //--------------------------------------------------------------------------
    // Validator unit tests. These drive appLoader::validate directly, with no
    // ledger involved.
    //--------------------------------------------------------------------------

    void
    testValidator()
    {
        testcase("validator");

        using R = appLoader::Result;

        // --- accepted ---------------------------------------------------
        BEAST_EXPECT(check(goodDoc()) == R::ok);
        BEAST_EXPECT(check("<!DOCTYPE html><html></html>") == R::ok);
        BEAST_EXPECT(check("<!doctype html><html></html>") == R::ok);
        BEAST_EXPECT(check("<!DoCtYpE html><html></html>") == R::ok);
        // No doctype, but opens with <html> directly.
        BEAST_EXPECT(check("<html></html>") == R::ok);
        BEAST_EXPECT(check("<html lang=\"en\"></html>") == R::ok);
        // Leading and trailing whitespace is tolerated.
        BEAST_EXPECT(check("  \n\t<html></html>\n\n  ") == R::ok);
        // Whitespace inside the closing tag is legal HTML.
        BEAST_EXPECT(check("<html></html >") == R::ok);
        // A UTF-8 BOM is skipped.
        BEAST_EXPECT(check("\xEF\xBB\xBF<html></html>") == R::ok);
        // Multi-byte UTF-8 in the body.
        BEAST_EXPECT(
            check("<html><body>\xE6\xBC\xA2\xE5\xAD\x97"
                  "</body></html>") == R::ok);
        // A literal </html> inside a script must not shadow the real one.
        BEAST_EXPECT(
            check("<html><script>var s=\"</html>\";</script></html>") == R::ok);
        // TAB / LF / CR / FF are HTML whitespace and are fine.
        BEAST_EXPECT(check("<html>\t\n\r\f</html>") == R::ok);

        // --- rejected ---------------------------------------------------
        BEAST_EXPECT(check("") == R::empty);
        BEAST_EXPECT(check("   \n\t ") == R::empty);

        // Plain text, JSON, and other non-documents.
        BEAST_EXPECT(check("hello world") == R::noDoctype);
        BEAST_EXPECT(check("{\"a\":1}") == R::noDoctype);
        BEAST_EXPECT(check("<?xml version=\"1.0\"?>") == R::noDoctype);
        BEAST_EXPECT(check("<body></body>") == R::noDoctype);
        // A fragment is not a document.
        BEAST_EXPECT(check("<div>hi</div>") == R::noDoctype);
        // Doctype must be followed by whitespace.
        BEAST_EXPECT(check("<!doctypehtml><html></html>") == R::noDoctype);
        // Tag name must actually end.
        BEAST_EXPECT(check("<htmlish></htmlish>") == R::noDoctype);

        // Structure problems.
        BEAST_EXPECT(check("<!DOCTYPE html>") == R::noHtmlElement);
        BEAST_EXPECT(check("<!DOCTYPE html><p>orphan</p>") == R::noHtmlElement);
        BEAST_EXPECT(check("<html>") == R::unclosed);
        BEAST_EXPECT(check("<html><body>no end tag</body>") == R::unclosed);
        // Missing '>' on the end tag.
        BEAST_EXPECT(check("<html></html") == R::unclosed);
        BEAST_EXPECT(check("<html></html>trailing") == R::trailingGarbage);
        BEAST_EXPECT(
            check("<html></html><script>x</script>") == R::trailingGarbage);

        // Control characters.
        BEAST_EXPECT(
            check("<html>\x01"
                  "</html>") == R::badControlChar);
        // Embedded NUL.
        BEAST_EXPECT(
            check(std::string("<html>\0</html>", 14)) == R::badControlChar);
        BEAST_EXPECT(
            check("<html>\x7F"
                  "</html>") == R::badControlChar);

        // Size, at the 4 KiB boundary.
        BEAST_EXPECT(maxAppLoaderLength == 4096);
        BEAST_EXPECT(check(docOfSize(maxAppLoaderLength - 1)) == R::ok);
        BEAST_EXPECT(check(docOfSize(maxAppLoaderLength)) == R::ok);
        BEAST_EXPECT(check(docOfSize(maxAppLoaderLength + 1)) == R::tooLarge);
    }

    void
    testUTF8()
    {
        testcase("utf8");

        using R = appLoader::Result;

        auto const wrap = [](std::string const& mid) {
            return "<html>" + mid + "</html>";
        };

        // Well-formed sequences of each length.
        BEAST_EXPECT(check(wrap("\x24")) == R::ok);              // U+0024
        BEAST_EXPECT(check(wrap("\xC2\xA2")) == R::ok);          // U+00A2
        BEAST_EXPECT(check(wrap("\xE0\xA4\xB9")) == R::ok);      // U+0939
        BEAST_EXPECT(check(wrap("\xF0\x90\x8D\x88")) == R::ok);  // U+10348
        BEAST_EXPECT(check(wrap("\xF4\x8F\xBF\xBF")) == R::ok);  // U+10FFFF
        // U+FFFE / U+FFFF are noncharacters but are valid UTF-8, and are
        // permitted in interchange.
        BEAST_EXPECT(check(wrap("\xEF\xBF\xBE")) == R::ok);
        BEAST_EXPECT(check(wrap("\xEF\xBF\xBF")) == R::ok);

        // Overlong encodings.
        BEAST_EXPECT(check(wrap("\xC0\xAF")) == R::badUTF8);
        BEAST_EXPECT(check(wrap("\xC1\xBF")) == R::badUTF8);
        BEAST_EXPECT(check(wrap("\xE0\x80\xAF")) == R::badUTF8);
        BEAST_EXPECT(check(wrap("\xF0\x80\x80\xAF")) == R::badUTF8);

        // UTF-16 surrogate halves.
        BEAST_EXPECT(check(wrap("\xED\xA0\x80")) == R::badUTF8);  // D800
        BEAST_EXPECT(check(wrap("\xED\xBF\xBF")) == R::badUTF8);  // DFFF

        // Beyond U+10FFFF.
        BEAST_EXPECT(check(wrap("\xF4\x90\x80\x80")) == R::badUTF8);
        BEAST_EXPECT(check(wrap("\xF5\x80\x80\x80")) == R::badUTF8);
        BEAST_EXPECT(check(wrap("\xF8\x88\x80\x80\x80")) == R::badUTF8);

        // Stray continuation bytes.
        BEAST_EXPECT(check(wrap("\x80")) == R::badUTF8);
        BEAST_EXPECT(check(wrap("\xBF")) == R::badUTF8);

        // Truncated sequences at the very end of the buffer.
        BEAST_EXPECT(check("<html></html>\xC2") == R::badUTF8);
        BEAST_EXPECT(check("<html></html>\xE0\xA4") == R::badUTF8);
        BEAST_EXPECT(check("<html></html>\xF0\x90\x8D") == R::badUTF8);
        BEAST_EXPECT(check(std::string("\xC2", 1)) == R::badUTF8);
    }

    //--------------------------------------------------------------------------
    // Ledger-level tests.
    //--------------------------------------------------------------------------

    void
    testEnabled(FeatureBitset features)
    {
        testcase("enabled");
        using namespace jtx;

        for (bool const withLoader : {false, true})
        {
            auto const amend =
                withLoader ? features : features - featurePWALoader;
            Env env{*this, amend};

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            auto jt = noop(alice);
            jt[sfAppLoader.fieldName] = strHex(goodDoc());

            if (withLoader)
            {
                env(jt, fee(XRP(1)));
                env.close();
                BEAST_EXPECT(env.le(keylet::appLoader(alice.id())));
            }
            else
            {
                env(jt, ter(temDISABLED));
                env.close();
                BEAST_EXPECT(!env.le(keylet::appLoader(alice.id())));
            }
        }

        // Removal is gated too: an empty blob is still a present field.
        {
            Env env{*this, features - featurePWALoader};
            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            auto jt = noop(alice);
            jt[sfAppLoader.fieldName] = "";
            env(jt, ter(temDISABLED));
        }
    }

    void
    testSetAndRemove(FeatureBitset features)
    {
        testcase("set and remove");
        using namespace jtx;

        Env env{*this, features};
        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        std::string const doc = goodDoc();
        auto const klLoader = keylet::appLoader(alice.id());

        // Nothing to begin with.
        BEAST_EXPECT(!env.le(klLoader));
        if (auto const root = env.le(alice); BEAST_EXPECT(root))
            BEAST_EXPECT(!root->isFieldPresent(sfAppLoaderID));

        // --- create ---
        auto jt = noop(alice);
        jt[sfAppLoader.fieldName] = strHex(doc);
        env(jt, fee(XRP(1)));
        env.close();

        {
            auto const sle = env.le(klLoader);
            if (BEAST_EXPECT(sle))
            {
                BEAST_EXPECT((*sle)[sfAppLoader] == makeSlice(doc));
                BEAST_EXPECT((*sle)[sfOwner] == alice.id());
            }
            // The AccountRoot points at it...
            auto const root = env.le(alice);
            if (BEAST_EXPECT(root))
            {
                BEAST_EXPECT((*root)[sfAppLoaderID] == klLoader.key);
                // ...and the blob is NOT inline on the AccountRoot.
                BEAST_EXPECT(!root->isFieldPresent(sfAppLoader));
            }
        }

        // No reserve is taken, and because the object is not in the owner
        // directory it is invisible to account_objects. Reaching it requires
        // sfAppLoaderID (or recomputing the keylet); this asserts the
        // trade-off deliberately rather than by omission.
        {
            auto const jrr = env.rpc(
                "json",
                "account_objects",
                R"({"account":")" + alice.human() + R"("})");
            BEAST_EXPECT(jrr[jss::result][jss::account_objects].size() == 0);

            // ledger_entry by index still resolves it.
            auto const le = env.rpc(
                "json",
                "ledger_entry",
                R"({"index":")" + to_string(klLoader.key) + R"("})");
            BEAST_EXPECT(
                le[jss::result][jss::node][sfLedgerEntryType.fieldName] ==
                jss::AppLoader);
        }

        // --- overwrite in place ---
        std::string const doc2 = "<html><body>v2</body></html>";
        jt[sfAppLoader.fieldName] = strHex(doc2);
        env(jt, fee(XRP(1)));
        env.close();
        {
            auto const sle = env.le(klLoader);
            if (BEAST_EXPECT(sle))
                BEAST_EXPECT((*sle)[sfAppLoader] == makeSlice(doc2));
            // The pointer is unchanged by an overwrite.
            if (auto const root = env.le(alice); BEAST_EXPECT(root))
                BEAST_EXPECT((*root)[sfAppLoaderID] == klLoader.key);
        }

        // --- remove with an empty blob ---
        jt[sfAppLoader.fieldName] = "";
        env(jt, fee(XRP(1)));
        env.close();
        BEAST_EXPECT(!env.le(klLoader));
        if (auto const root = env.le(alice); BEAST_EXPECT(root))
            BEAST_EXPECT(!root->isFieldPresent(sfAppLoaderID));

        // Removing when already absent is a no-op, not an error.
        env(jt, fee(XRP(1)));
        env.close();
        BEAST_EXPECT(!env.le(klLoader));

        // An AccountSet that does not mention AppLoader leaves it alone.
        jt[sfAppLoader.fieldName] = strHex(doc);
        env(jt, fee(XRP(1)));
        env.close();
        env(noop(alice));
        env.close();
        if (auto const sle = env.le(klLoader); BEAST_EXPECT(sle))
            BEAST_EXPECT((*sle)[sfAppLoader] == makeSlice(doc));
    }

    void
    testFee(FeatureBitset features)
    {
        testcase("fee");
        using namespace jtx;

        Env env{*this, features};
        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        auto const base = env.current()->fees().base;
        std::string const doc = docOfSize(1024);

        // The required fee is base + one drop per byte.
        auto const expected = base + XRPAmount{1024};

        auto jt = noop(alice);
        jt[sfAppLoader.fieldName] = strHex(doc);

        // One drop short is rejected.
        env(jt, fee(expected - XRPAmount{1}), ter(telINSUF_FEE_P));
        env.close();
        BEAST_EXPECT(!env.le(keylet::appLoader(alice.id())));

        // Exactly the required fee succeeds, and that much is burned.
        auto const before = env.balance(alice);
        env(jt, fee(expected));
        env.close();
        BEAST_EXPECT(env.le(keylet::appLoader(alice.id())));
        BEAST_EXPECT(before - env.balance(alice) == drops(expected));

        // Removal carries an empty blob, so it costs only the base fee.
        auto jtDel = noop(alice);
        jtDel[sfAppLoader.fieldName] = "";
        env(jtDel, fee(base));
        env.close();
        BEAST_EXPECT(!env.le(keylet::appLoader(alice.id())));
    }

    void
    testMalformed(FeatureBitset features)
    {
        testcase("malformed");
        using namespace jtx;

        Env env{*this, features};
        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        auto reject = [&](std::string const& doc) {
            auto jt = noop(alice);
            jt[sfAppLoader.fieldName] = strHex(doc);
            env(jt, fee(XRP(1)), ter(temMALFORMED));
            env.close();
            BEAST_EXPECT(!env.le(keylet::appLoader(alice.id())));
        };

        reject("hello world");
        reject("<div>fragment</div>");
        reject("<!DOCTYPE html>");
        reject("<html>");
        reject("<html></html>trailing");
        reject(std::string("<html>\0</html>", 14));
        reject("<html>\xC0\xAF</html>");
        reject(docOfSize(maxAppLoaderLength + 1));

        // The largest legal document is accepted.
        auto jt = noop(alice);
        jt[sfAppLoader.fieldName] = strHex(docOfSize(maxAppLoaderLength));
        env(jt, fee(XRP(1)));
        env.close();
        BEAST_EXPECT(env.le(keylet::appLoader(alice.id())));
    }

    void
    testAccountDelete(FeatureBitset features)
    {
        testcase("account delete");
        using namespace jtx;

        Env env{*this, features};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        auto jt = noop(alice);
        jt[sfAppLoader.fieldName] = strHex(goodDoc());
        env(jt, fee(XRP(1)));
        env.close();
        BEAST_EXPECT(env.le(keylet::appLoader(alice.id())));

        // AccountDelete requires the account to be well seasoned.
        for (int i = 0; i < 256; ++i)
            env.close();

        // The AppLoader is not in the owner directory, so it must not block
        // deletion -- and DeleteAccount must still erase it, or
        // AccountRootsDeletedClean fires on the orphan.
        env(acctdelete(alice, bob),
            fee(drops(env.current()->fees().increment)));
        env.close();

        BEAST_EXPECT(!env.le(alice));
        BEAST_EXPECT(!env.le(keylet::appLoader(alice.id())));
    }

    void
    testURITokenUTF8Gate(FeatureBitset features)
    {
        testcase("uritoken utf8 gate");
        using namespace jtx;

        // U+FFFF is well-formed UTF-8, but the pre-amendment URIToken check
        // rejected it. Under featurePWALoader it is accepted. This is the
        // observable behaviour change the amendment gate exists to cover.
        std::string const uri = "ipfs://x\xEF\xBF\xBF";

        for (bool const withLoader : {false, true})
        {
            auto const amend =
                withLoader ? features : features - featurePWALoader;
            Env env{*this, amend};

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            env(uritoken::mint(alice, uri),
                fee(XRP(1)),
                ter(withLoader ? TER{tesSUCCESS} : TER{temMALFORMED}));
            env.close();
        }

        // Genuinely malformed UTF-8 is rejected either side of the amendment.
        for (bool const withLoader : {false, true})
        {
            auto const amend =
                withLoader ? features : features - featurePWALoader;
            Env env{*this, amend};

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            env(uritoken::mint(alice, std::string("bad\xC0\xAF")),
                fee(XRP(1)),
                ter(temMALFORMED));
            env.close();
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnabled(features);
        testSetAndRemove(features);
        testFee(features);
        testMalformed(features);
        testAccountDelete(features);
        testURITokenUTF8Gate(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        testValidator();
        testUTF8();
        testWithFeats(supported_amendments());
    }
};

BEAST_DEFINE_TESTSUITE(PWALoader, app, ripple);

}  // namespace test
}  // namespace ripple
