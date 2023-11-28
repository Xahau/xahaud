//-----------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2020 Ripple Labs Inc.

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

#include <ripple/app/consensus/RCLValidations.h>
#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/app/misc/NegativeUNLVote.h>
#include <ripple/app/misc/ValidatorList.h>
#include <ripple/app/tx/apply.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/StringUtilities.h>
#include <ripple/beast/unit_test.h>
#include <ripple/core/ConfigSections.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <string>
#include <test/csf.h>
#include <test/jtx.h>
#include <vector>

namespace ripple {
namespace test {

// /*
//  * This file implements the following negative UNL related tests:
//  * -- test filling and applying ttUNL_MODIFY Tx and ledger update
//  * -- test ttUNL_MODIFY Tx failure without featureNegativeUNL amendment
//  * -- test the NegativeUNLVote class. The test cases are split to multiple
//  *    test classes to allow parallel execution.
//  * -- test the negativeUNLFilter function
//  *
//  * Other negative UNL related tests such as ValidatorList and RPC related
//  ones
//  * are put in their existing unit test files.
//  */

// /**
//  * Try to apply a ttUNL_MODIFY Tx, and test the apply result
//  *
//  * @param env the test environment
//  * @param view the OpenView of the ledger
//  * @param tx the ttUNL_MODIFY Tx
//  * @param pass if the Tx should be applied successfully
//  * @return true if meet the expectation of apply result
//  */
bool
applyAndTestUNLRResult(
    jtx::Env& env,
    OpenView& view,
    STTx const& tx,
    bool pass);

/**
 * Verify the content of UNL Report entries (public key and ledger sequence)
 * of a ledger
 *
 * @param l the ledger
 * @param nUnlLedgerSeq the expected PublicKeys and ledger Sequences
 * @note nUnlLedgerSeq is copied so that it can be modified.
 * @return true if meet the expectation
 */
bool
VerifyUNLRPubKeyAndSeq(
    std::shared_ptr<Ledger const> const& l,
    hash_map<PublicKey, std::uint32_t> nUnlLedgerSeq);

/**
 * Count the number of Tx in a TxSet
 *
 * @param txSet the TxSet
 * @return the number of Tx
 */
std::size_t
countUNLRTx(std::shared_ptr<SHAMap> const& txSet);

std::vector<std::string> const keys = {
    "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1"};

// std::unique_ptr<Config>
// makeNetworkVLConfig(uint32_t networkID, std::vector<std::string> keys);

/**
 * Verify if the UNL report exists
 *
 * @param env the environment
 * @return true if it exists false if it does not
 */
bool
hasUNLReport(jtx::Env const& env);

/**
 * Verify if the Import VL Key exists
 *
 * @param env the environment
 * @param pk the import public key
 * @return true if it exists false if it does not
 */
bool
isImportVL(jtx::Env const& env, PublicKey const& pk);

/**
 * Verify if the Validator exists
 *
 * @param env the environment
 * @param pk the public key of the validator
 * @return true if it exists false if it does not
 */
bool
isActiveValidator(jtx::Env const& env, PublicKey const& pk);

/**
 * Create fake public keys
 *
 * @param n the number of public keys
 * @return a vector of public keys created
 */
std::vector<PublicKey>
createUNLRPublicKeys(std::size_t n);

/**
 * Create ttUNL_REPORT Tx
 *
 * @param seq current ledger seq
 * @param importKey the import public key of the validator
 * @param valKey the public key of the validator
 * @return the ttUNL_REPORT Tx
 */
STTx
createUNLRTx(
    LedgerIndex seq,
    PublicKey const& importKey,
    PublicKey const& valKey);

class UNLReport_test : public beast::unit_test::suite
{
    // Import VL Keys
    std::vector<std::string> const _ivlKeys = {
        "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
        "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC2",
    };

    std::vector<PublicKey> ivlKeys;

    // VL Keys
    std::vector<std::string> const _vlKeys = {
        "0388935426E0D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97C052",
        "02691AC5AE1C4C333AE5DF8A93BDC495F0EEBFC6DB0DA7EB6EF808F3AFC006E3FE",
        "028949021029D5CC87E78BCF053AFEC0CAFD15108EC119EAAFEC466F5C095407BF",
        "027BAEF0CB02EA8B95F50DF4BC16C740B17B50C85F3757AA06A5DB6ADE0ED92106",
        "0318E0D644F3D2911D7B7E1B0B17684E7E625A6C36AECCE851BD16A4AD628B2136"};
    std::vector<PublicKey> vlKeys;

    void
    testNoImportVL(FeatureBitset features)
    {
        testcase("Test no import vl");
        using namespace jtx;

        std::vector<PublicKey> ivlKeys;
        for (auto const& strPk : _ivlKeys)
        {
            auto pkHex = strUnHex(strPk);
            ivlKeys.emplace_back(makeSlice(*pkHex));
        }

        std::vector<PublicKey> vlKeys;
        for (auto const& strPk : _vlKeys)
        {
            auto pkHex = strUnHex(strPk);
            vlKeys.emplace_back(makeSlice(*pkHex));
        }

        // Create UNLReport
        {
            Env env{*this, envconfig(), features, nullptr};

            auto l = std::make_shared<Ledger>(
                create_genesis,
                env.app().config(),
                std::vector<uint256>{},
                env.app().getNodeFamily());

            l = std::make_shared<Ledger>(
                *l, env.app().timeKeeper().closeTime());

            // insert a ttUNL_REPORT pseudo into the open ledger
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) -> bool {
                    STTx tx = createUNLRTx(l->seq(), ivlKeys[0], vlKeys[0]);
                    uint256 txID = tx.getTransactionID();
                    auto s = std::make_shared<ripple::Serializer>();
                    tx.add(*s);
                    env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                    view.rawTxInsert(txID, std::move(s), nullptr);
                    return true;
                });

            // close the ledger
            env.close();

            BEAST_EXPECT(isImportVL(env, ivlKeys[0]) == true);
            BEAST_EXPECT(isActiveValidator(env, vlKeys[0]) == true);
        }

        // Update UNLReport
        {
            Env env{*this, envconfig(), features, nullptr};

            auto l = std::make_shared<Ledger>(
                create_genesis,
                env.app().config(),
                std::vector<uint256>{},
                env.app().getNodeFamily());

            l = std::make_shared<Ledger>(
                *l, env.app().timeKeeper().closeTime());

            // insert a ttUNL_REPORT pseudo into the open ledger
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) -> bool {
                    STTx tx = createUNLRTx(l->seq(), ivlKeys[0], vlKeys[0]);
                    uint256 txID = tx.getTransactionID();
                    auto s = std::make_shared<ripple::Serializer>();
                    tx.add(*s);
                    env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                    view.rawTxInsert(txID, std::move(s), nullptr);
                    return true;
                });

            // close the ledger
            env.close();

            BEAST_EXPECT(isImportVL(env, ivlKeys[0]) == true);
            BEAST_EXPECT(isActiveValidator(env, vlKeys[0]) == true);

            l = std::make_shared<Ledger>(
                *l, env.app().timeKeeper().closeTime());

            // insert a ttUNL_REPORT pseudo into the open ledger
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) -> bool {
                    STTx tx = createUNLRTx(l->seq(), ivlKeys[0], vlKeys[1]);
                    uint256 txID = tx.getTransactionID();
                    auto s = std::make_shared<ripple::Serializer>();
                    tx.add(*s);
                    env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                    view.rawTxInsert(txID, std::move(s), nullptr);
                    return true;
                });

            // close the ledger
            env.close();

            BEAST_EXPECT(isImportVL(env, ivlKeys[0]) == true);
            BEAST_EXPECT(isActiveValidator(env, vlKeys[1]) == true);
        }
    }

    void
    testImportVL(FeatureBitset features)
    {
        testcase("Test import vl");
        using namespace jtx;

        std::vector<PublicKey> ivlKeys;
        for (auto const& strPk : _ivlKeys)
        {
            auto pkHex = strUnHex(strPk);
            ivlKeys.emplace_back(makeSlice(*pkHex));
        }

        std::vector<PublicKey> vlKeys;
        for (auto const& strPk : _vlKeys)
        {
            auto pkHex = strUnHex(strPk);
            vlKeys.emplace_back(makeSlice(*pkHex));
        }

        // telIMPORT_VL_KEY_NOT_RECOGNISED
        {
            test::jtx::Env env{
                *this,
                jtx::network::makeNetworkVLConfig(21337, keys),
                features,
                nullptr};

            auto l = std::make_shared<Ledger>(
                create_genesis,
                env.app().config(),
                std::vector<uint256>{},
                env.app().getNodeFamily());

            l = std::make_shared<Ledger>(
                *l, env.app().timeKeeper().closeTime());

            // insert a ttUNL_REPORT pseudo into the open ledger
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) -> bool {
                    STTx tx = createUNLRTx(l->seq(), ivlKeys[1], vlKeys[0]);
                    uint256 txID = tx.getTransactionID();
                    auto s = std::make_shared<ripple::Serializer>();
                    tx.add(*s);
                    env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                    view.rawTxInsert(txID, std::move(s), nullptr);
                    return true;
                });

            BEAST_EXPECT(hasUNLReport(env) == false);
        }

        // SUCCESS
        {
            test::jtx::Env env{
                *this,
                jtx::network::makeNetworkVLConfig(21337, keys),
                features,
                nullptr};

            auto l = std::make_shared<Ledger>(
                create_genesis,
                env.app().config(),
                std::vector<uint256>{},
                env.app().getNodeFamily());

            l = std::make_shared<Ledger>(
                *l, env.app().timeKeeper().closeTime());

            // insert a ttUNL_REPORT pseudo into the open ledger
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) -> bool {
                    STTx tx = createUNLRTx(l->seq(), ivlKeys[0], vlKeys[0]);
                    uint256 txID = tx.getTransactionID();
                    auto s = std::make_shared<ripple::Serializer>();
                    tx.add(*s);
                    env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                    view.rawTxInsert(txID, std::move(s), nullptr);
                    return true;
                });

            // close the ledger
            env.close();

            BEAST_EXPECT(isImportVL(env, ivlKeys[0]) == true);
            BEAST_EXPECT(isImportVL(env, ivlKeys[1]) == false);
            BEAST_EXPECT(isActiveValidator(env, vlKeys[0]) == true);
        }
    }

    void
    testValidMulitple(FeatureBitset features)
    {
        testcase("Test several validators all get on the list");
        using namespace jtx;

        test::jtx::Env env{
            *this,
            jtx::network::makeNetworkVLConfig(21337, keys),
            features,
            nullptr};

        std::vector<PublicKey> ivlKeys;
        for (auto const& strPk : _ivlKeys)
        {
            auto pkHex = strUnHex(strPk);
            ivlKeys.emplace_back(makeSlice(*pkHex));
        }

        std::vector<PublicKey> vlKeys;
        for (auto const& strPk : _vlKeys)
        {
            auto pkHex = strUnHex(strPk);
            vlKeys.emplace_back(makeSlice(*pkHex));
        }

        auto l = std::make_shared<Ledger>(
            create_genesis,
            env.app().config(),
            std::vector<uint256>{},
            env.app().getNodeFamily());

        l = std::make_shared<Ledger>(*l, env.app().timeKeeper().closeTime());

        for (auto const& vlKey : vlKeys)
        {
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) -> bool {
                    STTx tx = createUNLRTx(
                        env.current()->seq() + 1, ivlKeys[0], vlKey);
                    uint256 txID = tx.getTransactionID();
                    auto s = std::make_shared<ripple::Serializer>();
                    tx.add(*s);
                    env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                    view.rawTxInsert(txID, std::move(s), nullptr);
                    return true;
                });
        }

        // close the ledger
        env.close();

        BEAST_EXPECT(isImportVL(env, ivlKeys[0]) == true);
        BEAST_EXPECT(isImportVL(env, ivlKeys[1]) == false);
        BEAST_EXPECT(isActiveValidator(env, vlKeys[0]) == true);
        BEAST_EXPECT(isActiveValidator(env, vlKeys[1]) == true);
        BEAST_EXPECT(isActiveValidator(env, vlKeys[2]) == true);
        BEAST_EXPECT(isActiveValidator(env, vlKeys[3]) == true);
        BEAST_EXPECT(isActiveValidator(env, vlKeys[4]) == true);
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testNoImportVL(features);
        testImportVL(features);
        testValidMulitple(features);
    }

    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeats(sa);
    }
};

class UNLReportNoAmendment_test : public beast::unit_test::suite
{
    void
    testUNLReportNoAmendment()
    {
        testcase("No UNL report amendment");

        jtx::Env env(*this, jtx::supported_amendments() - featureXahauGenesis);
        std::vector<PublicKey> publicKeys = createUNLRPublicKeys(1);
        // genesis ledger
        auto l = std::make_shared<Ledger>(
            create_genesis,
            env.app().config(),
            std::vector<uint256>{},
            env.app().getNodeFamily());
        BEAST_EXPECT(!l->rules().enabled(featureXahauGenesis));

        // generate more ledgers
        for (auto i = 0; i < 256 - 1; ++i)
        {
            l = std::make_shared<Ledger>(
                *l, env.app().timeKeeper().closeTime());
        }
        BEAST_EXPECT(l->seq() == 256);
        STTx tx = createUNLRTx(l->seq(), publicKeys[0], publicKeys[0]);
        OpenView accum(&*l);
        BEAST_EXPECT(applyAndTestUNLRResult(env, accum, tx, false));
        accum.apply(*l);
        BEAST_EXPECT(!hasUNLReport(env));
    }

    void
    run() override
    {
        testUNLReportNoAmendment();
    }
};

class UNLReportFork_test : public beast::unit_test::suite
{
    // Import VL Keys
    std::vector<std::string> const _ivlKeys = {
        "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
        "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC2",
    };

    std::vector<PublicKey> ivlKeys;

    // VL Keys
    std::vector<std::string> const _vlKeys = {
        "0388935426E0D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97C052",
        "02691AC5AE1C4C333AE5DF8A93BDC495F0EEBFC6DB0DA7EB6EF808F3AFC006E3FE",
        "028949021029D5CC87E78BCF053AFEC0CAFD15108EC119EAAFEC466F5C095407BF",
        "027BAEF0CB02EA8B95F50DF4BC16C740B17B50C85F3757AA06A5DB6ADE0ED92106",
        "0318E0D644F3D2911D7B7E1B0B17684E7E625A6C36AECCE851BD16A4AD628B2136"};
    std::vector<PublicKey> vlKeys;

    void
    testVLImportByzantine()
    {
        using namespace jtx;
        using namespace csf;
        using namespace std::chrono;

        std::vector<std::pair<std::string, PublicKey>> ivlKeys;
        for (auto const& strPk : _ivlKeys)
        {
            auto pkHex = strUnHex(strPk);
            ivlKeys.emplace_back(strPk, makeSlice(*pkHex));
        }

        std::vector<PublicKey> vlKeys;
        for (auto const& strPk : _vlKeys)
        {
            auto pkHex = strUnHex(strPk);
            vlKeys.emplace_back(makeSlice(*pkHex));
        }

        Sim sim;
        ConsensusParms const parms{};

        SimDuration const delay =
            round<milliseconds>(0.2 * parms.ledgerGRANULARITY);
        PeerGroup a = sim.createGroup(1);
        PeerGroup b = sim.createGroup(1);
        PeerGroup c = sim.createGroup(1);
        PeerGroup d = sim.createGroup(1);
        PeerGroup e = sim.createGroup(1);

        a.trustAndConnect(a + b + c + d + e, delay);
        b.trustAndConnect(a + b + c + d + e, delay);
        c.trustAndConnect(a + b + c + d + e, delay);
        d.trustAndConnect(a + b + c + d + e, delay);
        e.trustAndConnect(a + b + c + d + e, delay);

        PeerGroup network = a + b + c + d + e;
#if 0
        StreamCollector sc{std::cout};

        sim.collectors.add(sc);
#endif

        // set prior state
        sim.run(255);

        PeerGroup wrongImportVLNodes = d + e;

        // RH TODO: replace this simulation with a better one where we use
        // actual UNLReport txns
        /*
        auto txns =
            NegativeUNLVote::generateImportVLVoteTx(
            std::map<std::string, PublicKey>
            {
                ivlKeys[wrongImportVLNodes.contains(peer) ? 1 : 0]
            }, seq);
        */

        // 2 of the 3 vote for one and the other 3 for the other
        for (Peer* peer : network)
        {
            uint32_t seq = uint32_t(peer->lastClosedLedger.seq()) + 1;
            peer->txInjections.emplace(
                seq, Tx(wrongImportVLNodes.contains(peer) ? 1 : 0));
        }

        sim.run(4);

        // all 5 vote for one but 3 of them also vote for the other
        for (Peer* peer : network)
        {
            uint32_t seq = uint32_t(peer->lastClosedLedger.seq()) + 1;

            if (!wrongImportVLNodes.contains(peer))
                peer->txInjections.emplace(seq, Tx(2));
            peer->txInjections.emplace(seq, Tx(3));
        }

        // RH TODO: check that ledgers closed with the correct number of txns
        /*
        sim.run(1);
        for (Peer* peer : network)
        {
            peer->lastClosedLedger
        }
        */

        sim.run(4);

        std::cout << "Branches: " << sim.branches() << "\n";
        std::cout << "Fully synchronized: " << std::boolalpha
                  << sim.synchronized() << "\n";

        BEAST_EXPECT(sim.synchronized());
        BEAST_EXPECT(sim.branches() == 1);

        pass();
    }

    void
    run() override
    {
        testVLImportByzantine();
    }
};

/**
 * Utility class for creating validators and ledger history
 */
struct URNetworkHistory
{
    using LedgerHistory = std::vector<std::shared_ptr<Ledger>>;
    /**
     *
     * Only reasonable parameters can be honored,
     * e.g cannot hasToReEnable when nUNLSize == 0
     */
    struct Parameter
    {
        std::uint32_t numNodes;    // number of validators
        std::uint32_t negUNLSize;  // size of negative UNL in the last ledger
        bool hasToDisable;         // if has ToDisable in the last ledger
        bool hasToReEnable;        // if has ToReEnable in the last ledger
        bool withVL = false;       // test with VL Import
        /**
         * if not specified, the number of ledgers in the history is calculated
         * from negUNLSize, hasToDisable, and hasToReEnable
         */
        std::optional<int> numLedgers;
    };

    URNetworkHistory(beast::unit_test::suite& suite, Parameter const& p)
        : env(suite,
              p.withVL ? jtx::network::makeNetworkVLConfig(21337, keys)
                       : jtx::envconfig(),
              jtx::supported_amendments() | featureNegativeUNL)
        , param(p)
        , validations(env.app().getValidations())
    {
        createNodes();
        if (!param.numLedgers)
            param.numLedgers = 256 * (param.negUNLSize + 1);
        goodHistory = createLedgerHistory();
    }

    void
    createNodes()
    {
        assert(param.numNodes <= 256);
        UNLKeys = createUNLRPublicKeys(param.numNodes);
        for (int i = 0; i < param.numNodes; ++i)
        {
            UNLKeySet.insert(UNLKeys[i]);
            UNLNodeIDs.push_back(calcNodeID(UNLKeys[i]));
            UNLNodeIDSet.insert(UNLNodeIDs.back());
        }
    }

    /**
     * create ledger history and apply needed ttUNL_MODIFY tx at flag ledgers
     * @return
     */
    bool
    createLedgerHistory()
    {
        static uint256 fake_amemdment;  // So we have different genesis ledgers
        auto l = std::make_shared<Ledger>(
            create_genesis,
            env.app().config(),
            std::vector<uint256>{fake_amemdment++},
            env.app().getNodeFamily());
        history.push_back(l);

        // When putting validators into the negative UNL, we start with
        // validator 0, then validator 1 ...
        int nidx = 0;
        while (l->seq() <= param.numLedgers)
        {
            l = std::make_shared<Ledger>(
                *l, env.app().timeKeeper().closeTime());
            history.push_back(l);

            if (l->isFlagLedger())
            {
                l->updateNegativeUNL();
                OpenView accum(&*l);
                if (l->negativeUNL().size() < param.negUNLSize)
                {
                    auto tx = unl::createTx(true, l->seq(), UNLKeys[nidx]);
                    if (!unl::applyAndTestResult(env, accum, tx, true))
                        break;
                    ++nidx;
                }
                else if (l->negativeUNL().size() == param.negUNLSize)
                {
                    if (param.hasToDisable)
                    {
                        auto tx = unl::createTx(true, l->seq(), UNLKeys[nidx]);
                        if (!unl::applyAndTestResult(env, accum, tx, true))
                            break;
                        ++nidx;
                    }
                    if (param.hasToReEnable)
                    {
                        auto tx = unl::createTx(false, l->seq(), UNLKeys[0]);
                        if (!unl::applyAndTestResult(env, accum, tx, true))
                            break;
                    }
                }
                accum.apply(*l);
            }
            l->updateSkipList();
        }
        return unl::negUnlSizeTest(
            l, param.negUNLSize, param.hasToDisable, param.hasToReEnable);
    }

    /**
     * Create a validation
     * @param ledger the ledger the validation validates
     * @param v the validator
     * @return the validation
     */
    std::shared_ptr<STValidation>
    createSTVal(std::shared_ptr<Ledger const> const& ledger, NodeID const& v)
    {
        static auto keyPair = randomKeyPair(KeyType::secp256k1);
        return std::make_shared<STValidation>(
            env.app().timeKeeper().now(),
            keyPair.first,
            keyPair.second,
            v,
            [&](STValidation& v) {
                v.setFieldH256(sfLedgerHash, ledger->info().hash);
                v.setFieldU32(sfLedgerSequence, ledger->seq());
                v.setFlag(vfFullValidation);
            });
    };

    /**
     * Walk the ledger history and create validation messages for the ledgers
     *
     * @tparam NeedValidation a function to decided if a validation is needed
     * @param needVal if a validation is needed for this particular combination
     *        of ledger and validator
     */
    template <class NeedValidation>
    void
    walkHistoryAndAddValidations(NeedValidation&& needVal)
    {
        std::uint32_t curr = 0;
        std::size_t need = 256 + 1;
        // only last 256 + 1 ledgers need validations
        if (history.size() > need)
            curr = history.size() - need;
        for (; curr != history.size(); ++curr)
        {
            for (std::size_t i = 0; i < param.numNodes; ++i)
            {
                if (needVal(history[curr], i))
                {
                    RCLValidation v(createSTVal(history[curr], UNLNodeIDs[i]));
                    v.setTrusted();
                    validations.add(UNLNodeIDs[i], v);
                }
            }
        }
    }

    std::shared_ptr<Ledger const>
    lastLedger() const
    {
        return history.back();
    }

    jtx::Env env;
    Parameter param;
    RCLValidations& validations;
    std::vector<PublicKey> UNLKeys;
    hash_set<PublicKey> UNLKeySet;
    std::vector<NodeID> UNLNodeIDs;
    hash_set<NodeID> UNLNodeIDSet;
    LedgerHistory history;
    bool goodHistory;
};

auto defaultUNLRPreVote = [](NegativeUNLVote& vote) {};
/**
 * Create a NegativeUNLVote object. It then creates ttUNL_MODIFY Tx as its vote
 * on negative UNL changes.
 *
 * @tparam PreVote a function to be called before vote
 * @param history the ledger history
 * @param myId the voting validator
 * @param expect the number of ttUNL_MODIFY Tx expected
 * @param pre the PreVote function
 * @return true if the number of ttUNL_MODIFY Txes created meet expectation
 */
template <typename PreVote = decltype(defaultUNLRPreVote)>
bool
voteAndCheckUNLR(
    URNetworkHistory& history,
    NodeID const& myId,
    std::size_t expectModify,
    std::size_t expectReport,
    PreVote const& pre = defaultUNLRPreVote)
{
    NegativeUNLVote vote(myId, history.env.journal, history.env.app());
    pre(vote);
    auto txSet = std::make_shared<SHAMap>(
        SHAMapType::TRANSACTION, history.env.app().getNodeFamily());
    vote.doVoting(
        history.lastLedger(), history.UNLKeySet, history.validations, txSet);

    return countUNLRTx(txSet) == expectReport &&
        unl::countTx(txSet) >= expectModify;
}

/*
 * Test the doVoting function of UNLReport.
 * The test cases are split to 5 classes for parallel execution.
 *
 * Voting tests: (use hasToDisable and hasToReEnable in some of the cases)
 *
 * == all good score, nUnl empty
 * -- txSet.size = 0
 * == all good score, nUnl not empty (use hasToDisable)
 * -- txSet.size = 1
 *
 * == 2 nodes offline, nUnl empty (use hasToReEnable)
 * -- txSet.size = 1
 * == 2 nodes offline, in nUnl
 * -- txSet.size = 0
 *
 * == 2 nodes offline, not in nUnl, but maxListed
 * -- txSet.size = 0
 *
 * == 2 nodes offline including me, not in nUnl
 * -- txSet.size = 0
 * == 2 nodes offline, not in negativeUNL, but I'm not a validator
 * -- txSet.size = 0
 * == 2 in nUnl, but not in unl, no other remove candidates
 * -- txSet.size = 1
 *
 * == 2 new validators have bad scores
 * -- txSet.size = 0
 * == 2 expired new validators have bad scores
 * -- txSet.size = 1
 */

class UNLReportVoteGoodScore_test : public beast::unit_test::suite
{
    void
    testDoVoting()
    {
        testcase("Do Voting");

        for (bool const withVLImport : {true, false})
        {
            {
                //== all good score, negativeUNL empty
                //-- txSet.size = 0
                auto const numNodes = 51;
                auto const unlrCount = withVLImport ? 52 : 51;
                URNetworkHistory history = {
                    *this, {numNodes, 0, false, false, withVLImport, {}}};
                BEAST_EXPECT(history.goodHistory);
                if (history.goodHistory)
                {
                    history.walkHistoryAndAddValidations(
                        [&](std::shared_ptr<Ledger const> const& l,
                            std::size_t idx) -> bool { return true; });
                    BEAST_EXPECT(voteAndCheckUNLR(
                        history, history.UNLNodeIDs[0], 0, unlrCount));
                }
            }

            {
                // all good score, negativeUNL not empty (use hasToDisable)
                //-- txSet.size = 1
                auto const numNodes = 37;
                auto const unlrCount = withVLImport ? 38 : 37;
                URNetworkHistory history = {
                    *this, {numNodes, 0, true, false, withVLImport, {}}};
                BEAST_EXPECT(history.goodHistory);
                if (history.goodHistory)
                {
                    history.walkHistoryAndAddValidations(
                        [&](std::shared_ptr<Ledger const> const& l,
                            std::size_t idx) -> bool { return true; });
                    BEAST_EXPECT(voteAndCheckUNLR(
                        history, history.UNLNodeIDs[0], 1, unlrCount));
                }
            }
        }
    }

    void
    run() override
    {
        testDoVoting();
    }
};

class UNLReportVoteOffline_test : public beast::unit_test::suite
{
    void
    testDoVoting()
    {
        testcase("Do Voting");

        for (bool const withVLImport : {true, false})
        {
            {
                //== 2 nodes offline, negativeUNL empty (use hasToReEnable)
                //-- txSet.size = 1
                auto const numNodes = 29;
                auto const unlrCount = withVLImport ? 28 : 27;
                URNetworkHistory history = {
                    *this, {numNodes, 1, false, true, withVLImport, {}}};
                BEAST_EXPECT(history.goodHistory);
                if (history.goodHistory)
                {
                    history.walkHistoryAndAddValidations(
                        [&](std::shared_ptr<Ledger const> const& l,
                            std::size_t idx) -> bool {
                            // skip node 0 and node 1
                            return idx > 1;
                        });
                    BEAST_EXPECT(voteAndCheckUNLR(
                        history, history.UNLNodeIDs.back(), 1, unlrCount));
                }
            }

            {
                // 2 nodes offline, in negativeUNL
                //-- txSet.size = 0
                auto const numNodes = 30;
                auto const unlrCount = withVLImport ? 29 : 28;
                URNetworkHistory history = {
                    *this, {numNodes, 1, true, false, withVLImport, {}}};
                BEAST_EXPECT(history.goodHistory);
                if (history.goodHistory)
                {
                    NodeID n1 = calcNodeID(
                        *history.lastLedger()->negativeUNL().begin());
                    NodeID n2 =
                        calcNodeID(*history.lastLedger()->validatorToDisable());
                    history.walkHistoryAndAddValidations(
                        [&](std::shared_ptr<Ledger const> const& l,
                            std::size_t idx) -> bool {
                            // skip node 0 and node 1
                            return history.UNLNodeIDs[idx] != n1 &&
                                history.UNLNodeIDs[idx] != n2;
                        });
                    BEAST_EXPECT(voteAndCheckUNLR(
                        history, history.UNLNodeIDs.back(), 0, unlrCount));
                }
            }
        }
    }

    void
    run() override
    {
        testDoVoting();
    }
};

class UNLReportVoteMaxListed_test : public beast::unit_test::suite
{
    void
    testDoVoting()
    {
        testcase("Do Voting");

        for (bool const withVLImport : {true, false})
        {
            {
                // 2 nodes offline, not in negativeUNL, but maxListed
                //-- txSet.size = 0
                auto const numNodes = 32;
                auto const unlrCount = withVLImport ? 22 : 21;
                URNetworkHistory history = {
                    *this, {numNodes, 8, true, true, withVLImport, {}}};
                BEAST_EXPECT(history.goodHistory);
                if (history.goodHistory)
                {
                    history.walkHistoryAndAddValidations(
                        [&](std::shared_ptr<Ledger const> const& l,
                            std::size_t idx) -> bool {
                            // skip node 0 ~ 10
                            return idx > 10;
                        });
                    BEAST_EXPECT(voteAndCheckUNLR(
                        history, history.UNLNodeIDs.back(), 0, unlrCount));
                }
            }
        }
    }

    void
    run() override
    {
        testDoVoting();
    }
};

class UNLReportVoteRetiredValidator_test : public beast::unit_test::suite
{
    void
    testDoVoting()
    {
        testcase("Do Voting");

        for (bool const withVLImport : {true, false})
        {
            {
                //== 2 nodes offline including me, not in negativeUNL
                //-- txSet.size = 0
                auto const numNodes = 35;
                auto const unlrCount = withVLImport ? 0 : 0;
                URNetworkHistory history = {
                    *this, {numNodes, 0, false, false, withVLImport, {}}};
                BEAST_EXPECT(history.goodHistory);
                if (history.goodHistory)
                {
                    history.walkHistoryAndAddValidations(
                        [&](std::shared_ptr<Ledger const> const& l,
                            std::size_t idx) -> bool { return idx > 1; });
                    BEAST_EXPECT(voteAndCheckUNLR(
                        history, history.UNLNodeIDs[0], 0, unlrCount));
                }
            }

            {
                // 2 nodes offline, not in negativeUNL, but I'm not a validator
                //-- txSet.size = 0
                auto const numNodes = 40;
                auto const unlrCount = withVLImport ? 0 : 0;
                URNetworkHistory history = {
                    *this, {numNodes, 0, false, false, withVLImport, {}}};
                BEAST_EXPECT(history.goodHistory);
                if (history.goodHistory)
                {
                    history.walkHistoryAndAddValidations(
                        [&](std::shared_ptr<Ledger const> const& l,
                            std::size_t idx) -> bool { return idx > 1; });
                    BEAST_EXPECT(voteAndCheckUNLR(
                        history, NodeID(0xdeadbeef), 0, unlrCount));
                }
            }

            {
                //== 2 in negativeUNL, but not in unl, no other remove
                // candidates
                //-- txSet.size = 1
                auto const numNodes = 25;
                auto const unlrCount = withVLImport ? 24 : 23;
                URNetworkHistory history = {
                    *this, {numNodes, 2, false, false, withVLImport, {}}};
                BEAST_EXPECT(history.goodHistory);
                if (history.goodHistory)
                {
                    history.walkHistoryAndAddValidations(
                        [&](std::shared_ptr<Ledger const> const& l,
                            std::size_t idx) -> bool { return idx > 1; });
                    BEAST_EXPECT(voteAndCheckUNLR(
                        history,
                        history.UNLNodeIDs.back(),
                        1,
                        unlrCount,
                        [&](NegativeUNLVote& vote) {
                            history.UNLKeySet.erase(history.UNLKeys[0]);
                            history.UNLKeySet.erase(history.UNLKeys[1]);
                        }));
                }
            }
        }
    }

    void
    run() override
    {
        testDoVoting();
    }
};

class UNLReportVoteNewValidator_test : public beast::unit_test::suite
{
    void
    testDoVoting()
    {
        testcase("Do Voting");

        for (bool const withVLImport : {true, false})
        {
            {
                //== 2 new validators have bad scores
                //-- txSet.size = 0
                auto const numNodes = 15;
                auto const unlrCount = withVLImport ? 16 : 15;
                URNetworkHistory history = {
                    *this, {numNodes, 0, false, false, withVLImport, {}}};
                BEAST_EXPECT(history.goodHistory);
                if (history.goodHistory)
                {
                    history.walkHistoryAndAddValidations(
                        [&](std::shared_ptr<Ledger const> const& l,
                            std::size_t idx) -> bool { return true; });
                    BEAST_EXPECT(voteAndCheckUNLR(
                        history,
                        history.UNLNodeIDs[0],
                        0,
                        unlrCount,
                        [&](NegativeUNLVote& vote) {
                            auto extra_key_1 =
                                randomKeyPair(KeyType::ed25519).first;
                            auto extra_key_2 =
                                randomKeyPair(KeyType::ed25519).first;
                            history.UNLKeySet.insert(extra_key_1);
                            history.UNLKeySet.insert(extra_key_2);
                            hash_set<NodeID> nowTrusted;
                            nowTrusted.insert(calcNodeID(extra_key_1));
                            nowTrusted.insert(calcNodeID(extra_key_2));
                            vote.newValidators(
                                history.lastLedger()->seq(), nowTrusted);
                        }));
                }
            }

            {
                //== 2 expired new validators have bad scores
                //-- txSet.size = 1
                auto const numNodes = 21;
                auto const unlrCount = withVLImport ? 22 : 21;
                URNetworkHistory history = {
                    *this,
                    {numNodes,
                     0,
                     false,
                     false,
                     withVLImport,
                     NegativeUNLVote::newValidatorDisableSkip * 2}};
                BEAST_EXPECT(history.goodHistory);
                if (history.goodHistory)
                {
                    history.walkHistoryAndAddValidations(
                        [&](std::shared_ptr<Ledger const> const& l,
                            std::size_t idx) -> bool { return true; });
                    BEAST_EXPECT(voteAndCheckUNLR(
                        history,
                        history.UNLNodeIDs[0],
                        1,
                        unlrCount,
                        [&](NegativeUNLVote& vote) {
                            auto extra_key_1 =
                                randomKeyPair(KeyType::ed25519).first;
                            auto extra_key_2 =
                                randomKeyPair(KeyType::ed25519).first;
                            history.UNLKeySet.insert(extra_key_1);
                            history.UNLKeySet.insert(extra_key_2);
                            hash_set<NodeID> nowTrusted;
                            nowTrusted.insert(calcNodeID(extra_key_1));
                            nowTrusted.insert(calcNodeID(extra_key_2));
                            vote.newValidators(256, nowTrusted);
                        }));
                }
            }
        }
    }

    void
    run() override
    {
        testDoVoting();
    }
};

BEAST_DEFINE_TESTSUITE(UNLReport, ledger, ripple);
BEAST_DEFINE_TESTSUITE(UNLReportNoAmendment, ledger, ripple);
BEAST_DEFINE_TESTSUITE(UNLReportFork, consensus, ripple);
BEAST_DEFINE_TESTSUITE_PRIO(UNLReportVoteGoodScore, consensus, ripple, 1);
BEAST_DEFINE_TESTSUITE(UNLReportVoteOffline, consensus, ripple);
BEAST_DEFINE_TESTSUITE(UNLReportVoteMaxListed, consensus, ripple);
BEAST_DEFINE_TESTSUITE_PRIO(
    UNLReportVoteRetiredValidator,
    consensus,
    ripple,
    1);
BEAST_DEFINE_TESTSUITE(UNLReportVoteNewValidator, consensus, ripple);

///////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////

bool
applyAndTestUNLRResult(jtx::Env& env, OpenView& view, STTx const& tx, bool pass)
{
    auto res = apply(env.app(), view, tx, ApplyFlags::tapNONE, env.journal);
    if (pass)
        return res.first == tesSUCCESS;
    else
        return res.first == tefFAILURE || res.first == temDISABLED ||
            res.first == temMALFORMED ||
            res.first == telIMPORT_VL_KEY_NOT_RECOGNISED;
}

bool
VerifyUNLRPubKeyAndSeq(
    std::shared_ptr<Ledger const> const& l,
    hash_map<PublicKey, std::uint32_t> nUnlLedgerSeq)
{
    auto sle = l->read(keylet::negativeUNL());
    if (!sle)
        return false;
    if (!sle->isFieldPresent(sfDisabledValidators))
        return false;

    auto const& nUnlData = sle->getFieldArray(sfDisabledValidators);
    if (nUnlData.size() != nUnlLedgerSeq.size())
        return false;

    for (auto const& n : nUnlData)
    {
        if (!n.isFieldPresent(sfFirstLedgerSequence) ||
            !n.isFieldPresent(sfPublicKey))
            return false;

        auto seq = n.getFieldU32(sfFirstLedgerSequence);
        auto d = n.getFieldVL(sfPublicKey);
        auto s = makeSlice(d);
        if (!publicKeyType(s))
            return false;
        PublicKey pk(s);
        auto it = nUnlLedgerSeq.find(pk);
        if (it == nUnlLedgerSeq.end())
            return false;
        if (it->second != seq)
            return false;
        nUnlLedgerSeq.erase(it);
    }
    return nUnlLedgerSeq.size() == 0;
}

std::size_t
countUNLRTx(std::shared_ptr<SHAMap> const& txSet)
{
    std::size_t count = 0;
    for (auto i = txSet->begin(); i != txSet->end(); ++i)
    {
        auto raw = i->slice();
        if (raw[0] == 0x12U && raw[1] == 0 && raw[2] == 0x68U)
            count++;
    }
    return count;
};

bool
hasUNLReport(jtx::Env const& env)
{
    auto const slep = env.le(keylet::UNLReport());
    return slep != nullptr;
}

bool
isImportVL(jtx::Env const& env, PublicKey const& pk)
{
    auto const slep = env.le(keylet::UNLReport());
    auto const& vlKeys = slep->getFieldArray(sfImportVLKeys);
    for (auto const& k : vlKeys)
        if (PublicKey(k[sfPublicKey]) == pk)
            return true;
    return false;
}

bool
isActiveValidator(jtx::Env const& env, PublicKey const& pk)
{
    auto const slep = env.le(keylet::UNLReport());
    auto const& activeVLs = slep->getFieldArray(sfActiveValidators);
    for (auto const& k : activeVLs)
        if (PublicKey(k[sfPublicKey]) == pk)
            return true;
    return false;
}

std::vector<PublicKey>
createUNLRPublicKeys(std::size_t n)
{
    std::vector<PublicKey> keys;
    std::size_t ss = 33;
    std::vector<uint8_t> data(ss, 0);
    data[0] = 0xED;
    for (int i = 0; i < n; ++i)
    {
        data[1]++;
        Slice s(data.data(), ss);
        keys.emplace_back(s);
    }
    return keys;
}

STTx
createUNLRTx(
    LedgerIndex seq,
    PublicKey const& importKey,
    PublicKey const& valKey)
{
    auto fill = [&](auto& obj) {
        obj.setFieldU32(sfLedgerSequence, seq);
        obj.set(([&]() {
            auto inner = std::make_unique<STObject>(sfActiveValidator);
            inner->setFieldVL(sfPublicKey, valKey);
            return inner;
        })());
        obj.set(([&]() {
            auto inner = std::make_unique<STObject>(sfImportVLKey);
            inner->setFieldVL(sfPublicKey, importKey);
            return inner;
        })());
    };
    return STTx(ttUNL_REPORT, fill);
}

}  // namespace test
}  // namespace ripple