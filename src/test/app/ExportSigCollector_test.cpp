//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES WHATSOEVER
    RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
    CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
    CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpld/app/misc/ExportSigCollector.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/digest.h>

namespace ripple {
namespace test {

namespace {

uint256
origin(std::uint32_t value)
{
    Serializer s;
    s.add32(value);
    return sha512Half(s.slice());
}

PublicKey
publicKey(char const* hex)
{
    auto const raw = strUnHex(hex);
    return PublicKey{makeSlice(*raw)};
}

Buffer
signature(std::uint8_t value)
{
    std::uint8_t bytes[] = {value, std::uint8_t(value + 1)};
    return Buffer{bytes, sizeof(bytes)};
}

}  // namespace

class ExportSigCollector_test : public beast::unit_test::suite
{
    PublicKey const keyA_ = publicKey(
        "0388935426E0D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97C05"
        "2");
    PublicKey const keyB_ = randomKeyPair(KeyType::secp256k1).first;

    static ExportSigCollector::Contribution
    contribution(
        ExportSigCollector::Position position,
        PublicKey const& key,
        std::uint8_t sig)
    {
        return {position, key, signature(sig)};
    }

public:
    void
    testAdmissionAndConflict()
    {
        testcase("admission and absorbing conflict");

        ExportSigCollector collector;
        auto const w = origin(1);
        BEAST_EXPECT(collector.registerOrigin(w, 9));
        auto first = collector.beginAttributedAdmission(
            w, contribution(7, keyA_, 1), 10);
        BEAST_EXPECT(first.result == ExportSigCollector::BeginResult::verify);
        BEAST_EXPECT(first.ticket.has_value());
        BEAST_EXPECT(collector.fullUnionSnapshot().empty());
        if (!first.ticket)
            return;

        auto accepted =
            collector.admitContribution(std::move(*first.ticket), true, 10);
        BEAST_EXPECT(
            accepted.result == ExportSigCollector::AdmitResult::accepted);
        auto snapshot = collector.fullUnionSnapshot();
        BEAST_EXPECT(snapshot.at(w).size() == 1);
        BEAST_EXPECT(snapshot.at(w).front().position == 7);

        auto duplicate = collector.beginAttributedAdmission(
            w, contribution(7, keyA_, 1), 11);
        BEAST_EXPECT(
            duplicate.result == ExportSigCollector::BeginResult::duplicate);

        auto second = collector.beginAttributedAdmission(
            w, contribution(7, keyB_, 2), 11);
        auto third = collector.beginAttributedAdmission(
            w, contribution(7, keyA_, 3), 11);
        BEAST_EXPECT(second.ticket.has_value());
        BEAST_EXPECT(third.result == ExportSigCollector::BeginResult::capacity);
        if (!second.ticket)
            return;

        auto conflicted =
            collector.admitContribution(std::move(*second.ticket), true, 11);
        BEAST_EXPECT(
            conflicted.result == ExportSigCollector::AdmitResult::conflicted);
        BEAST_EXPECT(conflicted.priorContribution.has_value());
        BEAST_EXPECT(conflicted.conflictingContribution.has_value());
        BEAST_EXPECT(
            collector.positionStatus(w, 7) ==
            ExportSigCollector::PositionStatus::conflicted);
        BEAST_EXPECT(collector.fullUnionSnapshot().empty());
        BEAST_EXPECT(
            collector.beginAttributedAdmission(w, contribution(7, keyA_, 4), 12)
                .result == ExportSigCollector::BeginResult::conflicted);
    }

    void
    testReservationAndOriginLifecycle()
    {
        testcase("verification reservations and origin registration");

        ExportSigCollector collector;
        auto const w = origin(2);
        BEAST_EXPECT(collector.registerOrigin(w, 19));
        auto invalid = collector.beginAttributedAdmission(
            w, contribution(3, keyA_, 10), 20);
        BEAST_EXPECT(invalid.ticket.has_value());
        if (!invalid.ticket)
            return;
        BEAST_EXPECT(
            collector.admitContribution(std::move(*invalid.ticket), false, 20)
                .result == ExportSigCollector::AdmitResult::invalid);

        auto valid = collector.beginAttributedAdmission(
            w, contribution(3, keyA_, 11), 20);
        BEAST_EXPECT(valid.ticket.has_value());
        if (!valid.ticket)
            return;
        BEAST_EXPECT(
            collector.admitContribution(std::move(*valid.ticket), true, 20)
                .result == ExportSigCollector::AdmitResult::accepted);

        auto other = collector.beginAttributedAdmission(
            w, contribution(4, keyB_, 12), 20);
        BEAST_EXPECT(other.ticket.has_value());
        if (!other.ticket)
            return;
        BEAST_EXPECT(
            collector.admitContribution(std::move(*other.ticket), true, 20)
                .result == ExportSigCollector::AdmitResult::accepted);

        BEAST_EXPECT(collector.registerOrigin(w, 21));
        BEAST_EXPECT(
            collector.positionStatus(w, 3) ==
            ExportSigCollector::PositionStatus::unique);
        BEAST_EXPECT(collector.fullUnionSnapshot().at(w).size() == 2);

        // Idempotent registration refreshes the stale-cleanup cursor.
        collector.cleanupStale(277);
        BEAST_EXPECT(!collector.fullUnionSnapshot().empty());
        collector.cleanupStale(278);
        BEAST_EXPECT(collector.fullUnionSnapshot().empty());
    }

    void
    testMalformedBoundaries()
    {
        testcase("malformed contribution boundaries");

        ExportSigCollector collector;
        auto good = contribution(0, keyA_, 1);
        BEAST_EXPECT(
            collector.beginAttributedAdmission(uint256{}, good).result ==
            ExportSigCollector::BeginResult::malformed);

        auto const w = origin(3);
        BEAST_EXPECT(
            collector.beginAttributedAdmission(w, good, 1).result ==
            ExportSigCollector::BeginResult::unknownOrigin);
        BEAST_EXPECT(collector.registerOrigin(w, 1));

        good.position = ExportLimits::maxCommitteeMembers;
        BEAST_EXPECT(
            collector.beginAttributedAdmission(w, good, 1).result ==
            ExportSigCollector::BeginResult::malformed);

        good.position = 0;
        good.signature = Buffer{};
        BEAST_EXPECT(
            collector.beginAttributedAdmission(w, good, 1).result ==
            ExportSigCollector::BeginResult::malformed);

        std::vector<std::uint8_t> oversized(
            ExportLimits::maxCanonicalExportSignatureBytes + 1, 0xAB);
        good.signature = Buffer{oversized.data(), oversized.size()};
        BEAST_EXPECT(
            collector.beginAttributedAdmission(w, good, 1).result ==
            ExportSigCollector::BeginResult::malformed);

        good = contribution(5, keyA_, 5);
        auto abandoned = collector.beginAttributedAdmission(w, good, 2);
        BEAST_EXPECT(abandoned.ticket.has_value());
        if (!abandoned.ticket)
            return;
        BEAST_EXPECT(collector.cancelAdmission(std::move(*abandoned.ticket)));
        BEAST_EXPECT(
            collector.beginAttributedAdmission(w, good, 2).result ==
            ExportSigCollector::BeginResult::verify);

        auto expired =
            collector.beginAttributedAdmission(w, contribution(6, keyA_, 6), 3);
        BEAST_EXPECT(expired.ticket.has_value());
        BEAST_EXPECT(
            collector.beginAttributedAdmission(w, contribution(6, keyB_, 7), 5)
                .result == ExportSigCollector::BeginResult::verify);

        collector.clear(w);
        BEAST_EXPECT(
            collector.beginAttributedAdmission(w, good, 7).result ==
            ExportSigCollector::BeginResult::unknownOrigin);
        BEAST_EXPECT(collector.registerOrigin(w, 7));
    }

    void
    testOriginRegistrationBounds()
    {
        testcase("origin registration bounds");

        ExportSigCollector collector;
        BEAST_EXPECT(!collector.registerOrigin(uint256{}, 1));
        BEAST_EXPECT(!collector.registerOrigin(origin(100), 0));

        bool registeredAll = true;
        for (std::size_t i = 0; i < ExportSigCollector::maxTrackedOrigins; ++i)
            registeredAll =
                collector.registerOrigin(
                    origin(static_cast<std::uint32_t>(i + 1'000)), 1) &&
                registeredAll;
        BEAST_EXPECT(registeredAll);
        BEAST_EXPECT(!collector.registerOrigin(origin(99'999), 1));
        BEAST_EXPECT(collector.registerOrigin(origin(1'000), 2));
    }

    void
    run() override
    {
        testAdmissionAndConflict();
        testReservationAndOriginLifecycle();
        testMalformedBoundaries();
        testOriginRegistrationBounds();
    }
};

BEAST_DEFINE_TESTSUITE(ExportSigCollector, app, ripple);

}  // namespace test
}  // namespace ripple
