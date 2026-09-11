//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

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

#include <xrpld/app/consensus/RCLCxPeerPos.h>
#include <xrpld/consensus/ConsensusProposal.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>
#include <cstring>
#include <sstream>

namespace ripple {
namespace test {

class ExtendedPosition_test : public beast::unit_test::suite
{
    // Generate deterministic test hashes
    static uint256
    makeHash(char const* label)
    {
        return sha512Half(Slice(label, std::strlen(label)));
    }

    void
    testSerializationRoundTrip()
    {
        testcase("Serialization round-trip");

        //@@start test-extended-position-legacy-compat
        // Empty position (legacy compat)
        {
            auto const txSet = makeHash("txset-a");
            ExtendedPosition pos{txSet};

            Serializer s;
            pos.add(s);

            // Should be exactly 32 bytes (no flags byte)
            BEAST_EXPECT(s.getDataLength() == 32);

            SerialIter sit(s.slice());
            auto deserialized =
                ExtendedPosition::fromSerialIter(sit, s.getDataLength());

            BEAST_EXPECT(deserialized.has_value());
            if (!deserialized)
                return;
            BEAST_EXPECT(deserialized->txSetHash == txSet);
            BEAST_EXPECT(!deserialized->myCommitment);
            BEAST_EXPECT(!deserialized->myReveal);
            BEAST_EXPECT(!deserialized->commitSetHash);
            BEAST_EXPECT(!deserialized->entropySetHash);
            BEAST_EXPECT(!deserialized->exportSigSetHash);
            BEAST_EXPECT(!deserialized->exportSignaturesHash);
            BEAST_EXPECT(!deserialized->observedParticipantsHash);
        }
        //@@end test-extended-position-legacy-compat

        //@@start test-extended-position-export-root-wire-bit
        // Pin the existing Export root allocation independently of the
        // all-fields fixture: byte 32 is flags, and bit 0x10 carries exactly
        // one following uint256 exportSigSetHash.
        {
            auto const txSet = makeHash("txset-export-root-wire");
            auto const exportRoot = makeHash("export-root-wire");
            ExtendedPosition pos{txSet};
            pos.exportSigSetHash = exportRoot;

            Serializer s;
            pos.add(s);
            BEAST_EXPECT(s.getDataLength() == 65);
            BEAST_EXPECT(s.peekData()[32] == 0x10);

            SerialIter sit(s.slice());
            auto const deserialized =
                ExtendedPosition::fromSerialIter(sit, s.getDataLength());
            BEAST_EXPECT(deserialized);
            if (deserialized)
            {
                BEAST_EXPECT(deserialized->txSetHash == txSet);
                BEAST_EXPECT(deserialized->exportSigSetHash == exportRoot);
                BEAST_EXPECT(!deserialized->exportSignaturesHash);
            }
        }
        //@@end test-extended-position-export-root-wire-bit

        // Position with commitment
        {
            auto const txSet = makeHash("txset-b");
            auto const commit = makeHash("commit-b");

            ExtendedPosition pos{txSet};
            pos.myCommitment = commit;

            Serializer s;
            pos.add(s);

            // 32 (txSet) + 1 (flags) + 32 (commitment) = 65
            BEAST_EXPECT(s.getDataLength() == 65);

            SerialIter sit(s.slice());
            auto deserialized =
                ExtendedPosition::fromSerialIter(sit, s.getDataLength());

            BEAST_EXPECT(deserialized.has_value());
            if (!deserialized)
                return;
            BEAST_EXPECT(deserialized->txSetHash == txSet);
            BEAST_EXPECT(deserialized->myCommitment == commit);
            BEAST_EXPECT(!deserialized->myReveal);
        }

        // Position with diagnostic participant hash only
        {
            auto const txSet = makeHash("txset-participants");
            auto const participants = makeHash("participants");

            ExtendedPosition pos{txSet};
            pos.observedParticipantsHash = participants;

            Serializer s;
            pos.add(s);

            // 32 (txSet) + 1 (flags) + 32 (participant hash) = 65
            BEAST_EXPECT(s.getDataLength() == 65);

            SerialIter sit(s.slice());
            auto deserialized =
                ExtendedPosition::fromSerialIter(sit, s.getDataLength());

            BEAST_EXPECT(deserialized.has_value());
            if (!deserialized)
                return;
            BEAST_EXPECT(deserialized->txSetHash == txSet);
            BEAST_EXPECT(
                deserialized->observedParticipantsHash == participants);
            BEAST_EXPECT(!deserialized->myCommitment);
            BEAST_EXPECT(!deserialized->myReveal);
        }

        // Position with all fields
        {
            auto const txSet = makeHash("txset-c");
            auto const commitSet = makeHash("commitset-c");
            auto const entropySet = makeHash("entropyset-c");
            auto const exportSigSet = makeHash("exportsigset-c");
            auto const exportSigs = makeHash("exportsigs-c");
            auto const participants = makeHash("participants-c");
            auto const commit = makeHash("commit-c");
            auto const reveal = makeHash("reveal-c");

            ExtendedPosition pos{txSet};
            pos.commitSetHash = commitSet;
            pos.entropySetHash = entropySet;
            pos.exportSigSetHash = exportSigSet;
            pos.exportSignaturesHash = exportSigs;
            pos.observedParticipantsHash = participants;
            pos.myCommitment = commit;
            pos.myReveal = reveal;

            Serializer s;
            pos.add(s);

            // 32 + 1 + 7*32 = 257
            BEAST_EXPECT(s.getDataLength() == 257);

            SerialIter sit(s.slice());
            auto deserialized =
                ExtendedPosition::fromSerialIter(sit, s.getDataLength());

            BEAST_EXPECT(deserialized.has_value());
            if (!deserialized)
                return;
            BEAST_EXPECT(deserialized->txSetHash == txSet);
            BEAST_EXPECT(deserialized->commitSetHash == commitSet);
            BEAST_EXPECT(deserialized->entropySetHash == entropySet);
            BEAST_EXPECT(deserialized->exportSigSetHash == exportSigSet);
            BEAST_EXPECT(deserialized->exportSignaturesHash == exportSigs);
            BEAST_EXPECT(
                deserialized->observedParticipantsHash == participants);
            BEAST_EXPECT(deserialized->myCommitment == commit);
            BEAST_EXPECT(deserialized->myReveal == reveal);
        }
    }

    void
    testSigningConsistency()
    {
        testcase("Signing hash consistency");

        // The signing hash from ConsensusProposal::signingHash() must match
        // what a receiver would compute via the same function after
        // deserializing the ExtendedPosition from the wire.

        auto const [pk, sk] = randomKeyPair(KeyType::secp256k1);
        auto const nodeId = calcNodeID(pk);
        auto const prevLedger = makeHash("prevledger");
        auto const closeTime =
            NetClock::time_point{NetClock::duration{1234567}};

        //@@start test-extended-position-signing-binds-fields
        // Test with commitment (the case that was failing)
        {
            auto const txSet = makeHash("txset-sign");
            auto const commit = makeHash("commitment-sign");

            ExtendedPosition pos{txSet};
            pos.myCommitment = commit;

            using Proposal =
                ConsensusProposal<NodeID, uint256, ExtendedPosition>;

            Proposal prop{
                prevLedger,
                Proposal::seqJoin,
                pos,
                closeTime,
                NetClock::time_point{},
                nodeId};

            // Sign it (same as propose() does)
            auto const signingHash = prop.signingHash();
            auto sig = signDigest(pk, sk, signingHash);

            // Serialize position to wire format
            Serializer positionData;
            pos.add(positionData);
            auto const posSlice = positionData.slice();

            // Deserialize (same as PeerImp::onMessage does)
            SerialIter sit(posSlice);
            auto const maybeReceivedPos =
                ExtendedPosition::fromSerialIter(sit, posSlice.size());

            BEAST_EXPECT(maybeReceivedPos.has_value());
            if (!maybeReceivedPos)
                return;

            // Reconstruct proposal on receiver side
            Proposal receivedProp{
                prevLedger,
                Proposal::seqJoin,
                *maybeReceivedPos,
                closeTime,
                NetClock::time_point{},
                nodeId};

            // The signing hash must match
            BEAST_EXPECT(receivedProp.signingHash() == signingHash);

            // Verify signature (same as checkSign does)
            BEAST_EXPECT(
                verifyDigest(pk, receivedProp.signingHash(), sig, false));
        }
        //@@end test-extended-position-signing-binds-fields

        // Test without commitment (legacy case)
        {
            auto const txSet = makeHash("txset-legacy");
            ExtendedPosition pos{txSet};

            using Proposal =
                ConsensusProposal<NodeID, uint256, ExtendedPosition>;

            Proposal prop{
                prevLedger,
                Proposal::seqJoin,
                pos,
                closeTime,
                NetClock::time_point{},
                nodeId};

            auto const signingHash = prop.signingHash();
            auto sig = signDigest(pk, sk, signingHash);

            Serializer positionData;
            pos.add(positionData);

            SerialIter sit(positionData.slice());
            auto const maybeReceivedPos = ExtendedPosition::fromSerialIter(
                sit, positionData.getDataLength());

            BEAST_EXPECT(maybeReceivedPos.has_value());
            if (!maybeReceivedPos)
                return;

            Proposal receivedProp{
                prevLedger,
                Proposal::seqJoin,
                *maybeReceivedPos,
                closeTime,
                NetClock::time_point{},
                nodeId};

            BEAST_EXPECT(receivedProp.signingHash() == signingHash);
            BEAST_EXPECT(
                verifyDigest(pk, receivedProp.signingHash(), sig, false));
        }
    }

    void
    testSuppressionConsistency()
    {
        testcase("Suppression hash consistency");

        // proposalUniqueId must produce the same result on sender and
        // receiver when given the same ExtendedPosition data.

        auto const [pk, sk] = randomKeyPair(KeyType::secp256k1);
        auto const prevLedger = makeHash("prevledger-supp");
        auto const closeTime =
            NetClock::time_point{NetClock::duration{1234567}};
        std::uint32_t const proposeSeq = 0;

        auto const txSet = makeHash("txset-supp");
        auto const commit = makeHash("commitment-supp");

        ExtendedPosition pos{txSet};
        pos.myCommitment = commit;

        // Sign (to get a real signature for suppression)
        using Proposal = ConsensusProposal<NodeID, uint256, ExtendedPosition>;
        Proposal prop{
            prevLedger,
            proposeSeq,
            pos,
            closeTime,
            NetClock::time_point{},
            calcNodeID(pk)};

        auto sig = signDigest(pk, sk, prop.signingHash());

        // Sender computes suppression
        auto const senderSuppression =
            proposalUniqueId(pos, prevLedger, proposeSeq, closeTime, pk, sig);

        // Simulate wire: serialize and deserialize
        Serializer positionData;
        pos.add(positionData);
        SerialIter sit(positionData.slice());
        auto const maybeReceivedPos =
            ExtendedPosition::fromSerialIter(sit, positionData.getDataLength());

        BEAST_EXPECT(maybeReceivedPos.has_value());
        if (!maybeReceivedPos)
            return;

        // Receiver computes suppression
        auto const receiverSuppression = proposalUniqueId(
            *maybeReceivedPos, prevLedger, proposeSeq, closeTime, pk, sig);

        BEAST_EXPECT(senderSuppression == receiverSuppression);
    }

    void
    testPeerPosition()
    {
        testcase("RCLCxPeerPos signed proposal wrapper");

        auto const [pk, sk] = randomKeyPair(KeyType::secp256k1);
        auto const nodeId = calcNodeID(pk);
        auto const prevLedger = makeHash("prevledger-peer");
        auto const closeTime =
            NetClock::time_point{NetClock::duration{7654321}};

        ExtendedPosition pos{makeHash("txset-peer")};
        pos.commitSetHash = makeHash("commitset-peer");
        pos.entropySetHash = makeHash("entropyset-peer");
        pos.exportSigSetHash = makeHash("exportsigset-peer");
        pos.exportSignaturesHash = makeHash("exportsigs-peer");
        pos.observedParticipantsHash = makeHash("participants-peer");
        pos.myCommitment = makeHash("commitment-peer");
        pos.myReveal = makeHash("reveal-peer");

        using Proposal = ConsensusProposal<NodeID, uint256, ExtendedPosition>;
        Proposal prop{
            prevLedger,
            Proposal::seqJoin,
            pos,
            closeTime,
            NetClock::time_point{},
            nodeId};

        auto const sig = signDigest(pk, sk, prop.signingHash());
        auto const suppression = proposalUniqueId(
            pos, prevLedger, prop.proposeSeq(), closeTime, pk, sig);
        std::vector<std::string> exportSignatures{
            "export-sig-a", "export-sig-b"};

        RCLCxPeerPos peer{
            pk,
            sig,
            suppression,
            Proposal{
                prevLedger,
                Proposal::seqJoin,
                pos,
                closeTime,
                NetClock::time_point{},
                nodeId},
            exportSignatures};

        BEAST_EXPECT(peer.checkSign());
        BEAST_EXPECT(peer.publicKey() == pk);
        BEAST_EXPECT(peer.signature().size() == sig.size());
        BEAST_EXPECT(peer.suppressionID() == suppression);
        BEAST_EXPECT(peer.proposal().position().txSetHash == pos.txSetHash);
        BEAST_EXPECT(peer.exportSignatures() == exportSignatures);
        BEAST_EXPECT(!peer.render().empty());

        auto const json = peer.getJson();
        BEAST_EXPECT(json.isMember(jss::peer_id));
        BEAST_EXPECT(
            json[jss::peer_id].asString() ==
            toBase58(TokenType::NodePublic, pk));

        auto badSig = sig;
        badSig.data()[badSig.size() - 1] ^= 0x01;
        RCLCxPeerPos badPeer{
            pk,
            badSig,
            suppression,
            Proposal{
                prevLedger,
                Proposal::seqJoin,
                pos,
                closeTime,
                NetClock::time_point{},
                nodeId}};
        BEAST_EXPECT(!badPeer.checkSign());

        auto mutated = pos;
        mutated.myReveal = makeHash("reveal-peer-mutated");
        BEAST_EXPECT(
            proposalUniqueId(
                mutated, prevLedger, prop.proposeSeq(), closeTime, pk, sig) !=
            suppression);

        mutated = pos;
        //@@start test-extended-position-export-root-binds-proposal
        mutated.exportSigSetHash = makeHash("exportsigset-peer-mutated");
        BEAST_EXPECT(
            proposalUniqueId(
                mutated, prevLedger, prop.proposeSeq(), closeTime, pk, sig) !=
            suppression);
        Proposal mutatedRootProposal{
            prevLedger,
            Proposal::seqJoin,
            mutated,
            closeTime,
            NetClock::time_point{},
            nodeId};
        BEAST_EXPECT(mutatedRootProposal.signingHash() != prop.signingHash());
        BEAST_EXPECT(
            !verifyDigest(pk, mutatedRootProposal.signingHash(), sig, false));
        //@@end test-extended-position-export-root-binds-proposal

        mutated = pos;
        mutated.exportSignaturesHash = makeHash("exportsigs-peer-mutated");
        BEAST_EXPECT(
            proposalUniqueId(
                mutated, prevLedger, prop.proposeSeq(), closeTime, pk, sig) !=
            suppression);

        BEAST_EXPECT(
            proposalUniqueId(
                pos,
                makeHash("prevledger-peer-mutated"),
                prop.proposeSeq(),
                closeTime,
                pk,
                sig) != suppression);
        BEAST_EXPECT(
            proposalUniqueId(
                pos, prevLedger, prop.proposeSeq() + 1, closeTime, pk, sig) !=
            suppression);
        BEAST_EXPECT(
            proposalUniqueId(
                pos,
                prevLedger,
                prop.proposeSeq(),
                NetClock::time_point{NetClock::duration{7654322}},
                pk,
                sig) != suppression);

        auto const [otherPk, unusedSk] = randomKeyPair(KeyType::secp256k1);
        (void)unusedSk;
        BEAST_EXPECT(
            proposalUniqueId(
                pos, prevLedger, prop.proposeSeq(), closeTime, otherPk, sig) !=
            suppression);
    }

    void
    testMalformedPayload()
    {
        testcase("Malformed payload rejected");

        //@@start test-extended-position-malformed-canonicality
        // Too short (< 32 bytes)
        {
            Serializer s;
            s.add32(0xDEADBEEF);  // only 4 bytes
            SerialIter sit(s.slice());
            auto result =
                ExtendedPosition::fromSerialIter(sit, s.getDataLength());
            BEAST_EXPECT(!result.has_value());
        }

        // Empty payload
        {
            Serializer s;
            SerialIter sit(s.slice());
            auto result = ExtendedPosition::fromSerialIter(sit, 0);
            BEAST_EXPECT(!result.has_value());
        }

        // Flags claim fields that aren't present (truncated)
        {
            auto const txSet = makeHash("txset-malformed");
            Serializer s;
            s.addBitString(txSet);
            // flags = 0x0F (all 4 fields), but no field data follows
            s.add8(0x0F);
            SerialIter sit(s.slice());
            auto result =
                ExtendedPosition::fromSerialIter(sit, s.getDataLength());
            BEAST_EXPECT(!result.has_value());
        }

        // Flags claim 2 fields but only 1 field's worth of data
        {
            auto const txSet = makeHash("txset-malformed2");
            auto const commit = makeHash("commit-malformed2");
            Serializer s;
            s.addBitString(txSet);
            // flags = 0x03 (commitSetHash + entropySetHash), but only
            // provide commitSetHash data
            s.add8(0x03);
            s.addBitString(commit);
            SerialIter sit(s.slice());
            auto result =
                ExtendedPosition::fromSerialIter(sit, s.getDataLength());
            BEAST_EXPECT(!result.has_value());
        }

        // Unknown flag bits above known extension fields (wire malleability)
        {
            auto const txSet = makeHash("txset-unkflags");
            Serializer s;
            s.addBitString(txSet);
            s.add8(0x81);  // bit 7 is unknown, bit 0 = commitSetHash
            s.addBitString(makeHash("commitset-unkflags"));
            SerialIter sit(s.slice());
            auto result =
                ExtendedPosition::fromSerialIter(sit, s.getDataLength());
            BEAST_EXPECT(!result.has_value());
        }

        // Size says extended payload, but no flag byte remains.
        {
            auto const txSet = makeHash("txset-missing-flags");
            Serializer s;
            s.addBitString(txSet);
            SerialIter sit(s.slice());
            auto result = ExtendedPosition::fromSerialIter(sit, 33);
            BEAST_EXPECT(!result.has_value());
        }

        // Zero flags are a non-canonical duplicate of the 32-byte legacy form.
        {
            auto const txSet = makeHash("txset-zero-flags");
            Serializer s;
            s.addBitString(txSet);
            s.add8(0);
            SerialIter sit(s.slice());
            auto result =
                ExtendedPosition::fromSerialIter(sit, s.getDataLength());
            BEAST_EXPECT(!result.has_value());
        }

        // Trailing extra bytes after valid fields
        {
            auto const txSet = makeHash("txset-trailing");
            auto const commitSet = makeHash("commitset-trailing");
            Serializer s;
            s.addBitString(txSet);
            s.add8(0x01);  // commitSetHash only
            s.addBitString(commitSet);
            s.add32(0xDEADBEEF);  // 4 extra trailing bytes
            SerialIter sit(s.slice());
            auto result =
                ExtendedPosition::fromSerialIter(sit, s.getDataLength());
            BEAST_EXPECT(!result.has_value());
        }

        // Valid flags with exactly the right amount of data (should succeed)
        {
            auto const txSet = makeHash("txset-ok");
            auto const commitSet = makeHash("commitset-ok");
            Serializer s;
            s.addBitString(txSet);
            s.add8(0x01);  // commitSetHash only
            s.addBitString(commitSet);
            SerialIter sit(s.slice());
            auto result =
                ExtendedPosition::fromSerialIter(sit, s.getDataLength());
            BEAST_EXPECT(result.has_value());
            if (result)
            {
                BEAST_EXPECT(result->txSetHash == txSet);
                BEAST_EXPECT(result->commitSetHash == commitSet);
                BEAST_EXPECT(!result->entropySetHash);
            }
        }
        //@@end test-extended-position-malformed-canonicality
    }

    void
    testTxSetIdentity()
    {
        testcase("Tx-set identity is explicit");

        auto const txSet = makeHash("txset-eq");
        auto const txSet2 = makeHash("txset-eq-2");

        ExtendedPosition a{txSet};
        a.myCommitment = makeHash("commit1-eq");

        ExtendedPosition b{txSet};
        b.myCommitment = makeHash("commit2-eq");

        // Same txSetHash, different leaves -> same consensus tx-set key.
        BEAST_EXPECT(positionTxSetID(a) == positionTxSetID(b));

        // Same txSetHash, different commitSetHash -> same tx-set key
        // (sub-state quorum handles commitSetHash agreement)
        b.commitSetHash = makeHash("cs-eq");
        BEAST_EXPECT(positionTxSetID(a) == positionTxSetID(b));

        // Same txSetHash, different entropySetHash -> same tx-set key
        b.entropySetHash = makeHash("es-eq");
        BEAST_EXPECT(positionTxSetID(a) == positionTxSetID(b));

        // Same txSetHash, different export signature digest -> same tx-set key
        b.exportSignaturesHash = makeHash("export-sigs-eq");
        BEAST_EXPECT(positionTxSetID(a) == positionTxSetID(b));

        // Same txSetHash, different participant diagnostics -> same tx-set key
        b.observedParticipantsHash = makeHash("participants-eq");
        BEAST_EXPECT(positionTxSetID(a) == positionTxSetID(b));

        // Different txSetHash -> different tx-set key
        ExtendedPosition c{txSet2};
        BEAST_EXPECT(positionTxSetID(a) != positionTxSetID(c));

        BEAST_EXPECT(positionTxSetID(a) == txSet);
        BEAST_EXPECT(positionTxSetID(a) != txSet2);

        a.updateTxSet(txSet2);
        BEAST_EXPECT(positionTxSetID(a) == txSet2);
        BEAST_EXPECT(positionTxSetID(a) != positionTxSetID(b));
    }

    void
    testExportSignatureDigest()
    {
        testcase("Export signature digest");

        //@@start test-extended-position-export-signature-digest
        std::vector<std::string> blobs;
        blobs.emplace_back("txhash-pubkey-sig-a");
        blobs.emplace_back("txhash-pubkey-sig-b");

        auto const digest = proposalExportSignaturesHash(blobs);
        BEAST_EXPECT(digest == proposalExportSignaturesHash(blobs));

        auto reordered = blobs;
        std::swap(reordered[0], reordered[1]);
        BEAST_EXPECT(digest != proposalExportSignaturesHash(reordered));

        auto mutated = blobs;
        mutated[1].push_back('x');
        BEAST_EXPECT(digest != proposalExportSignaturesHash(mutated));
        //@@end test-extended-position-export-signature-digest
    }

    void
    testStringJsonAndHash()
    {
        testcase("String, JSON, and hash helpers");

        auto const txSet = makeHash("txset-json");
        auto const commitSet = makeHash("commitset-json");
        auto const entropySet = makeHash("entropyset-json");
        auto const exportSigSet = makeHash("exportsigset-json");
        auto const exportSigs = makeHash("exportsigs-json");
        auto const participants = makeHash("participants-json");

        ExtendedPosition pos{txSet};
        pos.commitSetHash = commitSet;
        pos.entropySetHash = entropySet;
        pos.exportSigSetHash = exportSigSet;
        pos.exportSignaturesHash = exportSigs;
        pos.observedParticipantsHash = participants;

        BEAST_EXPECT(to_string(pos) == to_string(txSet));

        std::ostringstream os;
        os << pos;
        BEAST_EXPECT(os.str() == to_string(txSet));

        auto const json = pos.getJson();
        BEAST_EXPECT(json["tx_set"].asString() == to_string(txSet));
        BEAST_EXPECT(json["commit_set"].asString() == to_string(commitSet));
        BEAST_EXPECT(json["entropy_set"].asString() == to_string(entropySet));
        BEAST_EXPECT(
            json["export_sig_set"].asString() == to_string(exportSigSet));
        BEAST_EXPECT(
            json["export_signatures"].asString() == to_string(exportSigs));
        BEAST_EXPECT(
            json["observed_participants"].asString() ==
            to_string(participants));

        auto const fullHash = sha512Half(pos);
        auto withDifferentLeaf = pos;
        withDifferentLeaf.myCommitment = makeHash("commitment-json");
        BEAST_EXPECT(fullHash != sha512Half(withDifferentLeaf));

        auto sameTxDifferentSidecar = pos;
        sameTxDifferentSidecar.entropySetHash = makeHash("entropyset-other");
        BEAST_EXPECT(
            positionTxSetID(pos) == positionTxSetID(sameTxDifferentSidecar));
        BEAST_EXPECT(sha512Half(pos) != sha512Half(sameTxDifferentSidecar));
    }

public:
    void
    run() override
    {
        testSerializationRoundTrip();
        testSigningConsistency();
        testSuppressionConsistency();
        testPeerPosition();
        testMalformedPayload();
        testTxSetIdentity();
        testExportSignatureDigest();
        testStringJsonAndHash();
    }
};

BEAST_DEFINE_TESTSUITE(ExtendedPosition, consensus, ripple);

}  // namespace test
}  // namespace ripple
