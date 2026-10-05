//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <test/jtx.h>
#include <xrpld/app/consensus/ActiveValidatorView.h>
#include <xrpld/app/consensus/ConsensusExtensions.h>
#include <xrpld/app/consensus/RCLCxPeerPos.h>
#include <xrpld/app/ledger/InboundTransactions.h>
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/misc/CanonicalTXSet.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/consensus/ConsensusProposal.h>
#include <xrpld/ledger/OpenView.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/EntropyTier.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/digest.h>
#include <string>
#include <utility>
#include <vector>

namespace ripple {
namespace test {

namespace {

// Golden vectors for ConsensusEntropy: byte-exact outputs of the position
// envelope, proposal identity, sidecar snapshots, entropy selection, ordering
// salt and injected pseudo-transaction, all from fixed inputs. Every validator
// key comes from a fixed seed (secp256k1 signing is RFC 6979 deterministic),
// every time and label is fixed, and the feature set names its amendments.

FeatureBitset
goldenFeatures()
{
    return FeatureBitset{featureConsensusEntropy, featureNegativeUNL};
}

uint256
goldenHash(std::string const& label)
{
    return sha512Half(makeSlice(label));
}

std::pair<PublicKey, SecretKey>
goldenKey(std::size_t i)
{
    return generateKeyPair(
        KeyType::secp256k1, generateSeed("golden-" + std::to_string(i)));
}

// Standalone short-circuits selectEntropy to synthetic entropy; the golden
// vectors must exercise the real selector.
void
goldenForceNonStandalone(Application& app)
{
    const_cast<Config&>(app.config()).setupControl(true, true, false);
}

Buffer
goldenSignPosition(
    PublicKey const& publicKey,
    SecretKey const& secretKey,
    ExtendedPosition const& position,
    std::uint32_t proposeSeq,
    NetClock::time_point closeTime,
    uint256 const& prevLedger)
{
    using Proposal = ConsensusProposal<NodeID, uint256, ExtendedPosition>;
    Proposal proposal{
        prevLedger,
        proposeSeq,
        position,
        closeTime,
        NetClock::time_point{},
        calcNodeID(publicKey)};

    auto const sig = signDigest(publicKey, secretKey, proposal.signingHash());
    return Buffer(sig.data(), sig.size());
}

// Harvest a commit (proposeSeq 0) and its reveal (proposeSeq 1). Reveal
// verification resolves prevLedger through the LedgerMaster, so it must be a
// stored ledger whose successor is seq.
void
goldenHarvestCommitReveal(
    ConsensusExtensions& ce,
    std::pair<PublicKey, SecretKey> const& key,
    uint256 const& txSetHash,
    LedgerIndex seq,
    NetClock::time_point closeTime,
    uint256 const& prevLedger,
    uint256 const& reveal)
{
    auto const& [pk, sk] = key;
    auto const nodeId = calcNodeID(pk);

    ExtendedPosition commitPos{txSetHash};
    commitPos.myCommitment = sha512Half(reveal, pk, seq);
    auto const commitSig =
        goldenSignPosition(pk, sk, commitPos, 0, closeTime, prevLedger);
    ce.harvestRngData(
        nodeId,
        pk,
        commitPos,
        0,
        closeTime,
        prevLedger,
        Slice(commitSig.data(), commitSig.size()));

    ExtendedPosition revealPos{txSetHash};
    revealPos.myReveal = reveal;
    auto const revealSig =
        goldenSignPosition(pk, sk, revealPos, 1, closeTime, prevLedger);
    ce.harvestRngData(
        nodeId,
        pk,
        revealPos,
        1,
        closeTime,
        prevLedger,
        Slice(revealSig.data(), revealSig.size()));
}

// In-memory ledger carrying a UNLReport over activeKeys, with disabledKeys
// in the NegativeUNL. Not stored in the LedgerMaster: it only feeds the
// active validator view.
std::shared_ptr<Ledger>
goldenUNLReportLedger(
    jtx::Env& env,
    std::vector<PublicKey> const& activeKeys,
    std::vector<PublicKey> const& disabledKeys = {})
{
    auto const genesis = std::make_shared<Ledger>(
        create_genesis,
        env.app().config(),
        std::vector<uint256>{},
        env.app().getNodeFamily());
    auto ledger =
        std::make_shared<Ledger>(*genesis, env.app().timeKeeper().closeTime());

    auto report = std::make_shared<SLE>(keylet::UNLReport());
    std::vector<STObject> active;
    active.reserve(activeKeys.size());
    for (auto const& pk : activeKeys)
    {
        active.push_back(STObject::makeInnerObject(sfActiveValidator));
        active.back().setFieldVL(sfPublicKey, pk);
    }
    report->setFieldArray(
        sfActiveValidators, STArray(active, sfActiveValidators));

    OpenView accum(&*ledger);
    accum.rawInsert(report);

    if (!disabledKeys.empty())
    {
        auto negUnl = std::make_shared<SLE>(keylet::negativeUNL());
        std::vector<STObject> disabled;
        disabled.reserve(disabledKeys.size());
        for (auto const& pk : disabledKeys)
        {
            disabled.push_back(STObject::makeInnerObject(sfDisabledValidator));
            disabled.back().setFieldVL(sfPublicKey, pk);
            disabled.back().setFieldU32(sfFirstLedgerSequence, ledger->seq());
        }
        negUnl->setFieldArray(
            sfDisabledValidators, STArray(disabled, sfDisabledValidators));
        accum.rawInsert(negUnl);
    }

    accum.apply(*ledger);
    return ledger;
}

std::string
goldenPositionHex(ExtendedPosition const& position)
{
    Serializer s;
    position.add(s);
    return strHex(s.slice());
}

// Item keys of a sidecar snapshot, in leaf order.
std::string
goldenLeafKeys(Application& app, uint256 const& root)
{
    auto const map = app.getInboundTransactions().getSet(root, false);
    if (!map)
        return "missing";
    std::string out;
    map->visitLeaves([&](boost::intrusive_ptr<SHAMapItem const> const& item) {
        if (!out.empty())
            out += ",";
        out += to_string(item->key());
    });
    return out;
}

std::string
goldenSelection(ConsensusExtensions::EntropySelection const& selection)
{
    return to_string(selection.digest) + "/" + std::to_string(selection.tier) +
        "/" + std::to_string(selection.count) + "/" +
        std::to_string(selection.denominator) + "/" +
        strHex(selection.contributors);
}

std::shared_ptr<STTx const>
goldenSingleTx(CanonicalTXSet const& txs)
{
    if (std::distance(txs.begin(), txs.end()) != 1)
        return {};
    return txs.begin()->second;
}

}  // namespace

class ConsensusEntropyGolden_test : public beast::unit_test::suite
{
    // Every expected value was captured by running the code; a mismatch
    // reports the computed value.
    void
    pin(std::string const& name,
        std::string const& actual,
        std::string const& expected)
    {
        BEAST_EXPECTS(actual == expected, name + ": got " + actual);
    }

    void
    pin(std::string const& name,
        uint256 const& actual,
        std::string const& expected)
    {
        pin(name, to_string(actual), expected);
    }

    void
    pinInjected(
        std::string const& name,
        CanonicalTXSet const& txs,
        std::string const& expectedBytes,
        std::string const& expectedId)
    {
        auto const tx = goldenSingleTx(txs);
        BEAST_EXPECT(tx);
        if (!tx)
            return;
        BEAST_EXPECT(tx->getTxnType() == ttCONSENSUS_ENTROPY);
        Serializer s;
        tx->add(s);
        pin(name + " bytes", strHex(s.slice()), expectedBytes);
        pin(name + " id", tx->getTransactionID(), expectedId);
    }

    void
    testPositionAndProposalIdentity()
    {
        testcase("position envelope and proposal identity");

        ExtendedPosition legacy{goldenHash("golden-txset")};
        pin("legacy position",
            goldenPositionHex(legacy),
            "CEF7D985EF47DDFF1C2B5F62EE2E58A1FA0F33CD5E77DE90A34CE3EF4DF07C8B");

        ExtendedPosition observedOnly{goldenHash("golden-txset")};
        observedOnly.observedParticipantsHash = goldenHash("golden-observed");
        pin("0x40 position",
            goldenPositionHex(observedOnly),
            "CEF7D985EF47DDFF1C2B5F62EE2E58A1FA0F33CD5E77DE90A34CE3EF4DF07C8B40"
            "86314B08679F04E98EF631B60CA96EFFCD9C22CD273593CDB1D0D4DFFD8E33FE");

        ExtendedPosition full{goldenHash("golden-txset")};
        full.commitSetHash = goldenHash("golden-commit-set");
        full.entropySetHash = goldenHash("golden-entropy-set");
        full.myCommitment = goldenHash("golden-commitment");
        full.myReveal = goldenHash("golden-reveal");
        full.observedParticipantsHash = goldenHash("golden-observed");
        pin("0x4F position",
            goldenPositionHex(full),
            "CEF7D985EF47DDFF1C2B5F62EE2E58A1FA0F33CD5E77DE90A34CE3EF4DF07C8B4F"
            "A395C3965D1F3890F022620CCA80B5D39065A23C580B3E8828F764CBB6502E0485"
            "FA90FDC2FF7727E17FD8848E64AD0493F336F783997464EE8FE966A93AC83A95B8"
            "6A1A4D217317133B1C9B362C4517BE6E879084FAD1CD8582B0F185BF42D810DF96"
            "1D9D6F8782E5C0AF865E5A699D99AB4D25A32FD955DA41B2369421A79E86314B08"
            "679F04E98EF631B60CA96EFFCD9C22CD273593CDB1D0D4DFFD8E33FE");

        auto const [pk, sk] = goldenKey(0);
        auto const prevLedger = goldenHash("golden-prev-ledger");
        auto const closeTime = NetClock::time_point{NetClock::duration{1000}};
        std::uint32_t const proposeSeq = 3;

        using Proposal = ConsensusProposal<NodeID, uint256, ExtendedPosition>;
        Proposal const proposal{
            prevLedger,
            proposeSeq,
            full,
            closeTime,
            NetClock::time_point{},
            calcNodeID(pk)};
        pin("proposal signing hash",
            proposal.signingHash(),
            "2207A40257265482592D931FC677796ACD0F9B1051E39A084238CAFEE703847C");

        auto const sig =
            goldenSignPosition(pk, sk, full, proposeSeq, closeTime, prevLedger);
        pin("proposal unique id",
            proposalUniqueId(
                full,
                prevLedger,
                proposeSeq,
                closeTime,
                pk.slice(),
                Slice(sig.data(), sig.size())),
            "C591884A2ADB73F7A3685238E764420CC8F617DD9CF1CF31797D8A1A6CD17D55");
    }

    void
    testFullTierRound()
    {
        testcase("full participation: sidecars, selection, salt, injection");

        using namespace jtx;
        Env env{*this, envconfig(validator, ""), goldenFeatures(), nullptr};
        goldenForceNonStandalone(env.app());
        BEAST_EXPECT(!env.app().config().standalone());

        std::vector<std::pair<PublicKey, SecretKey>> keys;
        std::vector<PublicKey> activeKeys;
        for (std::size_t i = 0; i < 4; ++i)
        {
            keys.push_back(goldenKey(i));
            activeKeys.push_back(keys.back().first);
        }
        auto const viewLedger = goldenUNLReportLedger(env, activeKeys);
        auto const anchor = env.app().getLedgerMaster().getClosedLedger();
        pin("full anchor ledger",
            anchor->info().hash,
            "CCC3B3E88CCAC17F1BE6B4A648A55999411F19E3FE55EB721960EB0DF28EDDA5");
        pin("full view state",
            viewLedger->stateMap().getHash().as_uint256(),
            "A66AE6AF77B5E9E35D9F89CFF6111F3D63EA8B1D83AE6E71114602F9A377343A");

        auto const seq = anchor->info().seq + 1;
        auto const prevLedger = anchor->info().hash;
        auto const closeTime = NetClock::time_point{NetClock::duration{1000}};
        auto const txSetHash = goldenHash("golden-full-txset");

        ConsensusExtensions ce{env.app(), env.journal};
        ce.onRoundStart(RCLCxLedger{anchor}, {});
        ce.cacheUNLReport(viewLedger);
        ce.setRngEnabledThisRound(true);
        for (std::size_t i = 0; i < keys.size(); ++i)
            goldenHarvestCommitReveal(
                ce,
                keys[i],
                txSetHash,
                seq,
                closeTime,
                prevLedger,
                goldenHash("golden-full-reveal-" + std::to_string(i)));

        auto const commitRoot = ce.buildCommitSet(seq);
        auto const entropyRoot = ce.buildEntropySet(seq);
        pin("full commit root",
            commitRoot,
            "2E5355C52503CE8A7B9E72B949BEEE35B9482A082E28BFC9D97F4E2C187422B7");
        pin("full entropy root",
            entropyRoot,
            "C69882CCC4FFA3D3A076DD3415A7D3D8B74F9A7670D9737203939BA072C76281");
        pin("full commit leaves",
            goldenLeafKeys(env.app(), commitRoot),
            "045825CE54504284FD3F72BE1CB861D71EA8EFE54EEBD45A101924B60FEF3C42,"
            "A6A4D55E09EC74FC5E4A02A83B84930BA3F2E94A7A0BA6C12B61A0A6AE227A77,"
            "D250034BE274208ACE753642C2315A3C9A86C22AAEBAAE805D820B4A4018151A,"
            "D748F43DA7BED38446BF5C4BC86C8AFBF5A69FF3DF22A92916E16B7864EE03A1");
        pin("full reveal leaves",
            goldenLeafKeys(env.app(), entropyRoot),
            "2A2AE4F23C059C15868AD2E5C6115C644B8C48A3A2949ACBB7E1D736C541A25C,"
            "436861252114325A0AF0B11DE8EAC4D7CC77495766D88C8516949483B2DDB8C2,"
            "66C45C130C68043F493F1F6E3776156B90EC63CD5A22AFDF0B099752F2F9DC8B,"
            "98E83791727421B696ADBD1BE9603ABB31A5DDD720D600FB748AAB1FCDABC836");

        ce.acceptEntropySet(entropyRoot);
        auto const selection = ce.selectEntropy(txSetHash, seq);
        BEAST_EXPECT(selection.tier != entropyTierConsensusFallback);
        pin("full selection",
            goldenSelection(selection),
            "AD5F57F4E9C83C8AC20F119A1D5D17A7448C08D2B814C1DA709A46401A5C01F3/"
            "4/4/4/0F");
        pin("full ordering salt",
            ce.txnOrderingSalt(txSetHash, seq),
            "41467324A0CD9C4B97FF1F8331A6937311EFAB36AD92FF8E4EAC20C5D50AF2CF");

        CanonicalTXSet txs{goldenHash("golden-full-salt")};
        ce.onPreBuild(txs, seq, txSetHash);
        pinInjected(
            "full injected",
            txs,
            "1200691063000410640004240000000026000000035015AD5F57F4E9C83C8AC20F"
            "119A1D5D17A7448C08D2B814C1DA709A46401A5C01F36840000000000000007300"
            "7021010F8114000000000000000000000000000000000000000000101504",
            "654C06DF6A5A31CDBAE14F2441DFE19E2D1A7CBF17591574DBC52D7C765FB113");
    }

    void
    testViewAnchoringDecidesTier()
    {
        testcase(
            "UNLReport anchoring decides the tier of the same contribution");

        using namespace jtx;
        Env env{*this, envconfig(validator, ""), goldenFeatures(), nullptr};
        goldenForceNonStandalone(env.app());
        BEAST_EXPECT(!env.app().config().standalone());

        // A view built from local trusted config holds only this node's key,
        // so this node's own contribution is the one both views admit.
        auto const& valKeys = env.app().getValidatorKeys();
        BEAST_EXPECT(valKeys.keys);
        if (!valKeys.keys)
            return;
        std::pair<PublicKey, SecretKey> const local{
            valKeys.keys->publicKey, valKeys.keys->secretKey};
        BEAST_EXPECT(valKeys.nodeID == calcNodeID(local.first));

        auto const anchor = env.app().getLedgerMaster().getClosedLedger();
        auto const seq = anchor->info().seq + 1;
        auto const closeTime = NetClock::time_point{NetClock::duration{1003}};
        auto const txSetHash = goldenHash("golden-local-txset");

        auto runWith = [&](std::shared_ptr<Ledger const> const& viewLedger) {
            ConsensusExtensions ce{env.app(), env.journal};
            ce.onRoundStart(RCLCxLedger{anchor}, {});
            if (viewLedger)
                ce.cacheUNLReport(viewLedger);
            ce.setRngEnabledThisRound(true);
            // One key in either view: the fallback below can then come only
            // from the UNLReport gates, not from the participant threshold.
            auto const view = ce.activeValidatorView();
            BEAST_EXPECT(view->fromUNLReport == (viewLedger != nullptr));
            BEAST_EXPECT(view->size() == 1);
            BEAST_EXPECT(view->containsNode(valKeys.nodeID));
            goldenHarvestCommitReveal(
                ce,
                local,
                txSetHash,
                seq,
                closeTime,
                anchor->info().hash,
                goldenHash("golden-local-reveal"));
            auto const root = ce.buildEntropySet(seq);
            ce.acceptEntropySet(root);
            return std::make_pair(root, ce.selectEntropy(txSetHash, seq));
        };

        auto const anchored =
            runWith(goldenUNLReportLedger(env, {local.first}));
        BEAST_EXPECT(anchored.second.tier == entropyTierValidatorFull);
        pin("anchored local root",
            anchored.first,
            "91CD6652C4C4B9FEE0B31C8A78D7F5D193DEC1997C8288AEE7551B6EFCCC62B3");
        pin("anchored local selection",
            goldenSelection(anchored.second),
            "964DDFA830613F09A8A9525B9DC5CD8588674651BD7B62DF9B18935DF1CAB1C7/"
            "4/1/1/01");

        // The same accepted set under the trusted-config view mints only the
        // consensus fallback.
        auto const unanchored = runWith(nullptr);
        BEAST_EXPECT(unanchored.first == anchored.first);
        BEAST_EXPECT(unanchored.second.tier == entropyTierConsensusFallback);
        pin("unanchored local selection",
            goldenSelection(unanchored.second),
            "7CA6F3323698FB999F07EA269E837934F256527D5D5A7866AD165AB1E55A6BD9/"
            "1/0/0/");
    }

    void
    testValidatorQuorumRound()
    {
        testcase("validator quorum selection, salt and injection");

        using namespace jtx;
        Env env{*this, envconfig(validator, ""), goldenFeatures(), nullptr};
        goldenForceNonStandalone(env.app());
        BEAST_EXPECT(!env.app().config().standalone());

        // Six UNLReport validators: quorum 5, so 5 aligned contributors label
        // validator_quorum (all 6 would be validator_full).
        constexpr std::size_t kValidators = 6;
        constexpr std::size_t kRevealers = 5;
        std::vector<std::pair<PublicKey, SecretKey>> keys;
        std::vector<PublicKey> activeKeys;
        for (std::size_t i = 0; i < kValidators; ++i)
        {
            keys.push_back(goldenKey(200 + i));
            activeKeys.push_back(keys.back().first);
        }
        auto const viewLedger = goldenUNLReportLedger(env, activeKeys);
        auto const anchor = env.app().getLedgerMaster().getClosedLedger();
        pin("quorum view state",
            viewLedger->stateMap().getHash().as_uint256(),
            "6289DECAE4E639C1495ADDFE0400EABAD6C059B3D6BBC93BFFECCCD9BFBC216C");

        auto const seq = anchor->info().seq + 1;
        auto const prevLedger = anchor->info().hash;
        auto const closeTime = NetClock::time_point{NetClock::duration{1002}};
        auto const txSetHash = goldenHash("golden-quorum-txset");

        ConsensusExtensions ce{env.app(), env.journal};
        ce.onRoundStart(RCLCxLedger{anchor}, {});
        ce.cacheUNLReport(viewLedger);
        ce.setRngEnabledThisRound(true);
        for (std::size_t i = 0; i < kRevealers; ++i)
            goldenHarvestCommitReveal(
                ce,
                keys[i],
                txSetHash,
                seq,
                closeTime,
                prevLedger,
                goldenHash("golden-quorum-reveal-" + std::to_string(i)));

        auto const entropyRoot = ce.buildEntropySet(seq);
        pin("quorum entropy root",
            entropyRoot,
            "81560D89CAAE469BE29AA35C5FFE8B30B77A0F7B0AC93260A2220CDA94DFB5B4");

        ce.acceptEntropySet(entropyRoot);
        auto const selection = ce.selectEntropy(txSetHash, seq);
        BEAST_EXPECT(selection.tier == entropyTierValidatorQuorum);
        pin("quorum selection",
            goldenSelection(selection),
            "841077FFEA10779FFF44194E22AE4E190286618CB844A78EC3620DA428555A7A/"
            "3/5/6/2F");
        pin("quorum ordering salt",
            ce.txnOrderingSalt(txSetHash, seq),
            "B6CC07DB9CEA899FB4CE82947864E9384AC0E7B7737BC78C661E07773CE22E9F");

        CanonicalTXSet txs{goldenHash("golden-quorum-salt")};
        ce.onPreBuild(txs, seq, txSetHash);
        pinInjected(
            "quorum injected",
            txs,
            "1200691063000510640006240000000026000000035015841077FFEA10779FFF44"
            "194E22AE4E190286618CB844A78EC3620DA428555A7A6840000000000000007300"
            "7021012F8114000000000000000000000000000000000000000000101503",
            "9AD664173061ECF272F9BBC164B01A3611FFB6D5A7E8C6E1116B681DF6512542");
    }

    void
    testParticipantAlignedUnderNegativeUNL()
    {
        testcase("participant aligned under NegativeUNL");

        using namespace jtx;
        Env env{*this, envconfig(validator, ""), goldenFeatures(), nullptr};
        goldenForceNonStandalone(env.app());
        BEAST_EXPECT(!env.app().config().standalone());

        // 20 UNLReport validators, one disabled: the tier-2 floor anchors to
        // the original 20 (13) and the quorum to the effective 19 (16), so 15
        // aligned contributors label participant_aligned.
        constexpr std::size_t kOriginal = 20;
        constexpr std::size_t kDisabled = 1;
        constexpr std::size_t kRevealers = 15;
        std::vector<std::pair<PublicKey, SecretKey>> keys;
        std::vector<PublicKey> activeKeys;
        for (std::size_t i = 0; i < kOriginal; ++i)
        {
            keys.push_back(goldenKey(100 + i));
            activeKeys.push_back(keys.back().first);
        }
        std::vector<PublicKey> const disabledKeys(
            activeKeys.begin(), activeKeys.begin() + kDisabled);
        auto const viewLedger =
            goldenUNLReportLedger(env, activeKeys, disabledKeys);
        auto const anchor = env.app().getLedgerMaster().getClosedLedger();
        pin("aligned anchor ledger",
            anchor->info().hash,
            "CCC3B3E88CCAC17F1BE6B4A648A55999411F19E3FE55EB721960EB0DF28EDDA5");
        pin("aligned view state",
            viewLedger->stateMap().getHash().as_uint256(),
            "566A1DC0973A90ACD75C058E90303130800C522D6FEC7745BD60DB03B0B14699");

        auto const seq = anchor->info().seq + 1;
        auto const prevLedger = anchor->info().hash;
        auto const closeTime = NetClock::time_point{NetClock::duration{1001}};
        auto const txSetHash = goldenHash("golden-aligned-txset");

        ConsensusExtensions ce{env.app(), env.journal};
        ce.onRoundStart(RCLCxLedger{anchor}, {});
        ce.cacheUNLReport(viewLedger);
        ce.setRngEnabledThisRound(true);

        auto const view = ce.activeValidatorView();
        BEAST_EXPECT(view->originalViewSize == kOriginal);
        BEAST_EXPECT(view->size() == kOriginal - kDisabled);
        BEAST_EXPECT(view->nodeIds.size() == kOriginal - kDisabled);
        BEAST_EXPECT(!view->masterKeys.count(disabledKeys.front()));
        {
            Serializer s;
            for (auto const& key : view->orderedMasterKeys)
                s.addVL(key.slice());
            pin("aligned view order",
                sha512Half(s.slice()),
                "DE3FE53F7766824CF65A9B481692AC01318CE9A892F17FEE375FBD0CDB0C86"
                "2F");
        }

        for (std::size_t i = 0; i < kRevealers; ++i)
            goldenHarvestCommitReveal(
                ce,
                keys[kDisabled + i],
                txSetHash,
                seq,
                closeTime,
                prevLedger,
                goldenHash("golden-aligned-reveal-" + std::to_string(i)));

        auto const entropyRoot = ce.buildEntropySet(seq);
        pin("aligned entropy root",
            entropyRoot,
            "C3848F34A2D128CBC91703F390EA12745F6BDE6E7DE744534F8F5075D737D06B");

        ce.acceptEntropySet(entropyRoot);
        auto const selection = ce.selectEntropy(txSetHash, seq);
        BEAST_EXPECT(selection.tier == entropyTierParticipantAligned);
        pin("aligned selection",
            goldenSelection(selection),
            "3C186F87F8F7FFEA4C499D294615EABCECD9A8B8D04405640B87AACF51FB7163/"
            "2/15/19/7E7D07");
        pin("aligned ordering salt",
            ce.txnOrderingSalt(txSetHash, seq),
            "932EB55D85CB78A240DCFB32EEA267EDDC4A6189BD56F97E3445740E2D1D0E08");

        CanonicalTXSet txs{goldenHash("golden-aligned-salt")};
        ce.onPreBuild(txs, seq, txSetHash);
        pinInjected(
            "aligned injected",
            txs,
            "1200691063000F106400132400000000260000000350153C186F87F8F7FFEA4C49"
            "9D294615EABCECD9A8B8D04405640B87AACF51FB71636840000000000000007300"
            "7021037E7D078114000000000000000000000000000000000000000000101502",
            "B5607C4AB3FCC12251902A2557CB96B31F8E83EF971CC996AC437F78C29B3812");
    }

    void
    testFallback()
    {
        testcase("consensus fallback digest, salt and injection");

        using namespace jtx;
        Env env{*this, envconfig(validator, ""), goldenFeatures(), nullptr};
        goldenForceNonStandalone(env.app());
        BEAST_EXPECT(!env.app().config().standalone());

        std::vector<PublicKey> activeKeys;
        for (std::size_t i = 0; i < 4; ++i)
            activeKeys.push_back(goldenKey(i).first);
        auto const viewLedger = goldenUNLReportLedger(env, activeKeys);
        auto const anchor = env.app().getLedgerMaster().getClosedLedger();
        pin("fallback anchor ledger",
            anchor->info().hash,
            "CCC3B3E88CCAC17F1BE6B4A648A55999411F19E3FE55EB721960EB0DF28EDDA5");

        auto const seq = anchor->info().seq + 1;
        auto const txSetHash = goldenHash("golden-fallback-txset");

        ConsensusExtensions ce{env.app(), env.journal};
        ce.onRoundStart(RCLCxLedger{anchor}, {});
        ce.cacheUNLReport(viewLedger);
        ce.setRngEnabledThisRound(true);

        auto const selection = ce.selectEntropy(txSetHash, seq);
        BEAST_EXPECT(selection.tier == entropyTierConsensusFallback);
        BEAST_EXPECT(
            selection.digest ==
            sha512Half(
                HashPrefix::entropyFallback,
                anchor->info().hash,
                txSetHash,
                seq));
        pin("fallback selection",
            goldenSelection(selection),
            "559D97C5E924D42D4B83AA1926CCDD5B9CDB5ED6B7CA88F1CCF7E82B506531DA/"
            "1/0/0/");
        pin("fallback ordering salt",
            ce.txnOrderingSalt(txSetHash, seq),
            "2704C9C07B84E0AF0A7B00CF9CC46277B48A47DDE4C9DDE15F029BAA3C1D8A29");

        CanonicalTXSet txs{goldenHash("golden-fallback-salt")};
        ce.onPreBuild(txs, seq, txSetHash);
        pinInjected(
            "fallback injected",
            txs,
            "1200691063000010640000240000000026000000035015559D97C5E924D42D4B83"
            "AA1926CCDD5B9CDB5ED6B7CA88F1CCF7E82B506531DA6840000000000000007300"
            "7021008114000000000000000000000000000000000000000000101501",
            "D5DFFAE3095BBFED00AD759DB0FF1DB99EE81AF45B222B1ECD009F4A89F0F2FE");
    }

    void
    testObservedParticipants()
    {
        testcase("observed participants hash");

        using namespace jtx;
        Env env{*this, envconfig(validator, ""), goldenFeatures(), nullptr};

        std::vector<PublicKey> activeKeys;
        for (std::size_t i = 0; i < 4; ++i)
            activeKeys.push_back(goldenKey(i).first);
        auto const viewLedger = goldenUNLReportLedger(env, activeKeys);

        ConsensusExtensions ce{env.app(), env.journal};
        ce.cacheUNLReport(viewLedger);
        ce.setRngEnabledThisRound(true);
        // Two members and one outsider; observing mode leaves out the local
        // validator.
        ce.recordParticipantDiagnostics(
            ConsensusMode::observing,
            std::vector<NodeID>{
                calcNodeID(activeKeys[2]),
                calcNodeID(goldenKey(99).first),
                calcNodeID(activeKeys[0])});
        BEAST_EXPECT(ce.observedParticipantCount() == 2);
        auto const hash = ce.observedParticipantsHash();
        BEAST_EXPECT(hash);
        if (!hash)
            return;
        pin("observed participants hash",
            *hash,
            "90383B5A65CF8347FF05B4EC22E911B2744666369B7106D127D66C9F7C0BE15D");
        pin("observed participants bitmap",
            strHex(ce.observedParticipantsBitmapBin()),
            "31303130");

        ExtendedPosition position{goldenHash("golden-observed-txset")};
        ce.attachParticipantDiagnostics(position);
        pin("observed position",
            goldenPositionHex(position),
            "68CBC16ADB838EB9AB07B257ED4D6296F0BF795D515C608FE51566C3EA2F0DEA40"
            "90383B5A65CF8347FF05B4EC22E911B2744666369B7106D127D66C9F7C0BE15D");
    }

public:
    void
    run() override
    {
        testPositionAndProposalIdentity();
        testFullTierRound();
        testViewAnchoringDecidesTier();
        testValidatorQuorumRound();
        testParticipantAlignedUnderNegativeUNL();
        testFallback();
        testObservedParticipants();
    }
};

BEAST_DEFINE_TESTSUITE(ConsensusEntropyGolden, consensus, ripple);

}  // namespace test
}  // namespace ripple
