//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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
#include <test/shamap/common.h>
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LiteWatch.h>
#include <xrpld/core/Config.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/protocol/Indexes.h>

namespace ripple {
namespace test {

class LiteWatch_test : public beast::unit_test::suite
{
    static Config
    liteConfig(std::vector<std::string> watch)
    {
        Config cfg;
        cfg.LITE_MODE = true;
        cfg.LITE_WATCH = std::move(watch);
        return cfg;
    }

    void
    testConfigLines()
    {
        testcase("config lines");

        using namespace jtx;
        Account const alice{"alice"};

        std::string err;
        auto const acct = parseLiteWatchLine(" " + alice.human() + " ", err);
        BEAST_EXPECT(acct && acct->first && *acct->first == alice.id());
        BEAST_EXPECT(acct && acct->second == keylet::account(alice).key);

        auto const key = keylet::fees().key;
        auto const raw = parseLiteWatchLine(to_string(key), err);
        BEAST_EXPECT(raw && !raw->first && raw->second == key);

        BEAST_EXPECT(!parseLiteWatchLine("", err));
        BEAST_EXPECT(!parseLiteWatchLine("not a key", err));
        BEAST_EXPECT(!parseLiteWatchLine(to_string(key).substr(1), err));
    }

    void
    testWatchSet()
    {
        testcase("watch set");

        using namespace jtx;
        Account const alice{"alice"};
        Account const bob{"bob"};

        Env env{*this};
        LiteWatch watch{liteConfig({alice.human()}), env.journal};

        BEAST_EXPECT(watch.enabled());
        BEAST_EXPECT(watch.isWatchedAccount(alice));
        BEAST_EXPECT(watch.isWatched(keylet::account(alice).key));
        BEAST_EXPECT(!watch.isWatchedAccount(bob));

        // The keys every ledger needs are there, and cannot be removed.
        for (auto const& k :
             {keylet::fees().key,
              keylet::amendments().key,
              keylet::skip().key,
              keylet::negativeUNL().key})
        {
            BEAST_EXPECT(watch.isWatched(k));
            BEAST_EXPECT(!watch.unwatchKey(k));
            BEAST_EXPECT(watch.isWatched(k));
        }
        BEAST_EXPECT(watch.isWatched(keylet::hook(alice).key));
        BEAST_EXPECT(watch.size() == 6);

        BEAST_EXPECT(watch.watchAccount(bob));
        BEAST_EXPECT(!watch.watchAccount(bob));
        BEAST_EXPECT(watch.isWatched(keylet::account(bob).key));
        BEAST_EXPECT(watch.unwatchAccount(bob));
        BEAST_EXPECT(!watch.isWatched(keylet::account(bob).key));
        BEAST_EXPECT(!watch.isWatched(keylet::hook(bob).key));
        BEAST_EXPECT(!watch.unwatchAccount(bob));

        auto const k = keylet::ownerDir(alice).key;
        BEAST_EXPECT(watch.watchKey(k, LiteWatch::Tier::discovered, "test"));
        BEAST_EXPECT(!watch.watchKey(k));
        // Asking for a discovered key by name promotes it: it now survives
        // the account it was discovered through being unwatched.
        BEAST_EXPECT(watch.unwatchAccount(alice));
        BEAST_EXPECT(watch.isWatched(k));
        BEAST_EXPECT(watch.unwatchKey(k));
        BEAST_EXPECT(!watch.isWatched(k));

        auto const json = watch.getJson();
        BEAST_EXPECT(json[jss::enabled].asBool());
        BEAST_EXPECT(json["system"].asUInt() == 4);
    }

    void
    testDiscover()
    {
        testcase("discover");

        using namespace jtx;
        Account const gw{"gw"};
        Account const alice{"alice"};
        Account const bob{"bob"};
        auto const USD = gw["USD"];

        Env env{*this};
        env.fund(XRP(10000), gw, alice, bob);
        env.close();

        LiteWatch watch{liteConfig({alice.human()}), env.journal};
        auto const line = keylet::line(alice, gw, USD.currency).key;
        auto const bobLine = keylet::line(bob, gw, USD.currency).key;

        // Nothing in the funding ledger concerns alice's objects beyond her
        // root, which is configured already.
        watch.discover(*env.closed());
        BEAST_EXPECT(!watch.isWatched(keylet::account(bob).key));
        BEAST_EXPECT(!watch.isWatched(line));

        env.trust(USD(100), alice);
        env.trust(USD(100), bob);
        env(pay(gw, alice, USD(50)));
        env.close();

        auto const added = watch.discover(*env.closed());
        BEAST_EXPECT(added > 0);

        // alice's new trust line names her as a party; bob's does not.
        BEAST_EXPECT(watch.isWatched(line));
        BEAST_EXPECT(!watch.isWatched(bobLine));
        BEAST_EXPECT(!watch.isWatched(keylet::account(bob).key));
        // Her owner directory names her as its owner.
        BEAST_EXPECT(watch.isWatched(keylet::ownerDir(alice).key));

        // Paying the line back to zero and clearing the limit deletes it,
        // and a discovered object that is deleted is forgotten.
        env(pay(alice, gw, USD(50)));
        env(trust(alice, USD(0)));
        env.close();
        watch.discover(*env.closed());
        BEAST_EXPECT(!watch.isWatched(line));
        BEAST_EXPECT(watch.isWatched(keylet::account(alice).key));

        // With discovery off nothing is learned.
        auto cfg = liteConfig({alice.human()});
        cfg.LITE_DISCOVER = false;
        LiteWatch quiet{cfg, env.journal};
        env.trust(USD(100), alice);
        env.close();
        BEAST_EXPECT(quiet.discover(*env.closed()) == 0);
        BEAST_EXPECT(!quiet.isWatched(line));
    }

    void
    testPartialSync()
    {
        testcase("partial sync");

        using namespace jtx;
        Account const gw{"gw"};
        Account const alice{"alice"};
        Account const bob{"bob"};
        auto const USD = gw["USD"];

        Env env{*this};
        env.fund(XRP(10000), gw, alice, bob);
        env.trust(USD(100), alice);
        env(pay(gw, alice, USD(50)));
        env.close();

        auto const ledger =
            std::dynamic_pointer_cast<Ledger const>(env.closed());
        if (!BEAST_EXPECT(ledger))
            return;
        auto const& full = ledger->stateMap();

        LiteWatch watch{liteConfig({alice.human()}), env.journal};
        watch.discover(*ledger);
        auto const line = keylet::line(alice, gw, USD.currency).key;
        BEAST_EXPECT(watch.isWatched(line));

        // A whole map needs nothing.
        BEAST_EXPECT(watch.missingNodes(full, 100).empty());

        // A map holding just the root, in a family of its own so that
        // nothing resolves from a shared cache or store: what a lite node
        // holds after the root reply. Feed it what a peer would answer, one
        // round per level, until every watched key reads.
        TestNodeFamily family{env.journal};
        SHAMap partial{SHAMapType::STATE, family};
        partial.setSynching();
        {
            Serializer s;
            full.serializeRoot(s);
            BEAST_EXPECT(partial.addRootNode(full.getHash(), s.slice(), nullptr)
                             .isGood());
        }

        int rounds = 0;
        std::size_t held = 1;  // the root
        for (; rounds < 32; ++rounds)
        {
            auto const missing = watch.missingNodes(partial, 100);
            if (missing.empty())
                break;

            held += missing.size();
            for (auto const& [id, hash] : missing)
            {
                std::vector<std::pair<SHAMapNodeID, Blob>> data;
                full.getNodeFat(id, data, false, 0);
                if (!BEAST_EXPECT(!data.empty()))
                    return;
                BEAST_EXPECT(
                    partial.addKnownNode(id, makeSlice(data[0].second), nullptr)
                        .isGood());
            }
        }

        // Converged, in no more rounds than the map is deep.
        BEAST_EXPECT(rounds > 0 && rounds < 32);
        BEAST_EXPECT(watch.missingNodes(partial, 100).empty());
        BEAST_EXPECT(partial.getHash() == full.getHash());

        std::optional<std::pair<SHAMapNodeID, uint256>> gap;
        BEAST_EXPECT(partial.peekItemPartial(line, gap) && !gap);
        BEAST_EXPECT(
            partial.peekItemPartial(keylet::account(alice).key, gap) && !gap);
        BEAST_EXPECT(partial.peekItemPartial(keylet::fees().key, gap) && !gap);

        // bob was never asked for: his path stops at a node not held.
        BEAST_EXPECT(
            !partial.peekItemPartial(keylet::account(bob).key, gap) && gap);

        // And the partial map is a fraction of the whole one. (visitNodes()
        // cannot be asked of the partial map: it dereferences every child.)
        std::size_t whole = 0;
        full.visitNodes([&](SHAMapTreeNode&) {
            ++whole;
            return true;
        });
        BEAST_EXPECT(held < whole);
    }

public:
    void
    run() override
    {
        testConfigLines();
        testWatchSet();
        testDiscover();
        testPartialSync();
    }
};

BEAST_DEFINE_TESTSUITE(LiteWatch, app, ripple);

}  // namespace test
}  // namespace ripple
