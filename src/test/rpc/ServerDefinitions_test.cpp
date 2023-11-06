//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2016 Ripple Labs Inc.

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

#include <ripple/beast/unit_test.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

namespace ripple {

namespace test {

class ServerDefinitions_test : public beast::unit_test::suite
{
public:
    static Json::Value
    loadJson(std::string content)
    {
        // If the string is empty, return an empty Json::Value
        if (content.empty())
        {
            std::cout << "JSON string was empty"
                      << "\n";
            return {};
        }

        Json::Value jsonValue;
        Json::Reader reader;
        reader.parse(content, jsonValue);
        return jsonValue;
    }

    void
    testServerDefinitions()
    {
        using namespace test::jtx;

        std::string jsonLE = R"json({
            "AccountRoot" : 97,
            "Amendments" : 102,
            "Any" : 0,
            "Check" : 67,
            "Child" : 7378,
            "Contract" : 99,
            "DepositPreauth" : 112,
            "DirectoryNode" : 100,
            "EmittedTxn" : 69,
            "Escrow" : 117,
            "FeeSettings" : 115,
            "GeneratorMap" : 103,
            "Hook" : 72,
            "HookDefinition" : 68,
            "HookState" : 118,
            "ImportVLSequence" : 73,
            "Invalid" : -1,
            "LedgerHashes" : 104,
            "NFTokenOffer" : 55,
            "NFTokenPage" : 80,
            "NegativeUNL" : 78,
            "Nickname" : 110,
            "Offer" : 111,
            "PayChannel" : 120,
            "RippleState" : 114,
            "SignerList" : 83,
            "Ticket" : 84,
            "UNLReport" : 82,
            "URIToken" : 85
        })json";

        std::string jsonTT = R"json({
			"AccountDelete" : 21,
			"AccountSet" : 3,
			"Amendment" : 100,
			"CheckCancel" : 18,
			"CheckCash" : 17,
			"CheckCreate" : 16,
			"ClaimReward" : 98,
			"Contract" : 9,
			"DepositPreauth" : 19,
			"EmitFailure" : 103,
			"EscrowCancel" : 4,
			"EscrowCreate" : 1,
			"EscrowFinish" : 2,
			"Fee" : 101,
			"GenesisMint" : 96,
			"Import" : 97,
			"Invalid" : -1,
			"Invoke" : 99,
			"NFTokenAcceptOffer" : 29,
			"NFTokenBurn" : 26,
			"NFTokenCancelOffer" : 28,
			"NFTokenCreateOffer" : 27,
			"NFTokenMint" : 25,
			"NicknameSet" : 6,
			"OfferCancel" : 8,
			"OfferCreate" : 7,
			"Payment" : 0,
			"PaymentChannelClaim" : 15,
			"PaymentChannelCreate" : 13,
			"PaymentChannelFund" : 14,
			"SetHook" : 22,
			"SetRegularKey" : 5,
			"SignerListSet" : 12,
			"SpinalTap" : 11,
			"TicketCreate" : 10,
			"TrustSet" : 20,
			"UNLModify" : 102,
			"UNLReport" : 104,
			"URITokenBurn" : 46,
			"URITokenBuy" : 47,
			"URITokenCancelSellOffer" : 49,
			"URITokenCreateSellOffer" : 48,
			"URITokenMint" : 45
		})json";

        std::string jsonTY = R"json({
			"AccountID" : 8,
			"Amount" : 6,
			"Blob" : 7,
			"Done" : -1,
			"Hash128" : 4,
			"Hash160" : 17,
			"Hash256" : 5,
			"LedgerEntry" : 10002,
			"Metadata" : 10004,
			"NotPresent" : 0,
			"PathSet" : 18,
			"STArray" : 15,
			"STObject" : 14,
			"Transaction" : 10001,
			"UInt16" : 1,
			"UInt192" : 21,
			"UInt32" : 2,
			"UInt384" : 22,
			"UInt512" : 23,
			"UInt64" : 3,
			"UInt8" : 16,
			"UInt96" : 20,
			"Unknown" : -2,
			"Validation" : 10003,
			"Vector256" : 19
		})json";

        std::string jsonTEC = R"json({
			"tecAMM_BALANCE" : 163,
			"tecAMM_FAILED_BID" : 167,
			"tecAMM_FAILED_DEPOSIT" : 164,
			"tecAMM_FAILED_VOTE" : 168,
			"tecAMM_FAILED_WITHDRAW" : 165,
			"tecAMM_INVALID_TOKENS" : 166,
			"tecAMM_UNFUNDED" : 162,
			"tecBAD_XCHAIN_TRANSFER_ISSUE" : 171,
			"tecCANT_ACCEPT_OWN_NFTOKEN_OFFER" : 158,
			"tecCLAIM" : 100,
			"tecCRYPTOCONDITION_ERROR" : 146,
			"tecDIR_FULL" : 121,
			"tecDST_TAG_NEEDED" : 143,
			"tecDUPLICATE" : 149,
			"tecEXPIRED" : 148,
			"tecFAILED_PROCESSING" : 105,
			"tecFROZEN" : 137,
			"tecHAS_OBLIGATIONS" : 151,
			"tecHOOK_REJECTED" : 153,
			"tecINSUFFICIENT_FUNDS" : 159,
			"tecINSUFFICIENT_PAYMENT" : 161,
			"tecINSUFFICIENT_RESERVE" : 141,
			"tecINSUFF_FEE" : 136,
			"tecINSUF_RESERVE_LINE" : 122,
			"tecINSUF_RESERVE_OFFER" : 123,
			"tecINTERNAL" : 144,
			"tecINVARIANT_FAILED" : 147,
			"tecKILLED" : 150,
			"tecLAST_POSSIBLE_ENTRY" : 255,
			"tecMAX_SEQUENCE_REACHED" : 154,
			"tecNEED_MASTER_KEY" : 142,
			"tecNFTOKEN_BUY_SELL_MISMATCH" : 156,
			"tecNFTOKEN_OFFER_TYPE_MISMATCH" : 157,
			"tecNO_ALTERNATIVE_KEY" : 130,
			"tecNO_AUTH" : 134,
			"tecNO_DST" : 124,
			"tecNO_DST_INSUF_XRP" : 125,
			"tecNO_ENTRY" : 140,
			"tecNO_ISSUER" : 133,
			"tecNO_LINE" : 135,
			"tecNO_LINE_INSUF_RESERVE" : 126,
			"tecNO_LINE_REDUNDANT" : 127,
			"tecNO_PERMISSION" : 139,
			"tecNO_REGULAR_KEY" : 131,
			"tecNO_SUITABLE_NFTOKEN_PAGE" : 155,
			"tecNO_TARGET" : 138,
			"tecOBJECT_NOT_FOUND" : 160,
			"tecOVERSIZE" : 145,
			"tecOWNERS" : 132,
			"tecPATH_DRY" : 128,
			"tecPATH_PARTIAL" : 101,
			"tecPRECISION_LOSS" : 170,
			"tecREQUIRES_FLAG" : 169,
			"tecTOO_SOON" : 152,
			"tecUNFUNDED" : 129,
			"tecUNFUNDED_ADD" : 102,
			"tecUNFUNDED_OFFER" : 103,
			"tecUNFUNDED_PAYMENT" : 104,
			"tecXCHAIN_ACCOUNT_CREATE_PAST" : 182,
			"tecXCHAIN_ACCOUNT_CREATE_TOO_MANY" : 183,
			"tecXCHAIN_BAD_CLAIM_ID" : 173,
			"tecXCHAIN_BAD_PUBLIC_KEY_ACCOUNT_PAIR" : 186,
			"tecXCHAIN_CLAIM_NO_QUORUM" : 174,
			"tecXCHAIN_CREATE_ACCOUNT_NONXRP_ISSUE" : 176,
			"tecXCHAIN_INSUFF_CREATE_AMOUNT" : 181,
			"tecXCHAIN_NO_CLAIM_ID" : 172,
			"tecXCHAIN_NO_SIGNERS_LIST" : 179,
			"tecXCHAIN_PAYMENT_FAILED" : 184,
			"tecXCHAIN_PROOF_UNKNOWN_KEY" : 175,
			"tecXCHAIN_REWARD_MISMATCH" : 178,
			"tecXCHAIN_SELF_COMMIT" : 185,
			"tecXCHAIN_SENDING_ACCOUNT_MISMATCH" : 180,
			"tecXCHAIN_WRONG_CHAIN" : 177,
			"tefALREADY" : -198,
			"tefBAD_ADD_AUTH" : -197,
			"tefBAD_AUTH" : -196,
			"tefBAD_AUTH_MASTER" : -183,
			"tefBAD_LEDGER" : -195,
			"tefBAD_QUORUM" : -185,
			"tefBAD_SIGNATURE" : -186,
			"tefCREATED" : -194,
			"tefEXCEPTION" : -193,
			"tefFAILURE" : -199,
			"tefINTERNAL" : -192,
			"tefINVARIANT_FAILED" : -182,
			"tefMASTER_DISABLED" : -188,
			"tefMAX_LEDGER" : -187,
			"tefNFTOKEN_IS_NOT_TRANSFERABLE" : -179,
			"tefNOT_MULTI_SIGNING" : -184,
			"tefNO_AUTH_REQUIRED" : -191,
			"tefNO_TICKET" : -180,
			"tefPAST_IMPORT_SEQ" : -178,
			"tefPAST_IMPORT_VL_SEQ" : -177,
			"tefPAST_SEQ" : -190,
			"tefTOO_BIG" : -181,
			"tefWRONG_PRIOR" : -189,
			"telBAD_DOMAIN" : -398,
			"telBAD_PATH_COUNT" : -397,
			"telBAD_PUBLIC_KEY" : -396,
			"telCAN_NOT_QUEUE" : -392,
			"telCAN_NOT_QUEUE_BALANCE" : -391,
			"telCAN_NOT_QUEUE_BLOCKED" : -389,
			"telCAN_NOT_QUEUE_BLOCKS" : -390,
			"telCAN_NOT_QUEUE_FEE" : -388,
			"telCAN_NOT_QUEUE_FULL" : -387,
			"telCAN_NOT_QUEUE_IMPORT" : -381,
			"telFAILED_PROCESSING" : -395,
			"telIMPORT_VL_KEY_NOT_RECOGNISED" : -382,
			"telINSUF_FEE_P" : -394,
			"telLOCAL_ERROR" : -399,
			"telNETWORK_ID_MAKES_TX_NON_CANONICAL" : -384,
			"telNON_LOCAL_EMITTED_TXN" : -383,
			"telNO_DST_PARTIAL" : -393,
			"telREQUIRES_NETWORK_ID" : -385,
			"telWRONG_NETWORK" : -386,
			"temAMM_BAD_TOKENS" : -261,
			"temBAD_AMOUNT" : -298,
			"temBAD_CURRENCY" : -297,
			"temBAD_EXPIRATION" : -296,
			"temBAD_FEE" : -295,
			"temBAD_ISSUER" : -294,
			"temBAD_LIMIT" : -293,
			"temBAD_NFTOKEN_TRANSFER_FEE" : -262,
			"temBAD_OFFER" : -292,
			"temBAD_PATH" : -291,
			"temBAD_PATH_LOOP" : -290,
			"temBAD_QUORUM" : -271,
			"temBAD_REGKEY" : -289,
			"temBAD_SEND_XRP_LIMIT" : -288,
			"temBAD_SEND_XRP_MAX" : -287,
			"temBAD_SEND_XRP_NO_DIRECT" : -286,
			"temBAD_SEND_XRP_PARTIAL" : -285,
			"temBAD_SEND_XRP_PATHS" : -284,
			"temBAD_SEQUENCE" : -283,
			"temBAD_SIGNATURE" : -282,
			"temBAD_SIGNER" : -272,
			"temBAD_SRC_ACCOUNT" : -281,
			"temBAD_TICK_SIZE" : -269,
			"temBAD_TRANSFER_RATE" : -280,
			"temBAD_WEIGHT" : -270,
			"temCANNOT_PREAUTH_SELF" : -267,
			"temDISABLED" : -273,
			"temDST_IS_SRC" : -279,
			"temDST_NEEDED" : -278,
			"temHOOK_DATA_TOO_LARGE" : -253,
			"temHOOK_REJECTED" : -252,
			"temINVALID" : -277,
			"temINVALID_ACCOUNT_ID" : -268,
			"temINVALID_COUNT" : -266,
			"temINVALID_FLAG" : -276,
			"temMALFORMED" : -299,
			"temREDUNDANT" : -275,
			"temRIPPLE_EMPTY" : -274,
			"temSEQ_AND_TICKET" : -263,
			"temUNCERTAIN" : -265,
			"temUNKNOWN" : -264,
			"temXCHAIN_BAD_PROOF" : -259,
			"temXCHAIN_BRIDGE_BAD_ISSUES" : -258,
			"temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT" : -256,
			"temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT" : -255,
			"temXCHAIN_BRIDGE_NONDOOR_OWNER" : -257,
			"temXCHAIN_EQUAL_DOOR_ACCOUNTS" : -260,
			"temXCHAIN_TOO_MANY_ATTESTATIONS" : -254,
			"terFUNDS_SPENT" : -98,
			"terINSUF_FEE_B" : -97,
			"terLAST" : -91,
			"terNO_ACCOUNT" : -96,
			"terNO_AMM" : -87,
			"terNO_AUTH" : -95,
			"terNO_HOOK" : -86,
			"terNO_LINE" : -94,
			"terNO_RIPPLE" : -90,
			"terOWNERS" : -93,
			"terPRE_SEQ" : -92,
			"terPRE_TICKET" : -88,
			"terQUEUED" : -89,
			"terRETRY" : -99,
			"tesSUCCESS" : 0
		})json";

        {
            Env env(*this);
            auto const result = env.rpc("server_definitions");
            BEAST_EXPECT(!result[jss::result].isMember(jss::error));
            // FIELDS
            // DA: Not Tested
            // LEDGER ENTRY TYPES
            BEAST_EXPECT(
                result[jss::result]["LEDGER_ENTRY_TYPES"] == loadJson(jsonLE));
            // TRANSACTION TYPES
            BEAST_EXPECT(
                result[jss::result]["TRANSACTION_TYPES"] == loadJson(jsonTT));
            // TYPES
            BEAST_EXPECT(result[jss::result]["TYPES"] == loadJson(jsonTY));
            // TRANSACTION_RESULTS
            BEAST_EXPECT(
                result[jss::result]["TRANSACTION_RESULTS"] ==
                loadJson(jsonTEC));
            BEAST_EXPECT(result[jss::result][jss::status] == "success");
        }
    }

    void
    run() override
    {
        testServerDefinitions();
    }
};

BEAST_DEFINE_TESTSUITE(ServerDefinitions, rpc, ripple);

}  // namespace test
}  // namespace ripple
