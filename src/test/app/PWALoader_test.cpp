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

#include <iterator>
#include <random>
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
        // All-uppercase tag names are legal HTML.
        BEAST_EXPECT(check("<HTML></HTML>") == R::ok);
        BEAST_EXPECT(check("<!DOCTYPE HTML><HTML></HTML>") == R::ok);
        // Mixed case.
        BEAST_EXPECT(check("<Html></Html>") == R::ok);
        // Attributes on <html>.
        BEAST_EXPECT(
            check("<html lang=\"en\" class=\"root\"></html>") == R::ok);
        // Nested elements.
        BEAST_EXPECT(
            check("<!DOCTYPE html><html><body><p>hi</p></body></html>") ==
            R::ok);
        // Trailing whitespace after </html>.
        BEAST_EXPECT(check("<html></html>  \n") == R::ok);
        BEAST_EXPECT(check("<html></html>\t\r\f") == R::ok);
        // BOM followed by doctype.
        BEAST_EXPECT(
            check("\xEF\xBB\xBF<!DOCTYPE html><html></html>") == R::ok);
        // Whitespace between </html and >.
        BEAST_EXPECT(check("<html></html  >") == R::ok);
        // Content directly inside <html>, with no <body> wrapper.
        BEAST_EXPECT(check("<html><p>text</p></html>") == R::ok);
        // "<html/>" is accepted because '/' terminates the tag name, and
        // the trailing "</html>" still satisfies the end-tag requirement.
        BEAST_EXPECT(check("<html/></html>") == R::ok);

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

        // BOM only is empty.
        BEAST_EXPECT(check("\xEF\xBB\xBF") == R::empty);
        // BOM followed by whitespace only.
        BEAST_EXPECT(check("\xEF\xBB\xBF  \n\t") == R::empty);
        // An end tag first. Rejected by the opening gate, before any search
        // for <html> runs, hence noDoctype rather than unclosed.
        BEAST_EXPECT(check("</html><html></html>") == R::noDoctype);
        // Missing > on </html> end tag.
        BEAST_EXPECT(check("<html></html  ") == R::unclosed);
        // Some other element first. The document must *open* with a doctype
        // or <html>, so this never reaches the <html> search either.
        BEAST_EXPECT(check("<head></head><html></html>") == R::noDoctype);

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
        // U+FFFE / U+FFFF are well-formed but rejected, as everywhere else
        // on ledger (U+FFFE is a byte-swapped BOM).
        BEAST_EXPECT(check(wrap("\xEF\xBF\xBE")) == R::badUTF8);
        BEAST_EXPECT(check(wrap("\xEF\xBF\xBF")) == R::badUTF8);

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

            // ...and so does ledger_entry by account, which is the lookup a
            // client without sfAppLoaderID in hand actually has.
            auto const byAcct = env.rpc(
                "json",
                "ledger_entry",
                R"({"app_loader":")" + alice.human() + R"("})");
            BEAST_EXPECT(
                byAcct[jss::result][jss::node][sfLedgerEntryType.fieldName] ==
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

    // validate() is consensus-critical: any change to what it accepts needs
    // an amendment. These vectors pin behaviour at the edges that the
    // shallow checks leave open, so an innocent-looking refactor that moves
    // any of them fails here instead of forking the network.
    void
    testValidatorPinned()
    {
        testcase("validator pinned edge cases");
        using R = appLoader::Result;
        auto b = [](char const* s, std::size_t n) {
            return check(std::string(s, n));
        };
#define PIN(lit, expect) BEAST_EXPECT(b(lit, sizeof(lit) - 1) == R::expect)

        // The <html start tag may sit inside a comment; nothing parses
        // comments. Accepted by design -- clients must not rely on
        // structure beyond the checks documented in AppLoader.h.
        PIN("<!DOCTYPE html><!-- <html> --><p></p></html>", ok);
        // ...but a leading comment is not a doctype.
        PIN("<!-- x --><html></html>", noDoctype);
        // A trailing comment after </html> is rejected even though browsers
        // accept it (build tools commonly emit one).
        PIN("<!DOCTYPE html><html></html><!-- -->", trailingGarbage);

        // A literal "</html>" inside a script string does not end the
        // document early: the last well-formed end tag wins.
        PIN("<html><script>'</html>'</script></html>", ok);
        PIN("<html></html></html>", ok);
        PIN("<!DOCTYPE html><htmlx><html></html>", ok);
        // "</html/>" is not an end tag here.
        PIN("<html></html/>", unclosed);
        PIN("<html></HTML\t>", ok);

        // Only one BOM is skipped.
        PIN("\xEF\xBB\xBF\xEF\xBB\xBF<html></html>", noDoctype);
        // An interior U+FEFF is ordinary text.
        PIN("<html>\xEF\xBB\xBF</html>", ok);

        // FORM FEED is HTML whitespace and is accepted everywhere,
        // including as the doctype separator and inside the start tag.
        // (AppLoader.h's summary lists only TAB, LF, CR -- the code is
        // authoritative; fix the comment, not this vector.)
        PIN("<!doctype\x0Chtml><html></html>", ok);
        PIN("<html\x0C></html>", ok);
        PIN("<html\r\n></html>", ok);
        PIN("<html></html>\x0C", ok);

        // Other C0 and DEL are rejected; C1 (U+0080..U+009F) is not.
        PIN("<html>\x0B</html>", badControlChar);
        PIN("<html>\x1B</html>", badControlChar);
        PIN("<html>\x7F</html>", badControlChar);
        PIN("<html></html>\x0B", badControlChar);
        PIN("<html>\xC2\x85</html>", ok);

        // Only ASCII whitespace may trail: NBSP and IDEOGRAPHIC SPACE do not
        // count.
        PIN("<html></html>\xC2\xA0", trailingGarbage);
        PIN("<html></html>\xE3\x80\x80", trailingGarbage);

        PIN("<!doctype><html></html>", noDoctype);

        // Precedence: UTF-8 is checked before control characters, and size
        // before either.
        PIN("\xC0\xAF\x01", badUTF8);
        BEAST_EXPECT(
            check(std::string(maxAppLoaderLength + 1, '\x01')) == R::tooLarge);

        // Content is not policed beyond structure: a meta refresh to an
        // arbitrary site passes. The hosting endpoint is therefore an open
        // redirector for anyone willing to pay the fee.
        PIN("<html><meta http-equiv=\"refresh\" "
            "content=\"0;url=https://evil.example\"></html>",
            ok);
#undef PIN

        // nullptr with a non-zero size is defined, not UB.
        BEAST_EXPECT(appLoader::validate(nullptr, 5) == R::empty);
        BEAST_EXPECT(appLoader::validate(nullptr, 0) == R::empty);

        // Size is measured in bytes, not code points, at the boundary.
        {
            std::string const h = "<html>", t = "</html>";
            std::string const fits = h +
                std::string(maxAppLoaderLength - h.size() - t.size() - 2, 'x') +
                "\xC2\xA2" + t;
            BEAST_EXPECT(fits.size() == maxAppLoaderLength);
            BEAST_EXPECT(check(fits) == R::ok);
            BEAST_EXPECT(check(fits + " ") == R::tooLarge);
        }

        // Totality: random inputs built from the tokens the validator cares
        // about never crash and are deterministic. (Run under ASan/UBSan in
        // CI; standalone, 300k cases of this generator are clean.)
        {
            static char const* const frag[] = {
                "<html",
                "<HTML",
                ">",
                "</html",
                "</html>",
                "<!doctype",
                " ",
                "\t",
                "\n",
                "\f",
                "\xEF\xBB\xBF",
                "\xEF\xBF\xBE",
                "\xF4\x8F\xBF",
                "\xC2",
                "\xA2",
                "/",
                "x"};
            std::mt19937_64 g{0x5057414c};
            for (int i = 0; i < 20000; ++i)
            {
                std::string s;
                std::size_t const n = g() % 48;
                for (std::size_t k = 0; k < n; ++k)
                {
                    if (g() % 4)
                        s += frag[g() % std::size(frag)];
                    else
                        s += static_cast<char>(g());
                }
                auto const r1 = check(s);
                auto const r2 = check(s);
                BEAST_EXPECT(r1 == r2);
                BEAST_EXPECT(
                    std::string(appLoader::to_string(r1)) != "unknown");
            }
        }
    }

    // The design claim is that a loader can be published by an account
    // sitting at its reserve floor, and that it never touches OwnerCount.
    void
    testReserveFloor(FeatureBitset features)
    {
        testcase("reserve floor and owner count");
        using namespace jtx;

        Env env{*this, features};
        auto const alice = Account("alice");
        auto const base = env.current()->fees().base;
        std::string const doc = docOfSize(maxAppLoaderLength);
        auto const needed = base + XRPAmount{std::int64_t(doc.size())};

        env.fund(
            drops(env.current()->fees().accountReserve(0) + needed),
            noripple(alice));
        env.close();

        auto jt = noop(alice);
        jt[sfAppLoader.fieldName] = strHex(doc);
        env(jt, fee(needed));
        env.close();

        BEAST_EXPECT(env.le(keylet::appLoader(alice.id())));
        BEAST_EXPECT(
            env.balance(alice) ==
            drops(env.current()->fees().accountReserve(0)));
        env.require(owners(alice, 0));
    }

    void
    testFeeVariants(FeatureBitset features)
    {
        testcase("fee: overwrite and multisign");
        using namespace jtx;

        Env env{*this, features};
        auto const alice = Account("alice");
        auto const bogie = Account("bogie");
        auto const demon = Account("demon");
        env.fund(XRP(1000), alice);
        env.close();
        auto const base = env.current()->fees().base;

        std::string const doc = docOfSize(2000);
        auto jt = noop(alice);
        jt[sfAppLoader.fieldName] = strHex(doc);

        env(jt, fee(base + XRPAmount{2000}));
        env.close();

        // Overwrite with identical bytes still pays per byte: there is no
        // "unchanged" discount, and there must not be one, since the fee is
        // the only thing standing in for a reserve.
        env(jt, fee(base + XRPAmount{1999}), ter(telINSUF_FEE_P));
        env(jt, fee(base + XRPAmount{2000}));
        env.close();

        // Multisigned: base scales with signers, the per-byte part does not.
        env(signers(alice, 2, {{bogie, 1}, {demon, 1}}));
        env.close();
        auto const msFee = base * 3 + XRPAmount{2000};
        env(jt,
            msig(bogie, demon),
            fee(msFee - XRPAmount{1}),
            ter(telINSUF_FEE_P));
        env(jt, msig(bogie, demon), fee(msFee));
        env.close();
        BEAST_EXPECT(env.le(keylet::appLoader(alice.id())));
    }

    void
    testMetadata(FeatureBitset features)
    {
        testcase("metadata and threading");
        using namespace jtx;

        Env env{*this, features};
        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        auto countNodes = [&](SField const& kind) {
            int n = 0;
            for (auto const& node : env.meta()->getFieldArray(sfAffectedNodes))
                if (node.getFName() == kind &&
                    node.getFieldU16(sfLedgerEntryType) == ltAPP_LOADER)
                    ++n;
            return n;
        };

        auto jt = noop(alice);
        jt[sfAppLoader.fieldName] = strHex(goodDoc());
        env(jt, fee(XRP(1)));
        env.close();
        BEAST_EXPECT(countNodes(sfCreatedNode) == 1);
        auto const created = env.tx()->getTransactionID();
        if (auto const sle = env.le(keylet::appLoader(alice.id()));
            BEAST_EXPECT(sle))
            BEAST_EXPECT((*sle)[sfPreviousTxnID] == created);

        jt[sfAppLoader.fieldName] = strHex(std::string("<html>2</html>"));
        env(jt, fee(XRP(1)));
        env.close();
        BEAST_EXPECT(countNodes(sfModifiedNode) == 1);
        if (auto const sle = env.le(keylet::appLoader(alice.id()));
            BEAST_EXPECT(sle))
            BEAST_EXPECT(
                (*sle)[sfPreviousTxnID] == env.tx()->getTransactionID());

        jt[sfAppLoader.fieldName] = "";
        env(jt, fee(XRP(1)));
        env.close();
        BEAST_EXPECT(countNodes(sfDeletedNode) == 1);
    }

    void
    testCombinedAndTickets(FeatureBitset features)
    {
        testcase("combined with other AccountSet fields; tickets");
        using namespace jtx;

        Env env{*this, features};
        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        // Flags and the loader in one transaction both take effect.
        auto jt = fset(alice, asfRequireDest);
        jt[sfAppLoader.fieldName] = strHex(goodDoc());
        env(jt, fee(XRP(1)));
        env.close();
        BEAST_EXPECT(env.le(keylet::appLoader(alice.id())));
        BEAST_EXPECT(env.le(alice)->getFlags() & lsfRequireDestTag);

        // A malformed loader rejects the whole transaction, including the
        // flag change riding with it.
        auto bad = fclear(alice, asfRequireDest);
        bad[sfAppLoader.fieldName] = strHex(std::string("<div></div>"));
        env(bad, fee(XRP(1)), ter(temMALFORMED));
        env.close();
        BEAST_EXPECT(env.le(alice)->getFlags() & lsfRequireDestTag);

        // Via ticket.
        std::uint32_t const tkt = env.seq(alice) + 1;
        env(ticket::create(alice, 1));
        env.close();
        auto jtt = noop(alice);
        jtt[sfAppLoader.fieldName] = "";
        env(jtt, ticket::use(tkt), fee(XRP(1)));
        env.close();
        BEAST_EXPECT(!env.le(keylet::appLoader(alice.id())));
    }

    void
    testRecreateAfterDelete(FeatureBitset features)
    {
        testcase("account resurrected after delete");
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

        for (int i = 0; i < 256; ++i)
            env.close();
        env(acctdelete(alice, bob),
            fee(drops(env.current()->fees().increment)));
        env.close();

        env(pay(bob, alice, XRP(100)));
        env.close();

        // Nothing leaks across the account's two lives.
        BEAST_EXPECT(!env.le(keylet::appLoader(alice.id())));
        if (auto const root = env.le(alice); BEAST_EXPECT(root))
            BEAST_EXPECT(!root->isFieldPresent(sfAppLoaderID));

        env(jt, fee(XRP(1)));
        env.close();
        BEAST_EXPECT(env.le(keylet::appLoader(alice.id())));
    }

    void
    testLedgerEntryRPC(FeatureBitset features)
    {
        testcase("ledger_entry app_loader errors");
        using namespace jtx;

        Env env{*this, features};
        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        auto le = [&](std::string const& v) {
            return env.rpc(
                "json", "ledger_entry", R"({"app_loader":)" + v + "}");
        };

        BEAST_EXPECT(
            le(R"("notanaddress")")[jss::result][jss::error] ==
            "malformedAddress");
        BEAST_EXPECT(
            le("\"" + toBase58(AccountID{}) + "\"")[jss::result][jss::error] ==
            "malformedAddress");
        // Well-formed but absent.
        BEAST_EXPECT(
            le("\"" + alice.human() + "\"")[jss::result][jss::error] ==
            "entryNotFound");
        // Non-string parameter must not throw.
        BEAST_EXPECT(le("{}")[jss::result].isMember(jss::error));
        BEAST_EXPECT(le("123")[jss::result].isMember(jss::error));
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnabled(features);
        testSetAndRemove(features);
        testFee(features);
        testMalformed(features);
        testAccountDelete(features);
        testReserveFloor(features);
        testFeeVariants(features);
        testMetadata(features);
        testCombinedAndTickets(features);
        testRecreateAfterDelete(features);
        testLedgerEntryRPC(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        testValidator();
        testUTF8();
        testValidatorPinned();
        testWithFeats(supported_amendments());
    }
};

BEAST_DEFINE_TESTSUITE(PWALoader, app, ripple);

}  // namespace test
}  // namespace ripple
