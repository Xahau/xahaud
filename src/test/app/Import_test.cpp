//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPLF

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

/*
DENIS NOTES

// DA: failed to deserialize tx blob/meta inside xpop (invalid SerialIter
getRaw) outer txid:

*/

#include <ripple/core/ConfigSections.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <ripple/ledger/Directory.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Import.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

namespace ripple {
namespace test {

class Import_test : public beast::unit_test::suite
{
    std::unique_ptr<Config>
    makeNetworkConfig(uint32_t networkID)
    {
        using namespace jtx;
        std::vector<std::string> const keys = {
            "ED74D4036C6591A4BDF9C54CEFA39B996A"
            "5DCE5F86D11FDA1874481CE9D5A1CDC1"};
        return envconfig([&](std::unique_ptr<Config> cfg) {
            cfg->section(SECTION_RPC_STARTUP)
                .append(
                    "{ \"command\": \"log_level\", \"severity\": \"warning\" "
                    "}");
            cfg->NETWORK_ID = networkID;
            cfg->IMPORT_VL_KEYS = keys;
            return cfg;
        });
    }

    static Json::Value
    import(jtx::Account const& account, Json::Value const& xpop)
    {
        using namespace jtx;
        Json::Value jv;
        std::string strJson = Json::FastWriter().write(xpop);
        jv[jss::TransactionType] = jss::Import;
        jv[jss::Account] = account.human();
        jv[jss::Fee] = 0;
        jv[sfBlob.jsonName] = strHex(strJson);
        return jv;
    }

    static std::pair<uint256, std::shared_ptr<SLE const>>
    accountKeyAndSle(ReadView const& view, jtx::Account const& account)
    {
        auto const k = keylet::account(account);
        return {k.key, view.read(k)};
    }

    static std::pair<uint256, std::shared_ptr<SLE const>>
    signersKeyAndSle(ReadView const& view, jtx::Account const& account)
    {
        auto const k = keylet::signers(account);
        return {k.key, view.read(k)};
    }

    static std::size_t
    ownerDirCount(ReadView const& view, jtx::Account const& acct)
    {
        ripple::Dir const ownerDir(view, keylet::ownerDir(acct.id()));
        return std::distance(ownerDir.begin(), ownerDir.end());
    };

    static Json::Value
    accountSetXpop()
    {
        std::string strJson = R"json({
            "ledger": {
                "acroot": "4300DFEC01D20F18FB8AD86A1A4EC619A5F3A5C248F3A91913C0EF04D8F29734",
                "close": 739431052,
                "coins": "99999998999999844",
                "cres": 10,
                "flags": 0,
                "index": 41,
                "pclose": 739431051,
                "phash": "B8DB71F81667151FDD00CCADB209A777371A18F8F2BE9ADED7345D210CC5FDC1",
                "txroot": "CD68159512E84E0DE430AA685A7574C57AACD5FB6DA511CB9B8F4276CFAB53B5"
            },
            "transaction": {
                "blob": "12000322000000002400000002201B0000003B201D0000535968400000003B9ACA0073210388935426E0D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97C05274473045022100E3E45165E99720E0D9CA2F1101E619DC29F78DE310DEC25604A24F47D671860B02207626E36D9E5DBBB5BD33F892B0C1DF419A2E4511F164346341220627EB7FB41F8114AE123A8556F3CF91154711376AFB0F894F832B3D",
                "meta": "201C00000005F8E5110061250000002955E110681DFD389BE1F7D3A1A08919C3FD16ABA78EE6A86FD7F8A2CD7512E0CBE05692FA6A9FC8EA6018D5D16532D7795C91BFB0831355BDFDA177E86C8BF997985FE624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F48114AE123A8556F3CF91154711376AFB0F894F832B3DE1E1F1031000",
                "proof": {
                "children": {
                    "0": {
                    "children": {},
                    "hash": "D73E14195A5FE5C00E71FAD095AF5EF2C1B7CEC47EF45F6751B84972C1956FF6",
                    "key": "05137BD9679EDF737EE6CAA69427D243C53AE6945C97E41C462AE647D25B0B12"
                    },
                    "3": {
                    "children": {},
                    "hash": "00D9FDA1C18A1B3950750A43F6649BE81F06C565AE6513BB14F285A2DE7A9953",
                    "key": "38EF03286B154F56882CF62F6AFB3B94624E6089040562E965A8243FF7397A7B"
                    },
                    "4": {
                    "children": {},
                    "hash": "32A91095374D7A4239301B5CE7F0A9066986913CFACF29E1B6C3CD07DED921F6",
                    "key": "47D5B86F48B38FBB12E570778AA5FBD8126D2C6A452F135424E7061FA1C5CF42"
                    },
                    "5": {
                    "children": {},
                    "hash": "361C68190AB24629D15F44D13EFF57CC0A9CA6826D24DD840749ECDB4D8DC096",
                    "key": "55616BC25786E929DC7B163F2DABAA17DEF3D4FC13D8BF484AA70022AE38954C"
                    },
                    "7": {
                    "children": {},
                    "hash": "6691A9F3EECDB5C1D4B9736DF300FFC22C3138DAD64D0D77C93A56CC5217682F",
                    "key": "77AB1364F7C6D1C92A42CE737D5A5C749984AFD267230A69A05724602F92DD6A"
                    },
                    "8": {
                    "children": {},
                    "hash": "B0D13F5F9B7BA4E5963715BBCA6CD76A4A85AF3643784C1EC065E1325852E4F9",
                    "key": "895F910AC4C59E4138623432F103A92E2092D458B020296715A7C86946BD0224"
                    },
                    "A": {
                    "children": {},
                    "hash": "50B49F162EB2D8EA2DE6646ED7B75C4AC181A8C281BFAE62FF9EB38BC26E4F87",
                    "key": "AA8EB7038D77837CF9055B2AF13B1AACCCEDA792B9A07732549646925A97F462"
                    },
                    "D": {
                    "children": {},
                    "hash": "348754862F6E8ED4D23CA346FA53DF311375D42A3BA20C33D927FB74DF5DBC56",
                    "key": "D146454960D401B9AB4C6DE32F8E9A9186765EFE76EE611B4A0D18D69242D51E"
                    },
                    "E": {
                    "children": {
                        "1": {
                        "children": {},
                        "hash": "4FA153CE29ECA36803CAC28CF27DEC73491398B64032E7030F8219179CDADCEA",
                        "key": "E110681DFD389BE1F7D3A1A08919C3FD16ABA78EE6A86FD7F8A2CD7512E0CBE0"
                        },
                        "F": {
                        "children": {},
                        "hash": "0B316915D2F11B82EE9402A7C686C5AD576510C1C209AA36506A4AF5DED0DDA9",
                        "key": "EFFD664A294E07A5BF939E8A2CAF19C54BB11949FCD1D28D74C3862A50CCBEE3"
                        }
                    },
                    "hash": "0D7AC4E4D9BCBB51C279EC171A2FA672045A79FB1564B3ED87DAE809CD859148",
                    "key": "E000000000000000000000000000000000000000000000000000000000000000"
                    },
                    "F": {
                    "children": {
                        "1": {
                        "children": {},
                        "hash": "15AFD9F20741B8768EF4E8B4DF351F777E3660EA71F399A66C30F6A5D42FFDFB",
                        "key": "F11B47F322D2D1EFFA6CB6F7D3BEFE50AE152B87014CCF840886DB915FA40A13"
                        },
                        "3": {
                        "children": {},
                        "hash": "B663FD9DE6B4C98A4803C2ADAFA1590EF0EC8E1F9994059FF055890230BCA4EC",
                        "key": "F34B5CEA1C4CDEE7B4C8788B0F667C4921A555E86FC23768E51DE880924CBBD6"
                        },
                        "A": {
                        "children": {},
                        "hash": "0610257AAFD2B633AF9DB57CF4BAC521D3D6CD62357340A121B875442D0E9926",
                        "key": "FADD7A23E29B443A0F4B35B7CB0A17DACE13CA1CEAA08AF95B251701AAD81751"
                        },
                        "F": {
                        "children": {},
                        "hash": "F07D692BF72C72F5CC61F6FED04737A7C4E0CC8EBA0F52051C736B4FF233E9EC",
                        "key": "FFF1E36E035C347F06C0A54DA45981CB8ABA91EEDB82D7C61E3952F698769D2B"
                        }
                    },
                    "hash": "96AC68A145A7229869267AEF3D6AA4C2C56C30184CE3BB5C20FCC11161F7A49B",
                    "key": "F000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "hash": "CD68159512E84E0DE430AA685A7574C57AACD5FB6DA511CB9B8F4276CFAB53B5",
                "key": "0000000000000000000000000000000000000000000000000000000000000000"
                }
            },
            "validation": {
                "data": {
                "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000029292C12D28E514F1F56EB808B9DA5564AABB7019D3EA1692C3C5DFA49E975D56CC48963E941A25017F878F54DA3C37C554E7B5DF952E1BD1181A4D63CA05F1D6751DD1A129C4CEF4A732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100F7BA251EBE7AC9DFC6C95BD6BA6F1372F09E90BCDC302D73E6AD5D32CA2FAA1702201234FF330B5F1C5AB53D67C50396BEDC20389F8BB9A71158FBAA2F937FB98528",
                "n9KXYzdZD8YpsNiChtMjP6yhvQAhkkh5XeSTbvYyV1waF8wkNnBT": "22800000012600000029292C12D28E514F1F56EB808B9DA5564AABB7019D3EA1692C3C5DFA49E975D56CC48963E941A25017F878F54DA3C37C554E7B5DF952E1BD1181A4D63CA05F1D6751DD1A129C4CEF4A732102818C35CB205143702A8FA1EF385A5CA17E1C6C3A5E1123A032AB6A59189CC8CA76473045022100BC6CD6BE325712336C769D996F11107D145ED6732E7484B13FA8632752B3D9DF022017ABD68DC746273B2B175AACFCBB24EA1CD7DDA82D5FAA5C5BC4B054D5E94D79"
                },
                "unl": {
                "blob": "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9DdFNBND0ifV19",
                "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                "signature": "849F6B8DA6E11C213B561659C16F13D35385E8EA9E775483ADC84578F6D578943DE5EB681584B2C03EFFFDFD216F9E0B21576E482F941C7195893B72B5B1F70D",
                "version": 1
                }
            }
        })json";
        Json::Value xpop;
        Json::Reader reader;
        reader.parse(strJson, xpop);
        return xpop;
    }

    static Json::Value
    setRegularKeyXpop()
    {
        std::string strJson = R"json({
            "ledger": {
                "acroot": "A8AAC3B6BE8027650DCA96F5FC59D2BCB961E43B8E69554DCDC3E7D5546BD973",
                "close": 739435131,
                "coins": "99999998999999832",
                "cres": 10,
                "flags": 0,
                "index": 668,
                "pclose": 739435130,
                "phash": "E153F8A1286AAC8A3F0DA26DACCF52C2760FEA4DEB572CD2CEF0818618E84FA6",
                "txroot": "2D27060F123E5D8D96AC3ADB5F21D62CE58EF193A5EE1FAE5649D682AF490CD5"
            },
            "transaction": {
                "blob": "12000522000000002400000005201B000002AF201D0000535968400000000000000C73210388935426E0D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97C0527446304402205FC70E12E5638B25BD9662F22C16A3710247862CE06FC9E8D7E80DD1B97A1F5102204752F43F2F835748822EA74DA9B5AFB28BF8DDE7F8E88EDE2C72654364F5919A8114AE123A8556F3CF91154711376AFB0F894F832B3D8814AE123A8556F3CF91154711376AFB0F894F832B3D",
                "meta": "201C00000000F8E511006125000000295505137BD9679EDF737EE6CAA69427D243C53AE6945C97E41C462AE647D25B0B125692FA6A9FC8EA6018D5D16532D7795C91BFB0831355BDFDA177E86C8BF997985FE6240000000562400000003B9AC9DCE1E7220001000024000000062D0000000462400000003B9AC9D08114AE123A8556F3CF91154711376AFB0F894F832B3D8814AE123A8556F3CF91154711376AFB0F894F832B3DE1E1F1031000",
                "proof": {
                "children": {
                    "1": {
                    "children": {},
                    "hash": "98397F90D1A82985D12E58DDCF831F3D0B0299A920B2E5D51EC529D22CF02906",
                    "key": "194CB1D80535AE884C7E3CAE200267F68B03BBCD0137C391D9F3B5499B11B6D3"
                    }
                },
                "hash": "2D27060F123E5D8D96AC3ADB5F21D62CE58EF193A5EE1FAE5649D682AF490CD5",
                "key": "0000000000000000000000000000000000000000000000000000000000000000"
                }
            },
            "validation": {
                "data": {
                "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "2280000001260000029C292C12E27B51A19C5772B28B6A73A5DFD27FA933622CA24475C1809E5A79C7C9953340D5FD9D5017CF34CE9E06B20CFC46167FA254D3C2245F64C702E7B889A82315E41B2E0AED74732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B7646304402201F9E79949D6E122EAE09EE39913F69F792C4FCA9D6B00D13C85533D9C188848002206AC93CCF23B1B6B2E97A5E021E267EF635DE1F242DEABAC5A699534C2D53F463",
                "n9KXYzdZD8YpsNiChtMjP6yhvQAhkkh5XeSTbvYyV1waF8wkNnBT": "2280000001260000029C292C12E27B51A19C5772B28B6A73A5DFD27FA933622CA24475C1809E5A79C7C9953340D5FD9D5017CF34CE9E06B20CFC46167FA254D3C2245F64C702E7B889A82315E41B2E0AED74732102818C35CB205143702A8FA1EF385A5CA17E1C6C3A5E1123A032AB6A59189CC8CA76473045022100B93F0FAC8C9832F6CF67E0404B775BCCA205431616A221190666517DE3A39D9902206E244EFA5EDABE4B72BCF3C973C475B784BED3D44E4B00837580DC23B41129E1"
                },
                "unl": {
                "blob": "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9DdFNBND0ifV19",
                "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                "signature": "849F6B8DA6E11C213B561659C16F13D35385E8EA9E775483ADC84578F6D578943DE5EB681584B2C03EFFFDFD216F9E0B21576E482F941C7195893B72B5B1F70D",
                "version": 1
                }
            }
        })json";
        Json::Value xpop;
        Json::Reader reader;
        reader.parse(strJson, xpop);
        return xpop;
    }

    static Json::Value
    signersListSetXpop()
    {
        std::string strJson = R"json({
            "ledger": {
                "acroot": "4300DFEC01D20F18FB8AD86A1A4EC619A5F3A5C248F3A91913C0EF04D8F29734",
                "close": 739431052,
                "coins": "99999998999999844",
                "cres": 10,
                "flags": 0,
                "index": 41,
                "pclose": 739431051,
                "phash": "B8DB71F81667151FDD00CCADB209A777371A18F8F2BE9ADED7345D210CC5FDC1",
                "txroot": "CD68159512E84E0DE430AA685A7574C57AACD5FB6DA511CB9B8F4276CFAB53B5"
            },
            "transaction": {
                "blob": "12000C22000000002400000004201B0000003B201D0000535920230000000168400000000000000C73210388935426E0D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97C05274473045022100E462E53C592D6756968203CB4D146539BA2B397BCE5909890592E9AB7AB63204022061895689FD8FAA3CA1E1072554DB65E873BB586ECB5B7A04789D2BEC0785E3008114AE123A8556F3CF91154711376AFB0F894F832B3DF4EB1300018114F51DFC2A09D62CBBA1DFBDD4691DAC96AD98B90FE1F1",
                "meta": "201C00000007F8E311005356472CD116F1449F280243169C442271168E368750479CC7B20816170EDBDCA4E6E8202300000001F4EB1300018114F51DFC2A09D62CBBA1DFBDD4691DAC96AD98B90FE1F1E1E1E5110061250000002955F11B47F322D2D1EFFA6CB6F7D3BEFE50AE152B87014CCF840886DB915FA40A135692FA6A9FC8EA6018D5D16532D7795C91BFB0831355BDFDA177E86C8BF997985FE624000000042D0000000162400000003B9AC9E8E1E7220001000024000000052D0000000462400000003B9AC9DC8114AE123A8556F3CF91154711376AFB0F894F832B3D8814AE123A8556F3CF91154711376AFB0F894F832B3DE1E1E511006456A33EC6BB85FB5674074C4A3A43373BB17645308F3EAE1933E3E35252162B217DE7220000000058A33EC6BB85FB5674074C4A3A43373BB17645308F3EAE1933E3E35252162B217D8214AE123A8556F3CF91154711376AFB0F894F832B3DE1E1F1031000",
                "proof": {
                "children": {
                    "0": {
                    "children": {},
                    "hash": "D73E14195A5FE5C00E71FAD095AF5EF2C1B7CEC47EF45F6751B84972C1956FF6",
                    "key": "05137BD9679EDF737EE6CAA69427D243C53AE6945C97E41C462AE647D25B0B12"
                    },
                    "3": {
                    "children": {},
                    "hash": "00D9FDA1C18A1B3950750A43F6649BE81F06C565AE6513BB14F285A2DE7A9953",
                    "key": "38EF03286B154F56882CF62F6AFB3B94624E6089040562E965A8243FF7397A7B"
                    },
                    "4": {
                    "children": {},
                    "hash": "32A91095374D7A4239301B5CE7F0A9066986913CFACF29E1B6C3CD07DED921F6",
                    "key": "47D5B86F48B38FBB12E570778AA5FBD8126D2C6A452F135424E7061FA1C5CF42"
                    },
                    "5": {
                    "children": {},
                    "hash": "361C68190AB24629D15F44D13EFF57CC0A9CA6826D24DD840749ECDB4D8DC096",
                    "key": "55616BC25786E929DC7B163F2DABAA17DEF3D4FC13D8BF484AA70022AE38954C"
                    },
                    "7": {
                    "children": {},
                    "hash": "6691A9F3EECDB5C1D4B9736DF300FFC22C3138DAD64D0D77C93A56CC5217682F",
                    "key": "77AB1364F7C6D1C92A42CE737D5A5C749984AFD267230A69A05724602F92DD6A"
                    },
                    "8": {
                    "children": {},
                    "hash": "B0D13F5F9B7BA4E5963715BBCA6CD76A4A85AF3643784C1EC065E1325852E4F9",
                    "key": "895F910AC4C59E4138623432F103A92E2092D458B020296715A7C86946BD0224"
                    },
                    "A": {
                    "children": {},
                    "hash": "50B49F162EB2D8EA2DE6646ED7B75C4AC181A8C281BFAE62FF9EB38BC26E4F87",
                    "key": "AA8EB7038D77837CF9055B2AF13B1AACCCEDA792B9A07732549646925A97F462"
                    },
                    "D": {
                    "children": {},
                    "hash": "348754862F6E8ED4D23CA346FA53DF311375D42A3BA20C33D927FB74DF5DBC56",
                    "key": "D146454960D401B9AB4C6DE32F8E9A9186765EFE76EE611B4A0D18D69242D51E"
                    },
                    "E": {
                    "children": {
                        "1": {
                        "children": {},
                        "hash": "4FA153CE29ECA36803CAC28CF27DEC73491398B64032E7030F8219179CDADCEA",
                        "key": "E110681DFD389BE1F7D3A1A08919C3FD16ABA78EE6A86FD7F8A2CD7512E0CBE0"
                        },
                        "F": {
                        "children": {},
                        "hash": "0B316915D2F11B82EE9402A7C686C5AD576510C1C209AA36506A4AF5DED0DDA9",
                        "key": "EFFD664A294E07A5BF939E8A2CAF19C54BB11949FCD1D28D74C3862A50CCBEE3"
                        }
                    },
                    "hash": "0D7AC4E4D9BCBB51C279EC171A2FA672045A79FB1564B3ED87DAE809CD859148",
                    "key": "E000000000000000000000000000000000000000000000000000000000000000"
                    },
                    "F": {
                    "children": {
                        "1": {
                        "children": {},
                        "hash": "15AFD9F20741B8768EF4E8B4DF351F777E3660EA71F399A66C30F6A5D42FFDFB",
                        "key": "F11B47F322D2D1EFFA6CB6F7D3BEFE50AE152B87014CCF840886DB915FA40A13"
                        },
                        "3": {
                        "children": {},
                        "hash": "B663FD9DE6B4C98A4803C2ADAFA1590EF0EC8E1F9994059FF055890230BCA4EC",
                        "key": "F34B5CEA1C4CDEE7B4C8788B0F667C4921A555E86FC23768E51DE880924CBBD6"
                        },
                        "A": {
                        "children": {},
                        "hash": "0610257AAFD2B633AF9DB57CF4BAC521D3D6CD62357340A121B875442D0E9926",
                        "key": "FADD7A23E29B443A0F4B35B7CB0A17DACE13CA1CEAA08AF95B251701AAD81751"
                        },
                        "F": {
                        "children": {},
                        "hash": "F07D692BF72C72F5CC61F6FED04737A7C4E0CC8EBA0F52051C736B4FF233E9EC",
                        "key": "FFF1E36E035C347F06C0A54DA45981CB8ABA91EEDB82D7C61E3952F698769D2B"
                        }
                    },
                    "hash": "96AC68A145A7229869267AEF3D6AA4C2C56C30184CE3BB5C20FCC11161F7A49B",
                    "key": "F000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "hash": "CD68159512E84E0DE430AA685A7574C57AACD5FB6DA511CB9B8F4276CFAB53B5",
                "key": "0000000000000000000000000000000000000000000000000000000000000000"
                }
            },
            "validation": {
                "data": {
                "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000029292C12D28E514F1F56EB808B9DA5564AABB7019D3EA1692C3C5DFA49E975D56CC48963E941A25017F878F54DA3C37C554E7B5DF952E1BD1181A4D63CA05F1D6751DD1A129C4CEF4A732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100F7BA251EBE7AC9DFC6C95BD6BA6F1372F09E90BCDC302D73E6AD5D32CA2FAA1702201234FF330B5F1C5AB53D67C50396BEDC20389F8BB9A71158FBAA2F937FB98528",
                "n9KXYzdZD8YpsNiChtMjP6yhvQAhkkh5XeSTbvYyV1waF8wkNnBT": "22800000012600000029292C12D28E514F1F56EB808B9DA5564AABB7019D3EA1692C3C5DFA49E975D56CC48963E941A25017F878F54DA3C37C554E7B5DF952E1BD1181A4D63CA05F1D6751DD1A129C4CEF4A732102818C35CB205143702A8FA1EF385A5CA17E1C6C3A5E1123A032AB6A59189CC8CA76473045022100BC6CD6BE325712336C769D996F11107D145ED6732E7484B13FA8632752B3D9DF022017ABD68DC746273B2B175AACFCBB24EA1CD7DDA82D5FAA5C5BC4B054D5E94D79"
                },
                "unl": {
                "blob": "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9DdFNBND0ifV19",
                "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                "signature": "849F6B8DA6E11C213B561659C16F13D35385E8EA9E775483ADC84578F6D578943DE5EB681584B2C03EFFFDFD216F9E0B21576E482F941C7195893B72B5B1F70D",
                "version": 1
                }
            }
        })json";

        Json::Value xpop;
        Json::Reader reader;
        reader.parse(strJson, xpop);
        return xpop;
    }

    void
    testIsHex(FeatureBitset features)
    {
        testcase("import utils - isHex");
        bool const result = isHex("DEADBEEF");
        BEAST_EXPECT(result == true);
        bool const result1 = isHex("Hello world");
        BEAST_EXPECT(result1 == false);
    }

    void
    testIsBase58(FeatureBitset features)
    {
        testcase("import utils - isBase58");
        bool const result = isBase58("hE45rTtDcGvFz");
        BEAST_EXPECT(result == true);
        bool const result1 = isBase58("Hello world");
        BEAST_EXPECT(result1 == false);
    }

    void
    testIsBase64(FeatureBitset features)
    {
        testcase("import utils - isBase64");
        bool const result = isBase64("SGVsbG8gV29ybGQh");
        BEAST_EXPECT(result == true);
        bool const result1 = isBase64("Hello world");
        BEAST_EXPECT(result1 == false);
    }

    void
    testParseUint64(FeatureBitset features)
    {
        testcase("import utils - parseUint64");
        std::optional<uint64_t> result1 = parse_uint64("1234");
        BEAST_EXPECT(result1 == 1234);

        // std::optional<uint64_t> result2 = parse_uint64("0xFFAABBCCDD22");
        // std::cout << "RESULT: " << *result2 << "\n";
        // BEAST_EXPECT(result2 == 0xFFAABBCCDD22);

        std::optional<uint64_t> result3 = parse_uint64("2147483647");
        BEAST_EXPECT(result3 == 2147483647);

        std::optional<uint64_t> result4 = parse_uint64("18446744073709551615");
        BEAST_EXPECT(result4 == 18446744073709551615ull);
    }

    void
    testSyntaxCheckProofArray(FeatureBitset features)
    {
    }
    void
    testSyntaxCheckProofObject(FeatureBitset features)
    {
        testcase("import utils - syntaxCheckProof: is object");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(11111)};

        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        std::string strJson = R"json({
            "children": {
                "3": {
                    "children": {},
                    "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                    "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                }
            },
            "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
            "key": "0000000000000000000000000000000000000000000000000000000000000000"
        })json";

        Json::Value proof;
        Json::Reader reader;
        reader.parse(strJson, proof);

        // XPOP.transaction.proof list entry has wrong format
        {
            BEAST_EXPECT(syntaxCheckProof(proof, env.journal, 65) == false);
        }
        // XPOP.transaction.proof invalid branch size
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["30"] = tmpProof[jss::children]["3"];
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof child node was not 0-F
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["not a hex"] = tmpProof[jss::children]["3"];
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof tree node has wrong format
        // invalid child (must be object)
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["3"] = "not object";
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof tree node has wrong format
        // invalid hash (must be string)
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["3"][jss::hash] = 1234;
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof tree node has wrong format
        // invalid hash size (must be 64)
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["3"][jss::hash] =
                "00000000000000000000000000000000000000000000000000000000000000"
                "000";
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof tree node has wrong format
        // invalid key (must be string)
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["3"][jss::key] = 1234;
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof tree node has wrong format
        // invalid key size (must be 64)
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["3"][jss::key] =
                "00000000000000000000000000000000000000000000000000000000000000"
                "000";
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof tree node has wrong format
        // invalid node children (must be object)
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["3"][jss::children] = "bad object";
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // XPOP.transaction.proof bad children format
        // invalid node child children (must be hex)
        {
            Json::Value tmpProof = proof;
            tmpProof[jss::children]["3"][jss::children] =
                tmpProof[jss::children]["3"];
            BEAST_EXPECT(syntaxCheckProof(tmpProof, env.journal) == false);
        }
        // success
        {
            BEAST_EXPECT(syntaxCheckProof(proof, env.journal) == true);
        }
    }

    void
    testSyntaxCheckXPOP(FeatureBitset features)
    {
        testcase("import utils - syntaxCheckXPOP");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(11111)};

        // blob empty
        {
            Blob raw{};
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // blob string empty
        {
            Blob raw{0};
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP failed to parse string json
        {
            Blob raw{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP is not a JSON object
        {
            std::string strJson = R"json([])json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger is not a JSON object
        {
            std::string strJson = R"json({
                "ledger": "not object"
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.transaction is not a JSON object
        {
            std::string strJson = R"json({
                "ledger": {},
                "transaction": "not object"
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.validation is not a JSON object
        {
            std::string strJson = R"json({
                "ledger": {},
                "transaction": {},
                "validation": "not object"
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.LEDGER

        // XPOP.ledger.acroot missing or wrong format
        // invalid xpop.ledger.acroot (must be string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": {}
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.acroot missing or wrong format
        // invalid xpop.ledger.acroot (must be 64)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "00000000000000000000000000000000000000000000000000000000000000000"
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.acroot missing or wrong format
        // invalid xpop.ledger.acroot (must be hex)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "this string will pass a length check but fail when checking hex."
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.txroot missing or wrong format
        // invalid xpop.ledger.txroot (must be string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": {}
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.txroot missing or wrong format
        // invalid xpop.ledger.txroot (must be 64)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "00000000000000000000000000000000000000000000000000000000000000000"
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.txroot missing or wrong format
        // invalid xpop.ledger.txroot (must be hex)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "this string will pass a length check but fail when checking hex."
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.phash missing or wrong format
        // invalid xpop.ledger.phash (must be string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": {}
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.phash missing or wrong format
        // invalid xpop.ledger.phash (must be 64)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "00000000000000000000000000000000000000000000000000000000000000000"
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.phash missing or wrong format
        // invalid xpop.ledger.phash (must be hex)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "this string will pass a length check but fail when checking hex."
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.close missing or wrong format
        // invalid xpop.ledger.close (must be int)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": "738786851"

                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.coins missing or wrong format
        // invalid xpop.ledger.coins (must be int or string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": {}
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.coins missing or wrong format
        // invalid xpop.ledger.coins (must be int or string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "not an int string"
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.cres missing or wrong format
        // invalid xpop.ledger.cres (must be int)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": "not an int"
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.flags missing or wrong format
        // invalid xpop.ledger.flags (must be int)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": "not an int"
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }
        // XPOP.ledger.pclose missing or wrong format
        // invalid xpop.ledger.pclose (must be int)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": "not an int"
                },
                "transaction": {},
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.TRANSACTION

        // XPOP.transaction.blob missing or wrong format
        // invalid xpop.transaction.blob (must be hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": {}
                },
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.transaction.blob missing or wrong format
        // invalid xpop.transaction.blob (must be hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "not a hex"
                },
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.transaction.meta missing or wrong format
        // invalid xpop.transaction.meta (must be hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": {}
                },
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.transaction.meta missing or wrong format
        // invalid xpop.transaction.meta (must be hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "not a hex"
                },
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.transaction.proof failed syntax check
        // invalid xpop.transaction.proof (must pass syntaxCheckProof)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {}
                },
                "validation": {}
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.data missing or wrong format
        // invalid xpop.validation.data (must be object)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": "not an object"
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl missing or wrong format
        // invalid xpop.validation.unl (must be object)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": "not an object"
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.TRANSACTION.DATA

        // XPOP.validation.data entry has wrong format
        // invalid xpop.validation.data key/value (must be base58 hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A"
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "not base 58": "",
                    },
                    "unl": {}
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.public_key missing or
        // invalid xpop.validation.data key/value (must be base58 hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": 1,
                    },
                    "unl": {}
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.public_key invalid key type.
        // invalid xpop.validation.data key/value (must be base58 hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "not a hex",
                    },
                    "unl": {}
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.TRANSACTION.UNL

        // XPOP.validation.unl.public_key missing or
        // invalid xpop.validation.unl.public_key (must be hex string with valid
        // key type)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": 1
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.public_key missing or
        // invalid xpop.validation.unl.public_key (must be hex string with valid
        // key type)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "not a hex"
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.public_key missing or
        // invalid xpop.validation.unl.public_key (must be hex string with valid
        // key type)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "0074D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1"
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.manifest missing or wrong
        // invalid xpop.validation.unl.manifest (must be string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                        "manifest": 1
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.blob missing or wrong
        // invalid xpop.validation.unl.blob (must be base 64 string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                        "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                        "blob": 1
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.blob missing or wrong
        // invalid xpop.validation.unl.blob (must be base 64 string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                        "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                        "blob": "not a base 64 string"
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.signature missing or wrong
        // invalid xpop.validation.unl.signature (must be hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                        "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                        "blob": "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9DdFNBND0ifV19",
                        "signature": 1
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.signature missing or wrong
        // invalid xpop.validation.unl.signature (must be hex string)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                        "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                        "blob": "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9DdFNBND0ifV19",
                        "signature": "not a hex"
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl.version missing or
        // invalid xpop.validation.unl.version (must be int)
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                        "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                        "blob": "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9DdFNBND0ifV19",
                        "signature": "849F6B8DA6E11C213B561659C16F13D35385E8EA9E775483ADC84578F6D578943DE5EB681584B2C03EFFFDFD216F9E0B21576E482F941C7195893B72B5B1F70D",
                        "version": "not an int"
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(
                syntaxCheckXPOP(raw, env.journal).has_value() == false);
        }

        // XPOP.validation.unl entry has wrong format

        // valid xpop
        {
            std::string strJson = R"json({
                "ledger": {
                    "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                    "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                    "close": 738786851,
                    "coins": "99999998999999868",
                    "cres": 10,
                    "flags": 10,
                    "pclose": 738786851
                },
                "transaction": {
                    "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                    "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                    "proof": {
                        "children": {
                            "3": {
                            "children": {},
                            "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                            "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                            }
                        },
                        "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                        "key": "0000000000000000000000000000000000000000000000000000000000000000"
                    }
                },
                "validation": {
                    "data": {
                        "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                    },
                    "unl": {
                        "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                        "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                        "blob": "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9DdFNBND0ifV19",
                        "signature": "849F6B8DA6E11C213B561659C16F13D35385E8EA9E775483ADC84578F6D578943DE5EB681584B2C03EFFFDFD216F9E0B21576E482F941C7195893B72B5B1F70D",
                        "version": 1
                    }
                }
            })json";
            Blob raw = Blob(strJson.begin(), strJson.end());
            BEAST_EXPECT(syntaxCheckXPOP(raw, env.journal).has_value() == true);
        }
    }

    void
    testGetVLInfo(FeatureBitset features)
    {
        testcase("import utils - getVLInfo");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(11111)};

        std::string strJson = R"json({
            "ledger": {
                "acroot": "DCE36DCACDCFCB3441866E09A183B7B8064B3F5E06593CD6AA8ACCC1B284B477",
                "txroot": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                "phash": "BAB19E13B25251A83493073F424FD986EA7BA49F9F4C83A061700131460D747D",
                "close": 738786851,
                "coins": "99999998999999868",
                "cres": 10,
                "flags": 10,
                "pclose": 738786851
            },
            "transaction": {
                "blob": "12000322000000002400000002201B0000006C201D0000535968400000003B9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519560F81148EA87CA747AF74EE03A7E36C0F8EB1C1568D588A",
                "meta": "201C00000000F8E51100612500000052553CEFE169D8DB251A7757C8A80214D7466E81FB2EF6A91E0970B428326B2134EB56A92E4B1340C656FE01A66D529BB7957EAD34D9E193D190EA62AAE4764B5B08C5E624000000026240000000773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481148EA87CA747AF74EE03A7E36C0F8EB1C1568D588AE1E1F1031000",
                "proof": {
                    "children": {
                        "3": {
                        "children": {},
                        "hash": "AA126BA0486ADAE575BBC5335E42E236275A452037CA9A876D1A8CDACA1AE542",
                        "key": "39D6F6AB7D0A827DD1502B7B9571626A2B601DBE5BC786EF8ADD00E0CB7FCEB3"
                        }
                    },
                    "hash": "409C8D073DDB5AB07FD2CD4F14467A8F3BC8FFBA16A0032D12D823D8511C12F4",
                    "key": "0000000000000000000000000000000000000000000000000000000000000000"
                }
            },
            "validation": {
                "data": {
                    "n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN": "22800000012600000059292C08FE225149FB62BB67011E9C3F641848E32A209CEAD98BD4D73A78A11F2F92E4AFA65F6A50177BC1723CF6FEDBE539067FF3EA2CF5ADF8FD611DA22B682617DB782041BCA913732103D4A61C3C4E882313665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76473045022100E53006BDE6CB3ECB7FD08711B103DFD793438751DDFBED5DC421A3C7DE3545C4022045A2332222261A55153A338930545A5F463DAC2E1BF56C4CE5E02726E919A714"
                },
                "unl": {
                    "public_key": "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC1",
                    "manifest": "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/ikfgf9SZOlOGcBcBJAw44PLjH+HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+IILJIiIKU/+1Uxx0FRpQbMDA==",
                    "blob": "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3JzIjpbeyJ2YWxpZGF0aW9uX3B1YmxpY19rZXkiOiJFRDM4QkQ0NDVBRkQ2MjE1OTYyMENDMTk2QzI2NjhBMjZCNkZCQjM2QjA5OUVCNTVCMzhBNThDMTFDMTIwNERFNUMiLCJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21hS0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aGFYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBudkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJYdlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsidmFsaWRhdGlvbl9wdWJsaWNfa2V5IjoiRURCRUUzMEZBRTkyRUVFODhFMUM0OTgwRDA5RUNGREU5OUExMTZEMDc4RUMyMTg1N0RCMUI0N0I0MjY0MThFNDI4IiwibWFuaWZlc3QiOiJKQUFBQUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQmprS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJFcVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQkk2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUjZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9DdFNBND0ifV19",
                    "signature": "849F6B8DA6E11C213B561659C16F13D35385E8EA9E775483ADC84578F6D578943DE5EB681584B2C03EFFFDFD216F9E0B21576E482F941C7195893B72B5B1F70D",
                    "version": 1
                }
            }
        })json";

        Json::Value xpop;
        Json::Reader reader;
        reader.parse(strJson, xpop);
        Json::FastWriter writer;

        // invalid UNL Blob after base 64 decode
        {
            Json::Value tmpXpop = xpop;
            tmpXpop[jss::validation][jss::unl][jss::blob] = "YmFkSnNvbg==";
            std::string strJson = writer.write(tmpXpop);
            Blob raw(strJson.begin(), strJson.end());
            auto const xpop = syntaxCheckXPOP(raw, env.journal);
            BEAST_EXPECT(getVLInfo(*xpop, env.journal).has_value() == false);
        }

        // invalid UNL Manifest parse
        {
            Json::Value tmpXpop = xpop;
            tmpXpop[jss::validation][jss::unl][jss::manifest] = "YmFkSnNvbg==";
            std::string strJson = writer.write(tmpXpop);
            Blob raw(strJson.begin(), strJson.end());
            auto const xpop = syntaxCheckXPOP(raw, env.journal);
            BEAST_EXPECT(getVLInfo(*xpop, env.journal).has_value() == false);
        }

        // valid UNL blob & manifest
        {
            Json::Value tmpXpop = xpop;
            std::string strJson = writer.write(tmpXpop);
            Blob raw(strJson.begin(), strJson.end());
            auto const xpop = syntaxCheckXPOP(raw, env.journal);
            auto const [seq, masterKey] = *getVLInfo(*xpop, env.journal);
            BEAST_EXPECT(std::to_string(seq) == "2");
            BEAST_EXPECT(
                strHex(masterKey) ==
                "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1"
                "CDC1");
        }
    }

    void
    testEnabled(FeatureBitset features)
    {
        testcase("enabled");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        auto const alice = Account("alice");

        for (bool const withImport : {false, true})
        {
            // If the Import amendment is not enabled, you should not be able
            // to import.
            auto const amend = withImport ? features : features - featureImport;
            Env env{*this, makeNetworkConfig(21337), amend};

            env.fund(XRP(1000), alice);
            env.close();

            auto const txResult =
                withImport ? ter(tesSUCCESS) : ter(temDISABLED);
            auto const ownerDir = withImport ? 1 : 0;

            // IMPORT - Account Set
            env(import(alice, accountSetXpop()), txResult);
            env.close();

            // IMPORT - Signers List Set
            env(import(alice, signersListSetXpop()), txResult);
            env.close();

            // IMPORT - Set Regular Key
            env(import(alice, setRegularKeyXpop()), txResult);
            env.close();
        }
    }

    void
    testInvalidPreflight(FeatureBitset features)
    {
        testcase("import invalid preflight");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        auto const bob = Account("bob");

        env.fund(XRP(1000), alice, bob);
        env.close();

        //----------------------------------------------------------------------
        // preflight

        // temDISABLED - disabled - CHECKED IN ENABLED
        // return ret; - preflight1 no success

        // temMALFORMED - sfFee cannot be 0
        {
            Json::Value tx = import(alice, accountSetXpop());
            STAmount const& fee = XRP(10);
            tx[jss::Fee] = fee.getJson(JsonOptions::none);
            env(tx, ter(temMALFORMED));
        }

        //* DA: Technically breaking the size throws before preflight
        // temMALFORMED
        // temMALFORMED - Import: blob was more than 512kib
        // {
        //     ripple::Blob blob;
        //     blob.resize(513 * 1024);
        //     env(import(alice, blob), ter(temMALFORMED));
        // }

        // temMALFORMED - sfAmount field must be in drops
        {
            Json::Value tx = import(alice, accountSetXpop());
            STAmount const& amount = XRP(-1);
            tx[jss::Amount] = amount.getJson(JsonOptions::none);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - !xpop | XPOP.validation is not a JSON object
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::validation] = {};  // one of many ways to throw error
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // telIMPORT_VL_KEY_NOT_RECOGNISED - Import: (fromchain) key does not
        // match (tochain) key
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::validation][jss::unl][jss::public_key] =
                "ED84D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1"
                "CDC1";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(telIMPORT_VL_KEY_NOT_RECOGNISED));
        }

        // getInnerTxn - !xpop
        // DA: Duplicate -

        // getInnerTxn - failed to deserialize tx blob inside xpop (invalid hex)
        // DA: Duplicate - "XPOP.transaction.blob missing or wrong format"

        // getInnerTxn - failed to deserialize tx meta inside xpop (invalid hex)
        // DA: Duplicate - "XPOP.transaction.meta missing or wrong format"

        // getInnerTxn - failed to deserialize tx blob/meta inside xpop
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::transaction][jss::blob] = "DEADBEEF";
            tmpXpop[jss::transaction][jss::meta] = "DEADBEEF";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - !stpTrans
        // DA: Duplicate - getInnerTxn (Any Failure)

        // temMALFORMED - Import: attempted to import xpop containing an emitted
        // or pseudo txn.
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::transaction][jss::blob] =
                "12000322000000002400000002201B00000069201D0000535968400000003B"
                "9ACA0073210388935426E0D08083314842EDFBB2D517BD47699F9A4527318A"
                "8E10468C97C0527446304402200E40CA821A7BFE347448FFA6540F67C7FFBF"
                "0287756D324DFBADCEDE2B23782C02207BE1B1294F1A1D5AC21990740B78F9"
                "C0693431237D6E07FE84228082986E50FF8114AE123A8556F3CF9115471137"
                "6AFB0F894F832B3DED202E000000013D00000000000000015B2200676F4B9B"
                "45C5ADE9DE3C8CD01200CB12DFF8C792220DB088FE615BE5C2905C207064D8"
                "1954F6A7225A8BAADF5A3042016BFB87355D1D0AFEDBAA8FB22F98355D745F"
                "398EEE9E6B294BBE6A5681A31A6107243D19384E277B5A7B1F23B8C83DE78A"
                "14AE123A8556F3CF91154711376AFB0F894F832B3DE1";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: inner txn lacked transaction result
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::transaction][jss::meta] =
                "201C00000006F8E5110061250000005655463E39A6AFDDA77DBF3591BF3C2A"
                "4BE9BB8D9113BF6D0797EB403C3D0D894FEF5692FA6A9FC8EA6018D5D16532"
                "D7795C91BFB0831355BDFDA177E86C8BF997985FE624000000026240000000"
                "773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481"
                "14AE123A8556F3CF91154711376AFB0F894F832B3DE1E1F1";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: inner txn did not have a tesSUCCESS or tec
        // result
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::transaction][jss::meta] =
                "201C00000006F8E5110061250000005655463E39A6AFDDA77DBF3591BF3C2A"
                "4BE9BB8D9113BF6D0797EB403C3D0D894FEF5692FA6A9FC8EA6018D5D16532"
                "D7795C91BFB0831355BDFDA177E86C8BF997985FE624000000026240000000"
                "773593F4E1E7220000000024000000032D0000000162400000003B9AC9F481"
                "14AE123A8556F3CF91154711376AFB0F894F832B3DE1E1F103103C";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: import and txn inside xpop must be signed by
        // the same account
        {
            Json::Value tmpXpop = accountSetXpop();
            Json::Value tx = import(bob, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: attempted to import xpop containing a txn with
        // a sfNetworkID field.
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::transaction][jss::blob] =
                "120003210000535922000000002400000002201B00000069201D0000535968"
                "400000003B9ACA0073210388935426E0D08083314842EDFBB2D517BD47699F"
                "9A4527318A8E10468C97C0527446304402200E40CA821A7BFE347448FFA654"
                "0F67C7FFBF0287756D324DFBADCEDE2B23782C02207BE1B1294F1A1D5AC219"
                "90740B78F9C0693431237D6E07FE84228082986E50FF8114AE123A8556F3CF"
                "91154711376AFB0F894F832B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: OperationLimit missing from inner xpop txn.
        // outer txid:
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::transaction][jss::blob] =
                "12000322000000002400000002201B0000006C68400000003B9ACA007321ED"
                "A8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C"
                "747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F"
                "232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C51"
                "77C519560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: Wrong network ID for OperationLimit in inner
        // txn. outer txid:
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::transaction][jss::blob] =
                "12000322000000002400000002201B0000006C201D0000535A68400000003B"
                "9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62"
                "ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593EC"
                "C21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF"
                "3EC206AD3C5177C519560F8114AE123A8556F3CF91154711376AFB0F894F83"
                "2B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(telWRONG_NETWORK));
        }

        // temMALFORMED - Import: inner txn must be an AccountSet, SetRegularKey
        // or SignerListSet transaction.
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::transaction][jss::blob] =
                "12006322000000002400000002201B0000006C201D0000535968400000003B"
                "9ACA007321EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62"
                "ECAD0AE4A96C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593EC"
                "C21B3C79EF0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF"
                "3EC206AD3C5177C519560F8114AE123A8556F3CF91154711376AFB0F894F83"
                "2B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: outer and inner txns were (multi) signed with
        // different keys.
        // temMALFORMED - Import: outer and inner txns were signed with
        // different keys.
        // temMALFORMED - Import: inner txn signature verify failed

        // temMALFORMED - Import: failed to deserialize manifest on txid
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::validation][jss::unl][jss::manifest] = "YmFkSnNvbg==";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: manifest master key did not match top level
        // master key in unl section of xpop
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::validation][jss::unl][jss::manifest] =
                "JAAAAAFxIe2E1ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+"
                "b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkAnUjfY5zOEkhq31tU4338"
                "jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/"
                "ikfgf9SZOlOGcBcBJAw44PLjH+"
                "HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+"
                "IILJIiIKU/+1Uxx0FRpQbMDA==";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: manifest signature invalid
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::validation][jss::unl][jss::manifest] =
                "JAAAAAFxIe101ANsZZGkvfnFTO+jm5lqXc5fhtEf2hh0SBzp1aHNwXMh7TN9+"
                "b62cZqTngaFYU5tbGpYHC8oYuI3G3vwj9OW2Z9gdkA3UjfY5zOEkhq31tU4338"
                "jcyUpVA5/VTsANFce7unDo+JeVoEhfuOb/Y8WA3Diu9XzuOD4U/"
                "ikfgf9SZOlOGcBcBJAw34PLjH+"
                "HUtEnwX45lIRmo0x5aINFMvZsBpE9QteSDBXKwYzLdnSW4e1bs21o+"
                "IILJIiIKU/+1Uxx0FRpQbMDA==";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob not signed correctly
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::validation][jss::unl][jss::signature] =
                "949F6B8DA6E11C213B561659C16F13D35385E8EA9E775483ADC84578F6D578"
                "943DE5EB681584B2C03EFFFDFD216F9E0B21576E482F941C7195893B72B5B1"
                "F70D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob was not valid json (after base64
        // decoding)
        // temMALFORMED - Import: unl blob json (after base64 decoding) lacked
        // required fields and/or types
        // temMALFORMED - Import: unl blob validUnil <= validFrom
        // temMALFORMED - Import: unl blob expired
        // temMALFORMED - Import: unl blob not yet valid

        // temMALFORMED - Import: depth > 32
        // temMALFORMED - Import: !proof->isObject() && !proof->isArray()
        // DA: Catchall Error
        // temMALFORMED - Import: return false

        // temMALFORMED - Import: xpop proof did not contain the specified txn
        // hash
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::transaction][jss::proof][jss::children]["D"]
                   [jss::children]["7"][jss::hash] =
                       "12D47E7D543E15F1EDBA91CDF335722727851BDDA8C2FF8924772AD"
                       "C6B522A29";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: depth > 32
        // temMALFORMED - Import: !proof.isObject() && !proof.isArray()
        // DA: CatchAll Error

        // temMALFORMED - Import: computed txroot does not match xpop txroot,
        // invalid xpop.
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::transaction][jss::proof][jss::children]["3"]
                   [jss::hash] =
                       "22D47E7D543E15F1EDBA91CDF335722727851BDDA8C2FF8924772AD"
                       "C6B522A29";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: error parsing coins | phash | acroot in the
        // ledger section of XPOP.

        // temMALFORMED - Import: unl blob contained invalid validator entry,
        // skipping
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::validation][jss::unl][jss::blob] =
                "eyJzZXF1ZW5jZSI6MiwiZXhwaXJhdGlvbiI6NzQxMzk4NDAwLCJ2YWxpZGF0b3"
                "JzIjpbeyJtYW5pZmVzdCI6IkpBQUFBQUp4SWUwNHZVUmEvV0lWbGlETUdXd21h"
                "S0pyYjdzMnNKbnJWYk9LV01FY0VnVGVYSE1oQW9HTU5jc2dVVU53S28raDd6aG"
                "FYS0YrSEd3NlhoRWpvREtyYWxrWW5Naktka2N3UlFJaEFKN2NONEo2TmRWZnBu"
                "dkVJL1pldVdDVHZucGFKaXJLTkZjQzN6TU9qZ3dqQWlBYktJMGZiWGdTMVJMbG"
                "9OaHhkSGhWcTlvekVXVkU5Y0l3WEROM0F4cXlZM0FTUUN0MCt1L2lOU0RENmJY"
                "dlVUdGRtdDROcnRsYng0Vnp1bVRwZmpSWXA0bE1vSS9oNDNwVVRqcDdWRm9YYm"
                "5LV2pWaHFOYUdtNTc3SzZKNjk3WFo3VFFFPSJ9LHsibWFuaWZlc3QiOiJKQUFB"
                "QUFKeEllMis0dyt1a3U3b2poeEpnTkNlejk2Wm9SYlFlT3doaFgyeHRIdENaQm"
                "prS0hNaEE5U21IRHhPaUNNVFpsNW5SeHJwMnlqV1o1Z2p4MkRyb1VGclU0bnJF"
                "cVU3ZGtjd1JRSWhBTGRFRmVqWStwVW5nbGk3c1R2aWIwQm1ESFA3TjZpa1ZFQk"
                "k2SDdJd1UxekFpQmRzeW9TcVBjQzJOTXFnQW5IWEhHZGtBSXdCUUQxQVVnOVg4"
                "WkpMeWZjd0hBU1FDdDFiS1Z6T014UlFtUjN3Tks0ZEtkb2ZJR3J4RTlTanVMUj"
                "ZQYThCNW4wOFNZSjhLNjJnZSs5YTZCdFphbEVtL0hPZGN6ME5BRk9jeWNyRi9D"
                "dFNBND0ifV19=";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: unl blob contained an invalid validator key,
        // skipping
        // temMALFORMED - Import: unl blob contained an invalid manifest,
        // skipping
        // temMALFORMED - Import: unl blob list entry manifest master key did
        // not match master key, skipping
        // temMALFORMED - Import: unl blob list entry manifest signature
        // invalid, skipping
        // temMALFORMED - Import: validator nodepub did not appear in validator
        // list but did appear
        // temMALFORMED - Import: validator nodepub key appears more than once
        // in data section

        // temMALFORMED - Import: validation inside xpop was not valid hex
        {
            Json::Value tmpXpop = accountSetXpop();
            Json::Value valData;
            valData["n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN"] =
                "not a hex";
            tmpXpop[jss::validation][jss::data] = valData;
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: validation message was not for computed ledger
        // hash
        {
            Json::Value tmpXpop = accountSetXpop();
            Json::Value valData;
            valData["n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN"] =
                "22800000012600000056292C0D012051A0829745427488A59B6525231634DC"
                "327F91589EB03F469ABF1C3CA32070625A501790AD68E9045001FDEED03F0A"
                "C8277F11F6E46E0ABD2DC38B1F8240D56B22E4F2732103D4A61C3C4E882313"
                "665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76463044022072"
                "427336342AE80B7AAE407CEA611B0DA680426B3C5894E52EE2D23641D09A73"
                "02203B131B68D3C5B6C5482312CC7D90D2CE131A9C46458967F2F98688B726"
                "C16719";
            tmpXpop[jss::validation][jss::data] = valData;
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // temMALFORMED - Import: validation inside xpop was not signed with a
        // signing key we recognise
        {
            Json::Value tmpXpop = accountSetXpop();
            Json::Value valData;
            valData["n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN"] =
                "22800000012600000056292C0D012051B0829745427488A59B6525231634DC"
                "327F91589EB03F469ABF1C3CA32070625A501790BD68E9045001FDEED03F0A"
                "C8277F11F6E46E0ABD2DC38B1F8240D56B22E4F2732103E4A61C3C4E882313"
                "665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76463054022072"
                "427336342AE80B7AAE407CEA611B0DA680426B3C5894E52EE2D23641D09A73"
                "02203B131B68D3C5B6C5482312CC7D90D2CE131A9C46458967F2F98688B726"
                "C16719";
            tmpXpop[jss::validation][jss::data] = valData;
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // temMALFORMED - Import: validation inside xpop was not correctly
        // signed
        {
            Json::Value tmpXpop = accountSetXpop();
            Json::Value valData;
            valData["n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN"] =
                "22800000012600000056292C0D012051B0829745427488A59B6525231634DC"
                "327F91589EB03F469ABF1C3CA32070625A501790BD68E9045001FDEED03F0A"
                "C8277F11F6E46E0ABD2DC38B1F8240D56B22E4F2732103D4A61C3C4E882313"
                "665E67471AE9DB28D6679823C760EBA1416B5389EB12A53B76463044022072"
                "427336342AE80B7AAE407CEA611B0DA680426B3C5894E52EE2D23641D09A73"
                "02203B131B68D3C5B6C5482312CC7D90D2CE131A9C46458967F2F98688B726"
                "C16719";
            tmpXpop[jss::validation][jss::data] = valData;
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: validation inside xpop was not able to be
        // parsed DA: Unknown Catch All

        // temMALFORMED - Import: xpop did not contain an 80% quorum for the txn
        // it purports to prove.
        {
            Json::Value tmpXpop = accountSetXpop();
            Json::Value valData;
            valData["n94at1vSdHSBEun25yT4ZfgqD1tVQNsx1nqRZG3T6ygbuvwgcMZN"] =
                "";
            valData["n9KXYzdZD8YpsNiChtMjP6yhvQAhkkh5XeSTbvYyV1waF8wkNnBT"] =
                "";
            tmpXpop[jss::validation][jss::data] = valData;
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // temMALFORMED - Import: xpop inner txn did not contain a sequence
        // number or fee No Sequence
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::transaction][jss::blob] =
                "1200632200000000201B0000006C201D0000535968400000003B9ACA007321"
                "EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A9"
                "6C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF"
                "0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C"
                "5177C519560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // No Fee
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::transaction][jss::blob] =
                "12006322000000002400000002201B0000006C201D000053597321EDA8D46E"
                "11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A96C747440"
                "549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF0F232EB4"
                "375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C5177C519"
                "560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // Bad Fee - TODO
    }

    void
    testInvalidPreclaim(FeatureBitset features)
    {
        testcase("import invalid preclaim");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        auto const bob = Account("bob");

        env.fund(XRP(1000), alice, bob);
        env.close();

        //----------------------------------------------------------------------
        // preclaim

        // temDISABLED - disabled - CHECKED IN ENABLED
        // tefINTERNAL/temMALFORMED - during preclaim could not parse xpop,
        // bailing.
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::validation] = {};  // one of many ways to throw error
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // tefINTERNAL/temMALFORMED - during preclaim could not find
        // importSequence, bailing.
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::transaction][jss::blob] =
                "1200632200000000201B0000006C201D0000535968400000003B9ACA007321"
                "EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A9"
                "6C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF"
                "0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C"
                "5177C519560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // tefPAST_IMPORT_SEQ -
        {
            env(import(alice, setRegularKeyXpop()), ter(tesSUCCESS));
            env(import(alice, accountSetXpop()), ter(tefPAST_IMPORT_SEQ));
        }

        // tefINTERNAL/temMALFORMED - !vlInfo
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::validation][jss::unl][jss::blob] = "YmFkSnNvbg==";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // tefPAST_IMPORT_VL_SEQ - sfImportSequence > vlInfo->first
        // {
        //     Json::Value tx = import(alice, accountSetXpop());
        //     env(tx, ter(tefPAST_IMPORT_SEQ));
        // }
    }

    void
    testInvalidDoApply(FeatureBitset features)
    {
        testcase("import invalid doApply");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        auto const bob = Account("bob");

        env.fund(XRP(1000), alice, bob);
        env.close();

        //----------------------------------------------------------------------
        // doApply

        // temDISABLED - disabled
        // DA: Sanity Check

        // tefINTERNAL/temMALFORMED - ctx_.tx.isFieldPresent(sfBlob)
        {
            Json::Value tx;
            tx[jss::TransactionType] = jss::Import;
            tx[jss::Account] = alice.human();
            env(tx, ter(temMALFORMED));
        }

        // tefINTERNAL/temMALFORMED - !xpop
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::validation] = {};  // one of many ways to throw error
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // tefINTERNAL/temMALFORMED - during apply could not find importSequence
        // or fee, bailing.
        // No Fee
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::transaction][jss::blob] =
                "1200632200000000201B0000006C201D0000535968400000003B9ACA007321"
                "EDA8D46E11FD5D2082A4E6FF3039EB6259FBC2334983D015FC62ECAD0AE4A9"
                "6C747440549A370E68DBB1947419D4CCDF90CAE0BCA9121593ECC21B3C79EF"
                "0F232EB4375F95F1EBCED78B94D09838B5E769D43F041019ADEF3EC206AD3C"
                "5177C519560F8114AE123A8556F3CF91154711376AFB0F894F832B3D";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }
        // No Sequence

        // tefINTERNAL - initBal <= beast::zero
        // Sanity Check

        // tefINTERNAL - ImportSequence passed
        // Sanity Check (tefPAST_IMPORT_SEQ)

        // tefINTERNAL/temMALFORMED - !infoVL
        {
            Json::Value tmpXpop = accountSetXpop();
            tmpXpop[jss::validation][jss::unl][jss::blob] = "YmFkSnNvbg==";
            Json::Value tx = import(alice, tmpXpop);
            env(tx, ter(temMALFORMED));
        }

        // tefINTERNAL - current > infoVL->first
        // Sanity Check (tefPAST_IMPORT_VL_SEQ)
    }

    void
    testAccountSetNA(FeatureBitset features)
    {
        testcase("account set tx - no account");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};

        // NO ACCOUNT
        {
            auto const alice = Account("alice");
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));
            Json::Value tx = import(alice, accountSetXpop());
            tx[jss::Sequence] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + XRP(1000) + XRP(2));
        }
    }

    void
    testAccountSetFA(FeatureBitset features)
    {
        testcase("account set tx - funded account");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        // FUNDED ACCOUNT
        {
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));
            env(import(alice, accountSetXpop()), ter(tesSUCCESS));
            env.close();
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + XRP(1000));
        }
    }

    void
    testSetRegularKeyNA(FeatureBitset features)
    {
        testcase("set regular key tx - no account");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};

        // NO ACCOUNT
        {
            auto const alice = Account("alice");
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));
            Json::Value tx = import(alice, setRegularKeyXpop());
            tx[jss::Sequence] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();
            auto const postAlice = env.balance(alice);
            auto const feeDrops = env.current()->fees().base;
            BEAST_EXPECT(
                postAlice == preAlice + XRP(2) + feeDrops + XRP(0.000002));
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == alice.id());
        }
    }

    void
    testSetRegularKeyFA(FeatureBitset features)
    {
        testcase("set regular key tx - funded account");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        // FUNDED ACCOUNT
        {
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));
            env(import(alice, setRegularKeyXpop()), ter(tesSUCCESS));
            env.close();
            auto const postAlice = env.balance(alice);
            auto const feeDrops = env.current()->fees().base;
            BEAST_EXPECT(postAlice == preAlice + feeDrops + XRP(0.000002));
            auto const [acct, acctSle] =
                accountKeyAndSle(*env.current(), alice);
            BEAST_EXPECT(acctSle->getAccountID(sfRegularKey) == alice.id());
        }
    }

    void
    testSignersListSetNA(FeatureBitset features)
    {
        testcase("signers list set tx - no account");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};

        // NO ACCOUNT
        {
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(0));
            Json::Value tx = import(alice, signersListSetXpop());
            tx[jss::Sequence] = 0;
            env(tx, alice, ter(tesSUCCESS));
            env.close();
            auto const postAlice = env.balance(alice);
            auto const feeDrops = env.current()->fees().base;
            BEAST_EXPECT(
                postAlice == preAlice + feeDrops + XRP(2) + XRP(0.000002));
            auto const [signers, signersSle] =
                signersKeyAndSle(*env.current(), alice);
            auto const signerEntries =
                signersSle->getFieldArray(sfSignerEntries);
            BEAST_EXPECT(signerEntries.size() == 1);
            BEAST_EXPECT(signerEntries[0u].getFieldU16(sfSignerWeight) == 1);
            BEAST_EXPECT(signerEntries[0u].getAccountID(sfAccount) == bob.id());
        }
    }

    void
    testSignersListSetFA(FeatureBitset features)
    {
        testcase("signers list set tx - funded account");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        // FUNDED ACCOUNT
        {
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));
            env(import(alice, signersListSetXpop()), ter(tesSUCCESS));
            env.close();
            auto const postAlice = env.balance(alice);
            auto const feeDrops = env.current()->fees().base;
            BEAST_EXPECT(postAlice == preAlice + feeDrops + XRP(0.000002));

            auto const [signers, signersSle] =
                signersKeyAndSle(*env.current(), alice);
            auto const signerEntries =
                signersSle->getFieldArray(sfSignerEntries);
            BEAST_EXPECT(signerEntries.size() == 1);
            BEAST_EXPECT(signerEntries[0u].getFieldU16(sfSignerWeight) == 1);
            BEAST_EXPECT(signerEntries[0u].getAccountID(sfAccount) == bob.id());
        }
    }

    void
    testDoubleEntry(FeatureBitset features)
    {
        testcase("double entry");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        auto const feeDrops = env.current()->fees().base;

        // ACCOUNT SET
        {
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(1000));
            env(import(alice, accountSetXpop()), ter(tesSUCCESS));
            env.close();
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + XRP(1000) + feeDrops);
        }

        // DOUBLE ENTRY
        {
            auto const preAlice = env.balance(alice);
            BEAST_EXPECT(preAlice == XRP(2000) + feeDrops);
            env(import(alice, accountSetXpop()), ter(tefPAST_IMPORT_SEQ));
            env.close();
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice);
        }
        // DELETE ACCOUNT
        // ACCOUNT SET
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        testWithFeats(all);
    }

    void
    testWithFeats(FeatureBitset features)
    {
        // testIsHex(features);
        // testIsBase58(features);
        // testIsBase64(features);
        // testParseUint64(features);
        // testSyntaxCheckProofObject(features);
        // testSyntaxCheckXPOP(features);
        // testGetVLInfo(features);
        // testEnabled(features);
        // testInvalidPreflight(features);
        // testInvalidPreclaim(features);
        // testInvalidDoApply(features);
        // testAccountSetNA(features);
        // testAccountSetFA(features);
        // testSetRegularKeyNA(features);
        // testSetRegularKeyFA(features);
        // testSignersListSetNA(features);
        // testSignersListSetFA(features);
        testDoubleEntry(features);
    }
};

BEAST_DEFINE_TESTSUITE(Import, app, ripple);

}  // namespace test
}  // namespace ripple
