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

#include <xrpld/app/misc/ExportSigCollectorV2.h>
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

class ExportSigCollectorV2_test : public beast::unit_test::suite
{
    PublicKey const keyA_ = publicKey(
        "0388935426E0D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97C05"
        "2");
    PublicKey const keyB_ = randomKeyPair(KeyType::secp256k1).first;

    static ExportSigCollectorV2::Contribution
    contribution(
        ExportSigCollectorV2::Position position,
        PublicKey const& key,
        std::uint8_t sig)
    {
        return {position, key, signature(sig)};
    }

public:
    void
    testAdmissionAndConflict()
    {
        testcase("v2 admission and absorbing conflict");

        ExportSigCollectorV2 collector;
        auto const w = origin(1);
        auto const publication = collector.reopenPublication(w, w, 9);
        BEAST_EXPECT(publication.has_value());
        auto first = collector.beginAdmission(w, contribution(7, keyA_, 1), 10);
        BEAST_EXPECT(first.result == ExportSigCollectorV2::BeginResult::verify);
        BEAST_EXPECT(first.ticket.has_value());
        BEAST_EXPECT(collector.fullUnionSnapshot().empty());
        if (!first.ticket)
            return;

        auto accepted =
            collector.admitContribution(std::move(*first.ticket), true, 10);
        BEAST_EXPECT(
            accepted.result == ExportSigCollectorV2::AdmitResult::accepted);
        auto snapshot = collector.fullUnionSnapshot();
        BEAST_EXPECT(snapshot.at(w).size() == 1);
        BEAST_EXPECT(snapshot.at(w).front().position == 7);

        auto duplicate =
            collector.beginAdmission(w, contribution(7, keyA_, 1), 11);
        BEAST_EXPECT(
            duplicate.result == ExportSigCollectorV2::BeginResult::duplicate);

        auto second =
            collector.beginAdmission(w, contribution(7, keyB_, 2), 11);
        auto third = collector.beginAdmission(w, contribution(7, keyA_, 3), 11);
        BEAST_EXPECT(second.ticket.has_value());
        BEAST_EXPECT(
            third.result == ExportSigCollectorV2::BeginResult::capacity);
        if (!second.ticket)
            return;

        auto conflicted =
            collector.admitContribution(std::move(*second.ticket), true, 11);
        BEAST_EXPECT(
            conflicted.result == ExportSigCollectorV2::AdmitResult::conflicted);
        BEAST_EXPECT(conflicted.priorContribution.has_value());
        BEAST_EXPECT(
            collector.positionStatus(w, 7) ==
            ExportSigCollectorV2::PositionStatus::conflicted);
        BEAST_EXPECT(collector.fullUnionSnapshot().empty());
        BEAST_EXPECT(
            collector.beginAdmission(w, contribution(7, keyA_, 4), 12).result ==
            ExportSigCollectorV2::BeginResult::conflicted);
    }

    void
    testReservationAndPublicationLifecycle()
    {
        testcase("v2 verification reservations and publication reopening");

        ExportSigCollectorV2 collector;
        auto const w = origin(2);
        auto publication = collector.reopenPublication(w, w, 19);
        BEAST_EXPECT(publication.has_value());
        auto invalid =
            collector.beginAdmission(w, contribution(3, keyA_, 10), 20);
        BEAST_EXPECT(invalid.ticket.has_value());
        if (!invalid.ticket)
            return;
        BEAST_EXPECT(
            collector.admitContribution(std::move(*invalid.ticket), false, 20)
                .result == ExportSigCollectorV2::AdmitResult::invalid);

        auto valid =
            collector.beginAdmission(w, contribution(3, keyA_, 11), 20);
        BEAST_EXPECT(valid.ticket.has_value());
        if (!valid.ticket)
            return;
        BEAST_EXPECT(
            collector.admitContribution(std::move(*valid.ticket), true, 20)
                .result == ExportSigCollectorV2::AdmitResult::accepted);

        auto other =
            collector.beginAdmission(w, contribution(4, keyB_, 12), 20);
        BEAST_EXPECT(other.ticket.has_value());
        if (!other.ticket)
            return;
        BEAST_EXPECT(
            collector.admitContribution(std::move(*other.ticket), true, 20)
                .result == ExportSigCollectorV2::AdmitResult::accepted);

        BEAST_EXPECT(publication.has_value());
        if (!publication)
            return;
        BEAST_EXPECT(collector.claimPublication(*publication, 3, 2));
        BEAST_EXPECT(collector.publicationGeneration(w) == 1);
        BEAST_EXPECT(!collector.claimPublication(*publication, 3, 2));

        // Reopening the same trigger is idempotent and does not reset slots.
        auto samePublication = collector.reopenPublication(w, w, 21);
        BEAST_EXPECT(samePublication.has_value());
        if (!samePublication)
            return;
        BEAST_EXPECT(collector.publicationGeneration(w) == 1);
        BEAST_EXPECT(!collector.claimPublication(*samePublication, 3, 2));

        auto const nextTrigger = origin(22);
        auto nextPublication = collector.reopenPublication(w, nextTrigger, 22);
        BEAST_EXPECT(nextPublication.has_value());
        if (!nextPublication)
            return;
        BEAST_EXPECT(collector.publicationGeneration(w) == 2);
        BEAST_EXPECT(!collector.claimPublication(*publication, 4, 2));
        BEAST_EXPECT(collector.claimPublication(*nextPublication, 4, 1));
        BEAST_EXPECT(
            collector.positionStatus(w, 3) ==
            ExportSigCollectorV2::PositionStatus::unique);
        BEAST_EXPECT(collector.fullUnionSnapshot().at(w).size() == 2);

        collector.cleanupStale(278);
        BEAST_EXPECT(!collector.fullUnionSnapshot().empty());
        collector.cleanupStale(279);
        BEAST_EXPECT(collector.fullUnionSnapshot().empty());
    }

    void
    testMalformedBoundaries()
    {
        testcase("v2 malformed contribution boundaries");

        ExportSigCollectorV2 collector;
        auto good = contribution(0, keyA_, 1);
        BEAST_EXPECT(
            collector.beginAdmission(uint256{}, good).result ==
            ExportSigCollectorV2::BeginResult::malformed);

        auto const w = origin(3);
        BEAST_EXPECT(
            collector.beginAdmission(w, good, 1).result ==
            ExportSigCollectorV2::BeginResult::unknownOrigin);
        BEAST_EXPECT(collector.reopenPublication(w, w, 1).has_value());

        good.position = ExportLimits::maxValidatorUniverseMembers;
        BEAST_EXPECT(
            collector.beginAdmission(w, good, 1).result ==
            ExportSigCollectorV2::BeginResult::malformed);

        good.position = 0;
        good.signature = Buffer{};
        BEAST_EXPECT(
            collector.beginAdmission(w, good, 1).result ==
            ExportSigCollectorV2::BeginResult::malformed);

        std::vector<std::uint8_t> oversized(
            ExportSigCollectorV2::maxSignatureBytes + 1, 0xAB);
        good.signature = Buffer{oversized.data(), oversized.size()};
        BEAST_EXPECT(
            collector.beginAdmission(w, good, 1).result ==
            ExportSigCollectorV2::BeginResult::malformed);

        good = contribution(5, keyA_, 5);
        auto abandoned = collector.beginAdmission(w, good, 2);
        BEAST_EXPECT(abandoned.ticket.has_value());
        if (!abandoned.ticket)
            return;
        BEAST_EXPECT(collector.cancelAdmission(std::move(*abandoned.ticket)));
        BEAST_EXPECT(
            collector.beginAdmission(w, good, 2).result ==
            ExportSigCollectorV2::BeginResult::verify);
    }

    void
    run() override
    {
        testAdmissionAndConflict();
        testReservationAndPublicationLifecycle();
        testMalformedBoundaries();
    }
};

BEAST_DEFINE_TESTSUITE(ExportSigCollectorV2, app, ripple);

}  // namespace test
}  // namespace ripple
