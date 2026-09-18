#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/SteppingReplay.h>
#include <test/jtx/pay.h>

#include <xrpld/app/consensus/ConsensusExtensions.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/ExportCommittee.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/TxFlags.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ripple::test {

class SteppingExtensions_test : public beast::unit_test::suite
{
    static constexpr std::uint32_t networkID = 21337;
    static constexpr std::uint32_t observer = 3;
    static constexpr std::uint32_t warmLedger = 257;

    struct Observations
    {
        std::array<std::size_t, 4> secrets{};
        std::array<std::size_t, 4> directFrames{};
        std::array<std::size_t, 4> proposalFrames{};
    };

    struct World
    {
        SteppingNetwork& net;
        bool rng;
        bool exportEnabled;
        std::shared_ptr<Observations> observed = std::make_shared<Observations>();
        jtx::Account owner{"dsf-export-owner"};
        jtx::Account destination{"dsf-export-destination"};

        World(SteppingNetwork& network, bool rngOn, bool exportOn)
            : net(network), rng(rngOn), exportEnabled(exportOn)
        {
            net.configureNodes([stats = observed, rngOn, exportOn](std::uint32_t id, Config& cfg) {
                cfg.NETWORK_ID = networkID;
                // Let the real flag-ledger vote produce the common active view.
                cfg.features.insert(featureNegativeUNL);
                cfg.features.insert(featureXahauGenesis);
                if (rngOn)
                    cfg.features.insert(featureConsensusEntropy);
                if (exportOn)
                    cfg.features.insert(featureExport);
                cfg.harnessEntropySecret = [stats, id](uint256 const& parent, std::uint32_t seq) {
                    ++stats->secrets.at(id);
                    return sha512Half(std::string{"DSF entropy fixture"}, id, parent, seq);
                };
                cfg.harnessPeerMessage =
                    [stats, id](std::uint16_t type, std::string const&, std::uint32_t,
                                beast::IP::Endpoint const&, ::google::protobuf::Message const& msg) {
                        if (type == protocol::mtEXPORT_SHARES)
                            ++stats->directFrames.at(id);
                        if (type == protocol::mtPROPOSE_LEDGER &&
                            static_cast<protocol::TMProposeSet const&>(msg).exportsignatures_size())
                            ++stats->proposalFrames.at(id);
                    };
            });
            net.validators(3);
            net.observer();
            net.mesh();
        }

        std::shared_ptr<Transaction>
        submit(std::uint32_t node, Json::Value tx, jtx::Account const& signer)
        {
            tx[jss::NetworkID] = networkID;
            return net.submit(node, std::move(tx), signer);
        }

        Json::Value
        intent(std::uint32_t ticket = 1)
        {
            std::vector<PublicKey> keys;
            for (std::uint32_t i = 0; i < observer; ++i)
                keys.push_back(net.node(i).app().getValidatorKeys().keys->masterPublicKey);
            auto const roster = canonicalizeExportCommittee(makeSlice(serializeExportCommittee(keys)));
            if (!roster)
                throw std::logic_error("Invalid fixture committee");

            STObject inner(sfExportedTxn);
            inner.setFieldU16(sfTransactionType, ttPAYMENT);
            inner.setFieldU32(sfFlags, tfFullyCanonicalSig);
            inner.setFieldU32(sfSequence, 0);
            inner.setFieldU32(sfTicketSequence, ticket);
            inner.setFieldU32(sfLastLedgerSequence, 1'000'000);
            inner.setFieldAmount(sfAmount, XRPAmount{1'000'000});
            inner.setFieldAmount(sfFee, XRPAmount{1000});
            inner.setFieldVL(sfSigningPubKey, Blob{});
            inner.setAccountID(sfAccount, owner.id());
            inner.setAccountID(sfDestination, destination.id());

            Json::Value tx;
            tx[jss::TransactionType] = jss::Export;
            tx[jss::Account] = owner.human();
            tx[jss::Fee] = "1000000";
            tx[jss::LastLedgerSequence] =
                net.node(observer).app().openLedger().current()->seq() +
                ExportLimits::maxAdmissionWindowLedgers;
            tx[sfExportedTxn.jsonName] = inner.getJson(JsonOptions::none);
            tx[sfExportCommittee.jsonName] = strHex(*roster);
            tx[sfExportCommitteeHash.jsonName] = to_string(exportCommitteeHash(makeSlice(*roster)));
            return tx;
        }
    };

    bool
    ready(World& world)
    {
        auto& net = world.net;
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return false;
        net.runTo(warmLedger, SteppingNetwork::RunBudget{10'000, 2'000'000});
        if (!BEAST_EXPECT(net.minValidatedSeq() >= warmLedger))
        {
            log << net.jobDiagnostics() << std::endl;
            return false;
        }
        for (std::uint32_t i = 0; i <= observer; ++i)
        {
            auto const ledger = net.ledger(i, warmLedger);
            if (!BEAST_EXPECT(ledger != nullptr))
                return false;
            BEAST_EXPECT(ledger->rules().enabled(featureConsensusEntropy) == world.rng);
            BEAST_EXPECT(ledger->rules().enabled(featureExport) == world.exportEnabled);
            auto const report = ledger->read(keylet::UNLReport());
            if (!BEAST_EXPECT(report && report->getFieldArray(sfActiveValidators).size() == observer))
                return false;
        }
        BEAST_EXPECT(net.ledgersAgree(warmLedger));
        BEAST_EXPECT(!net.node(observer).app().getValidatorKeys().keys);
        BEAST_EXPECT(world.observed->secrets[observer] == 0);
        return true;
    }

    std::uint32_t
    witnessAt(SteppingNetwork& net, uint256 const& origin, std::uint32_t from)
    {
        for (auto seq = from; seq <= net.validSeq(observer); ++seq)
            if (auto const ledger = net.ledger(observer, seq))
                for (auto const& [tx, meta] : ledger->txs)
                    if (tx->getTxnType() == ttEXPORT_SIGNATURES &&
                        tx->getFieldH256(sfTransactionHash) == origin)
                        return seq;
        return 0;
    }

    std::optional<std::vector<uint256>>
    baseline(SteppingNetwork& net, bool rng, bool exportOn)
    {
        World world(net, rng, exportOn);
        if (!ready(world))
            return std::nullopt;

        auto const funding = world.submit(
            observer, jtx::pay(jtx::Account::master, world.owner, jtx::XRP(10'000)),
            jtx::Account::master);
        if (!BEAST_EXPECT(funding->getResult() == tesSUCCESS))
            return std::nullopt;
        net.runTo(warmLedger + 2);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= warmLedger + 2))
            return std::nullopt;
        auto const payment = world.submit(
            observer, jtx::pay(world.owner, world.destination, jtx::XRP(1000)), world.owner);
        if (!BEAST_EXPECT(payment->getResult() == tesSUCCESS))
            return std::nullopt;

        std::optional<uint256> origin;
        if (exportOn)
        {
            auto const tx = world.submit(observer, world.intent(), world.owner);
            if (!BEAST_EXPECT(tx->getResult() == tesSUCCESS))
                return std::nullopt;
            origin = tx->getID();
        }
        auto const target = warmLedger + 8;
        net.runTo(target);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
            return std::nullopt;
        BEAST_EXPECT(net.ledgersAgree(target));
        BEAST_EXPECT(net.validatedForkFree());
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);
        BEAST_EXPECT(world.observed->secrets[observer] == 0);
        for (std::uint32_t i = 0; i < observer; ++i)
            BEAST_EXPECT((world.observed->secrets[i] != 0) == rng);

        bool entropySeen = false;
        bool paymentSeen = false;
        bool witnessSeen = false;
        std::vector<uint256> outcome;
        for (auto seq = warmLedger; seq <= target; ++seq)
        {
            auto const ledger = net.ledger(observer, seq);
            if (!BEAST_EXPECT(ledger != nullptr))
                return std::nullopt;
            for (std::uint32_t i = 0; i < observer; ++i)
                BEAST_EXPECT(net.ledgerHash(i, seq) == ledger->info().hash);
            for (auto const& [tx, meta] : ledger->txs)
            {
                BEAST_EXPECT(meta != nullptr);
                entropySeen |= tx->getTxnType() == ttCONSENSUS_ENTROPY;
                paymentSeen |= tx->getTransactionID() == payment->getID();
                witnessSeen |= tx->getTxnType() == ttEXPORT_SIGNATURES;
                if (tx->getTxnType() == ttCONSENSUS_ENTROPY && seq > warmLedger)
                    BEAST_EXPECT(tx->getFieldU16(sfEntropyCount) == observer);
            }
            outcome.push_back(ledger->info().hash);
        }
        BEAST_EXPECT(entropySeen == rng);
        BEAST_EXPECT(paymentSeen);
        BEAST_EXPECT(witnessSeen == exportOn);
        if (origin)
        {
            BEAST_EXPECT(witnessAt(net, *origin, warmLedger) != 0);
            BEAST_EXPECT(world.observed->directFrames[observer] != 0);
            BEAST_EXPECT(world.observed->proposalFrames[observer] != 0);
        }
        else
        {
            BEAST_EXPECT(world.observed->directFrames[observer] == 0);
            BEAST_EXPECT(world.observed->proposalFrames[observer] == 0);
        }
        return outcome;
    }

public:
    void
    run() override
    {
        for (auto const rng : {false, true})
            for (auto const exportOn : {false, true})
            {
                auto const label = std::string{"feature matrix rng="} + (rng ? "on" : "off") +
                    " export=" + (exportOn ? "on" : "off");
                testcase(label);
                expectReplays(*this, label.c_str(), [this, rng, exportOn](SteppingNetwork& net) {
                    return baseline(net, rng, exportOn);
                });
            }
    }
};

BEAST_DEFINE_TESTSUITE(SteppingExtensions, consensus, ripple);

}  // namespace ripple::test
