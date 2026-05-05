//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL-Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  S
    OFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <test/app/Import_json.h>
#include <test/jtx.h>
#include <test/jtx/AMM.h>
#include <test/jtx/Oracle.h>
#include <test/jtx/TestHelpers.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/misc/TxQ.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/app/tx/detail/NFTokenUtils.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/hook/Enum.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/PayChan.h>
#include <xrpl/protocol/jss.h>
#include <vector>

namespace ripple {
namespace test {

#define BEAST_REQUIRE(x)     \
    {                        \
        BEAST_EXPECT(!!(x)); \
        if (!(x))            \
            return;          \
    }

struct SetHookTSH0_test : public beast::unit_test::suite
{
private:
    const uint64_t tshSTRONG = 0;
    const uint64_t tshWEAK = 1;
    const uint64_t tshNONE = 2;

    // helper
    void static overrideFlag(Json::Value& jv)
    {
        jv[jss::Flags] = hsfOVERRIDE;
    }
    void static collectFlag(Json::Value& jv)
    {
        jv[jss::Flags] = hsfOVERRIDE | hsfCOLLECT;
    }

    const std::vector<uint8_t> CallbackHook = {
        0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x35U,
        0x08U, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU, 0x01U, 0x7EU, 0x60U, 0x05U,
        0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x02U, 0x7FU,
        0x7FU, 0x01U, 0x7EU, 0x60U, 0x04U, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x01U,
        0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x00U, 0x01U, 0x7EU,
        0x60U, 0x03U, 0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x02U, 0x7FU,
        0x7FU, 0x01U, 0x7FU, 0x02U, 0xAEU, 0x01U, 0x0BU, 0x03U, 0x65U, 0x6EU,
        0x76U, 0x06U, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x00U,
        0x03U, 0x65U, 0x6EU, 0x76U, 0x05U, 0x74U, 0x72U, 0x61U, 0x63U, 0x65U,
        0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0CU, 0x68U, 0x6FU, 0x6FU,
        0x6BU, 0x5FU, 0x61U, 0x63U, 0x63U, 0x6FU, 0x75U, 0x6EU, 0x74U, 0x00U,
        0x02U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0AU, 0x75U, 0x74U, 0x69U, 0x6CU,
        0x5FU, 0x61U, 0x63U, 0x63U, 0x69U, 0x64U, 0x00U, 0x03U, 0x03U, 0x65U,
        0x6EU, 0x76U, 0x0CU, 0x65U, 0x74U, 0x78U, 0x6EU, 0x5FU, 0x72U, 0x65U,
        0x73U, 0x65U, 0x72U, 0x76U, 0x65U, 0x00U, 0x04U, 0x03U, 0x65U, 0x6EU,
        0x76U, 0x0AU, 0x6CU, 0x65U, 0x64U, 0x67U, 0x65U, 0x72U, 0x5FU, 0x73U,
        0x65U, 0x71U, 0x00U, 0x05U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0AU, 0x6FU,
        0x74U, 0x78U, 0x6EU, 0x5FU, 0x66U, 0x69U, 0x65U, 0x6CU, 0x64U, 0x00U,
        0x06U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0CU, 0x65U, 0x74U, 0x78U, 0x6EU,
        0x5FU, 0x64U, 0x65U, 0x74U, 0x61U, 0x69U, 0x6CU, 0x73U, 0x00U, 0x02U,
        0x03U, 0x65U, 0x6EU, 0x76U, 0x0DU, 0x65U, 0x74U, 0x78U, 0x6EU, 0x5FU,
        0x66U, 0x65U, 0x65U, 0x5FU, 0x62U, 0x61U, 0x73U, 0x65U, 0x00U, 0x02U,
        0x03U, 0x65U, 0x6EU, 0x76U, 0x04U, 0x65U, 0x6DU, 0x69U, 0x74U, 0x00U,
        0x03U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U, 0x07U,
        0x03U, 0x03U, 0x02U, 0x04U, 0x04U, 0x05U, 0x03U, 0x01U, 0x00U, 0x02U,
        0x06U, 0x31U, 0x08U, 0x7FU, 0x01U, 0x41U, 0xE0U, 0x8BU, 0x04U, 0x0BU,
        0x7FU, 0x00U, 0x41U, 0xA0U, 0x09U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0x80U,
        0x08U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0xD1U, 0x0BU, 0x0BU, 0x7FU, 0x00U,
        0x41U, 0x80U, 0x08U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0xE0U, 0x8BU, 0x04U,
        0x0BU, 0x7FU, 0x00U, 0x41U, 0x00U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0x01U,
        0x0BU, 0x07U, 0x0FU, 0x02U, 0x04U, 0x63U, 0x62U, 0x61U, 0x6BU, 0x00U,
        0x0BU, 0x04U, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x00U, 0x0CU, 0x0AU, 0xA6U,
        0x8EU, 0x00U, 0x02U, 0x8BU, 0x82U, 0x00U, 0x02U, 0x03U, 0x7EU, 0x04U,
        0x7FU, 0x02U, 0x7EU, 0x23U, 0x00U, 0x21U, 0x04U, 0x20U, 0x04U, 0x41U,
        0x10U, 0x6BU, 0x21U, 0x04U, 0x20U, 0x04U, 0x24U, 0x00U, 0x20U, 0x04U,
        0x20U, 0x00U, 0x36U, 0x02U, 0x0CU, 0x20U, 0x04U, 0x41U, 0x08U, 0x6AU,
        0x21U, 0x06U, 0x20U, 0x06U, 0x21U, 0x00U, 0x20U, 0x04U, 0x20U, 0x00U,
        0x36U, 0x02U, 0x04U, 0x20U, 0x04U, 0x28U, 0x02U, 0x0CU, 0x21U, 0x00U,
        0x20U, 0x00U, 0xADU, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x18U, 0x88U,
        0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0xFFU, 0x01U, 0x83U, 0x21U, 0x01U,
        0x20U, 0x01U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x04U, 0x28U, 0x02U, 0x04U,
        0x21U, 0x05U, 0x20U, 0x05U, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U,
        0x04U, 0x28U, 0x02U, 0x0CU, 0x21U, 0x00U, 0x20U, 0x00U, 0xADU, 0x21U,
        0x01U, 0x20U, 0x01U, 0x42U, 0x10U, 0x88U, 0x21U, 0x01U, 0x20U, 0x01U,
        0x42U, 0xFFU, 0x01U, 0x83U, 0x21U, 0x01U, 0x20U, 0x01U, 0xA7U, 0x21U,
        0x00U, 0x20U, 0x04U, 0x28U, 0x02U, 0x04U, 0x21U, 0x05U, 0x20U, 0x05U,
        0x20U, 0x00U, 0x3AU, 0x00U, 0x01U, 0x20U, 0x04U, 0x28U, 0x02U, 0x0CU,
        0x21U, 0x00U, 0x20U, 0x00U, 0xADU, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U,
        0x08U, 0x88U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0xFFU, 0x01U, 0x83U,
        0x21U, 0x01U, 0x20U, 0x01U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x04U, 0x28U,
        0x02U, 0x04U, 0x21U, 0x05U, 0x20U, 0x05U, 0x20U, 0x00U, 0x3AU, 0x00U,
        0x02U, 0x20U, 0x04U, 0x28U, 0x02U, 0x0CU, 0x21U, 0x00U, 0x20U, 0x00U,
        0xADU, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x00U, 0x88U, 0x21U, 0x01U,
        0x20U, 0x01U, 0x42U, 0xFFU, 0x01U, 0x83U, 0x21U, 0x01U, 0x20U, 0x01U,
        0xA7U, 0x21U, 0x00U, 0x20U, 0x04U, 0x28U, 0x02U, 0x04U, 0x21U, 0x05U,
        0x20U, 0x05U, 0x20U, 0x00U, 0x3AU, 0x00U, 0x03U, 0x20U, 0x06U, 0x21U,
        0x00U, 0x20U, 0x00U, 0x41U, 0x04U, 0x42U, 0x32U, 0x10U, 0x00U, 0x21U,
        0x02U, 0x20U, 0x02U, 0x1AU, 0x20U, 0x04U, 0x41U, 0x10U, 0x6AU, 0x21U,
        0x00U, 0x20U, 0x00U, 0x24U, 0x00U, 0x42U, 0x00U, 0x21U, 0x03U, 0x42U,
        0x00U, 0x0BU, 0x0BU, 0x94U, 0x8CU, 0x00U, 0x02U, 0x09U, 0x7EU, 0x05U,
        0x7FU, 0x02U, 0x40U, 0x02U, 0x40U, 0x23U, 0x00U, 0x21U, 0x0BU, 0x20U,
        0x0BU, 0x41U, 0x80U, 0x01U, 0x6BU, 0x21U, 0x0BU, 0x20U, 0x0BU, 0x24U,
        0x00U, 0x20U, 0x0BU, 0x20U, 0x00U, 0x36U, 0x02U, 0x7CU, 0x41U, 0xFBU,
        0x08U, 0x41U, 0x15U, 0x41U, 0xE7U, 0x08U, 0x41U, 0x14U, 0x41U, 0x00U,
        0x10U, 0x01U, 0x21U, 0x02U, 0x20U, 0x02U, 0x1AU, 0x41U, 0x9DU, 0x0AU,
        0x41U, 0x14U, 0x10U, 0x02U, 0x21U, 0x03U, 0x20U, 0x03U, 0x1AU, 0x41U,
        0xB3U, 0x0AU, 0x41U, 0x14U, 0x41U, 0x84U, 0x08U, 0x41U, 0x23U, 0x10U,
        0x03U, 0x21U, 0x01U, 0x20U, 0x0BU, 0x20U, 0x01U, 0x37U, 0x03U, 0x58U,
        0x41U, 0x01U, 0x10U, 0x04U, 0x21U, 0x04U, 0x20U, 0x04U, 0x1AU, 0x10U,
        0x05U, 0x21U, 0x01U, 0x20U, 0x01U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x00U,
        0x41U, 0x01U, 0x6AU, 0x21U, 0x00U, 0x20U, 0x0BU, 0x20U, 0x00U, 0x36U,
        0x02U, 0x54U, 0x20U, 0x0BU, 0x28U, 0x02U, 0x54U, 0x21U, 0x00U, 0x20U,
        0x00U, 0x41U, 0xFFU, 0x01U, 0x71U, 0x21U, 0x00U, 0x20U, 0x00U, 0x41U,
        0x18U, 0x74U, 0x21U, 0x00U, 0x20U, 0x0BU, 0x28U, 0x02U, 0x54U, 0x21U,
        0x0AU, 0x20U, 0x0AU, 0x41U, 0x80U, 0xFEU, 0x03U, 0x71U, 0x21U, 0x0AU,
        0x20U, 0x0AU, 0x41U, 0x08U, 0x74U, 0x21U, 0x0AU, 0x20U, 0x00U, 0x20U,
        0x0AU, 0x72U, 0x21U, 0x00U, 0x20U, 0x0BU, 0x28U, 0x02U, 0x54U, 0x21U,
        0x0AU, 0x20U, 0x0AU, 0x41U, 0x80U, 0x80U, 0xFCU, 0x07U, 0x71U, 0x21U,
        0x0AU, 0x20U, 0x0AU, 0x41U, 0x08U, 0x76U, 0x21U, 0x0AU, 0x20U, 0x00U,
        0x20U, 0x0AU, 0x72U, 0x21U, 0x00U, 0x20U, 0x0BU, 0x28U, 0x02U, 0x54U,
        0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x80U, 0x80U, 0x80U, 0x78U, 0x71U,
        0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x18U, 0x76U, 0x21U, 0x0AU, 0x20U,
        0x00U, 0x20U, 0x0AU, 0x72U, 0x21U, 0x00U, 0x41U, 0x00U, 0x20U, 0x00U,
        0x36U, 0x02U, 0xB4U, 0x09U, 0x20U, 0x0BU, 0x28U, 0x02U, 0x54U, 0x21U,
        0x00U, 0x20U, 0x00U, 0x41U, 0x04U, 0x6AU, 0x21U, 0x00U, 0x20U, 0x0BU,
        0x20U, 0x00U, 0x36U, 0x02U, 0x50U, 0x20U, 0x0BU, 0x28U, 0x02U, 0x50U,
        0x21U, 0x00U, 0x20U, 0x00U, 0x41U, 0xFFU, 0x01U, 0x71U, 0x21U, 0x00U,
        0x20U, 0x00U, 0x41U, 0x18U, 0x74U, 0x21U, 0x00U, 0x20U, 0x0BU, 0x28U,
        0x02U, 0x50U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x80U, 0xFEU, 0x03U,
        0x71U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x08U, 0x74U, 0x21U, 0x0AU,
        0x20U, 0x00U, 0x20U, 0x0AU, 0x72U, 0x21U, 0x00U, 0x20U, 0x0BU, 0x28U,
        0x02U, 0x50U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x80U, 0x80U, 0xFCU,
        0x07U, 0x71U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x08U, 0x76U, 0x21U,
        0x0AU, 0x20U, 0x00U, 0x20U, 0x0AU, 0x72U, 0x21U, 0x00U, 0x20U, 0x0BU,
        0x28U, 0x02U, 0x50U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x80U, 0x80U,
        0x80U, 0x78U, 0x71U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x18U, 0x76U,
        0x21U, 0x0AU, 0x20U, 0x00U, 0x20U, 0x0AU, 0x72U, 0x21U, 0x00U, 0x41U,
        0x00U, 0x20U, 0x00U, 0x36U, 0x02U, 0xBAU, 0x09U, 0x20U, 0x0BU, 0x42U,
        0xC0U, 0x84U, 0x3DU, 0x37U, 0x03U, 0x48U, 0x20U, 0x0BU, 0x41U, 0xBFU,
        0x09U, 0x36U, 0x02U, 0x44U, 0x20U, 0x0BU, 0x29U, 0x03U, 0x48U, 0x21U,
        0x01U, 0x20U, 0x01U, 0x42U, 0x38U, 0x88U, 0x21U, 0x01U, 0x20U, 0x01U,
        0x42U, 0x3FU, 0x83U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0xC0U, 0x00U,
        0x7CU, 0x21U, 0x01U, 0x20U, 0x01U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x0BU,
        0x28U, 0x02U, 0x44U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x01U, 0x6AU,
        0x21U, 0x0CU, 0x20U, 0x0BU, 0x20U, 0x0CU, 0x36U, 0x02U, 0x44U, 0x20U,
        0x0AU, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x0BU, 0x29U, 0x03U,
        0x48U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x30U, 0x88U, 0x21U, 0x01U,
        0x20U, 0x01U, 0x42U, 0xFFU, 0x01U, 0x83U, 0x21U, 0x01U, 0x20U, 0x01U,
        0xA7U, 0x21U, 0x00U, 0x20U, 0x0BU, 0x28U, 0x02U, 0x44U, 0x21U, 0x0AU,
        0x20U, 0x0AU, 0x41U, 0x01U, 0x6AU, 0x21U, 0x0CU, 0x20U, 0x0BU, 0x20U,
        0x0CU, 0x36U, 0x02U, 0x44U, 0x20U, 0x0AU, 0x20U, 0x00U, 0x3AU, 0x00U,
        0x00U, 0x20U, 0x0BU, 0x29U, 0x03U, 0x48U, 0x21U, 0x01U, 0x20U, 0x01U,
        0x42U, 0x28U, 0x88U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0xFFU, 0x01U,
        0x83U, 0x21U, 0x01U, 0x20U, 0x01U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x0BU,
        0x28U, 0x02U, 0x44U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x01U, 0x6AU,
        0x21U, 0x0CU, 0x20U, 0x0BU, 0x20U, 0x0CU, 0x36U, 0x02U, 0x44U, 0x20U,
        0x0AU, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x0BU, 0x29U, 0x03U,
        0x48U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x20U, 0x88U, 0x21U, 0x01U,
        0x20U, 0x01U, 0x42U, 0xFFU, 0x01U, 0x83U, 0x21U, 0x01U, 0x20U, 0x01U,
        0xA7U, 0x21U, 0x00U, 0x20U, 0x0BU, 0x28U, 0x02U, 0x44U, 0x21U, 0x0AU,
        0x20U, 0x0AU, 0x41U, 0x01U, 0x6AU, 0x21U, 0x0CU, 0x20U, 0x0BU, 0x20U,
        0x0CU, 0x36U, 0x02U, 0x44U, 0x20U, 0x0AU, 0x20U, 0x00U, 0x3AU, 0x00U,
        0x00U, 0x20U, 0x0BU, 0x29U, 0x03U, 0x48U, 0x21U, 0x01U, 0x20U, 0x01U,
        0x42U, 0x18U, 0x88U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0xFFU, 0x01U,
        0x83U, 0x21U, 0x01U, 0x20U, 0x01U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x0BU,
        0x28U, 0x02U, 0x44U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x01U, 0x6AU,
        0x21U, 0x0CU, 0x20U, 0x0BU, 0x20U, 0x0CU, 0x36U, 0x02U, 0x44U, 0x20U,
        0x0AU, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x0BU, 0x29U, 0x03U,
        0x48U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x10U, 0x88U, 0x21U, 0x01U,
        0x20U, 0x01U, 0x42U, 0xFFU, 0x01U, 0x83U, 0x21U, 0x01U, 0x20U, 0x01U,
        0xA7U, 0x21U, 0x00U, 0x20U, 0x0BU, 0x28U, 0x02U, 0x44U, 0x21U, 0x0AU,
        0x20U, 0x0AU, 0x41U, 0x01U, 0x6AU, 0x21U, 0x0CU, 0x20U, 0x0BU, 0x20U,
        0x0CU, 0x36U, 0x02U, 0x44U, 0x20U, 0x0AU, 0x20U, 0x00U, 0x3AU, 0x00U,
        0x00U, 0x20U, 0x0BU, 0x29U, 0x03U, 0x48U, 0x21U, 0x01U, 0x20U, 0x01U,
        0x42U, 0x08U, 0x88U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0xFFU, 0x01U,
        0x83U, 0x21U, 0x01U, 0x20U, 0x01U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x0BU,
        0x28U, 0x02U, 0x44U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x01U, 0x6AU,
        0x21U, 0x0CU, 0x20U, 0x0BU, 0x20U, 0x0CU, 0x36U, 0x02U, 0x44U, 0x20U,
        0x0AU, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x0BU, 0x29U, 0x03U,
        0x48U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x00U, 0x88U, 0x21U, 0x01U,
        0x20U, 0x01U, 0x42U, 0xFFU, 0x01U, 0x83U, 0x21U, 0x01U, 0x20U, 0x01U,
        0xA7U, 0x21U, 0x00U, 0x20U, 0x0BU, 0x28U, 0x02U, 0x44U, 0x21U, 0x0AU,
        0x20U, 0x0AU, 0x41U, 0x01U, 0x6AU, 0x21U, 0x0CU, 0x20U, 0x0BU, 0x20U,
        0x0CU, 0x36U, 0x02U, 0x44U, 0x20U, 0x0AU, 0x20U, 0x00U, 0x3AU, 0x00U,
        0x00U, 0x41U, 0xAEU, 0x09U, 0x41U, 0x04U, 0x41U, 0x83U, 0x80U, 0x08U,
        0x10U, 0x06U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x04U, 0x51U, 0x21U,
        0x00U, 0x20U, 0x00U, 0x41U, 0x01U, 0x71U, 0x21U, 0x00U, 0x20U, 0x00U,
        0x45U, 0x21U, 0x00U, 0x20U, 0x00U, 0x45U, 0x21U, 0x00U, 0x0BU, 0x20U,
        0x00U, 0x04U, 0x40U, 0x02U, 0x40U, 0x41U, 0x00U, 0x41U, 0x2EU, 0x3AU,
        0x00U, 0xADU, 0x09U, 0x01U, 0x0BU, 0x05U, 0x01U, 0x0BU, 0x0BU, 0x02U,
        0x7EU, 0x02U, 0x40U, 0x02U, 0x40U, 0x41U, 0xC7U, 0x0AU, 0x41U, 0x8AU,
        0x01U, 0x10U, 0x07U, 0x21U, 0x05U, 0x20U, 0x05U, 0x1AU, 0x41U, 0xA0U,
        0x09U, 0x41U, 0xB1U, 0x02U, 0x10U, 0x08U, 0x21U, 0x01U, 0x20U, 0x0BU,
        0x20U, 0x01U, 0x37U, 0x03U, 0x38U, 0x20U, 0x0BU, 0x41U, 0xF0U, 0x09U,
        0x36U, 0x02U, 0x34U, 0x20U, 0x0BU, 0x29U, 0x03U, 0x38U, 0x21U, 0x01U,
        0x20U, 0x01U, 0x42U, 0x38U, 0x87U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U,
        0x3FU, 0x83U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0xC0U, 0x00U, 0x7CU,
        0x21U, 0x01U, 0x20U, 0x01U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x0BU, 0x28U,
        0x02U, 0x34U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x01U, 0x6AU, 0x21U,
        0x0CU, 0x20U, 0x0BU, 0x20U, 0x0CU, 0x36U, 0x02U, 0x34U, 0x20U, 0x0AU,
        0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x0BU, 0x29U, 0x03U, 0x38U,
        0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x30U, 0x87U, 0x21U, 0x01U, 0x20U,
        0x01U, 0x42U, 0xFFU, 0x01U, 0x83U, 0x21U, 0x01U, 0x20U, 0x01U, 0xA7U,
        0x21U, 0x00U, 0x20U, 0x0BU, 0x28U, 0x02U, 0x34U, 0x21U, 0x0AU, 0x20U,
        0x0AU, 0x41U, 0x01U, 0x6AU, 0x21U, 0x0CU, 0x20U, 0x0BU, 0x20U, 0x0CU,
        0x36U, 0x02U, 0x34U, 0x20U, 0x0AU, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U,
        0x20U, 0x0BU, 0x29U, 0x03U, 0x38U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U,
        0x28U, 0x87U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0xFFU, 0x01U, 0x83U,
        0x21U, 0x01U, 0x20U, 0x01U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x0BU, 0x28U,
        0x02U, 0x34U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x01U, 0x6AU, 0x21U,
        0x0CU, 0x20U, 0x0BU, 0x20U, 0x0CU, 0x36U, 0x02U, 0x34U, 0x20U, 0x0AU,
        0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x0BU, 0x29U, 0x03U, 0x38U,
        0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x20U, 0x87U, 0x21U, 0x01U, 0x20U,
        0x01U, 0x42U, 0xFFU, 0x01U, 0x83U, 0x21U, 0x01U, 0x20U, 0x01U, 0xA7U,
        0x21U, 0x00U, 0x20U, 0x0BU, 0x28U, 0x02U, 0x34U, 0x21U, 0x0AU, 0x20U,
        0x0AU, 0x41U, 0x01U, 0x6AU, 0x21U, 0x0CU, 0x20U, 0x0BU, 0x20U, 0x0CU,
        0x36U, 0x02U, 0x34U, 0x20U, 0x0AU, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U,
        0x20U, 0x0BU, 0x29U, 0x03U, 0x38U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U,
        0x18U, 0x87U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0xFFU, 0x01U, 0x83U,
        0x21U, 0x01U, 0x20U, 0x01U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x0BU, 0x28U,
        0x02U, 0x34U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x01U, 0x6AU, 0x21U,
        0x0CU, 0x20U, 0x0BU, 0x20U, 0x0CU, 0x36U, 0x02U, 0x34U, 0x20U, 0x0AU,
        0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x0BU, 0x29U, 0x03U, 0x38U,
        0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x10U, 0x87U, 0x21U, 0x01U, 0x20U,
        0x01U, 0x42U, 0xFFU, 0x01U, 0x83U, 0x21U, 0x01U, 0x20U, 0x01U, 0xA7U,
        0x21U, 0x00U, 0x20U, 0x0BU, 0x28U, 0x02U, 0x34U, 0x21U, 0x0AU, 0x20U,
        0x0AU, 0x41U, 0x01U, 0x6AU, 0x21U, 0x0CU, 0x20U, 0x0BU, 0x20U, 0x0CU,
        0x36U, 0x02U, 0x34U, 0x20U, 0x0AU, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U,
        0x20U, 0x0BU, 0x29U, 0x03U, 0x38U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U,
        0x08U, 0x87U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0xFFU, 0x01U, 0x83U,
        0x21U, 0x01U, 0x20U, 0x01U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x0BU, 0x28U,
        0x02U, 0x34U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x01U, 0x6AU, 0x21U,
        0x0CU, 0x20U, 0x0BU, 0x20U, 0x0CU, 0x36U, 0x02U, 0x34U, 0x20U, 0x0AU,
        0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x0BU, 0x29U, 0x03U, 0x38U,
        0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x00U, 0x87U, 0x21U, 0x01U, 0x20U,
        0x01U, 0x42U, 0xFFU, 0x01U, 0x83U, 0x21U, 0x01U, 0x20U, 0x01U, 0xA7U,
        0x21U, 0x00U, 0x20U, 0x0BU, 0x28U, 0x02U, 0x34U, 0x21U, 0x0AU, 0x20U,
        0x0AU, 0x41U, 0x01U, 0x6AU, 0x21U, 0x0CU, 0x20U, 0x0BU, 0x20U, 0x0CU,
        0x36U, 0x02U, 0x34U, 0x20U, 0x0AU, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U,
        0x41U, 0x80U, 0x08U, 0x41U, 0x03U, 0x41U, 0xA0U, 0x09U, 0x41U, 0xB1U,
        0x02U, 0x41U, 0x01U, 0x10U, 0x01U, 0x21U, 0x06U, 0x20U, 0x06U, 0x1AU,
        0x20U, 0x0BU, 0x41U, 0x10U, 0x6AU, 0x21U, 0x00U, 0x20U, 0x00U, 0x41U,
        0x20U, 0x41U, 0xA0U, 0x09U, 0x41U, 0xB1U, 0x02U, 0x10U, 0x09U, 0x21U,
        0x01U, 0x20U, 0x0BU, 0x20U, 0x01U, 0x37U, 0x03U, 0x08U, 0x20U, 0x0BU,
        0x29U, 0x03U, 0x08U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x00U, 0x55U,
        0x21U, 0x00U, 0x20U, 0x00U, 0x41U, 0x01U, 0x71U, 0x21U, 0x00U, 0x20U,
        0x00U, 0x45U, 0x21U, 0x00U, 0x20U, 0x00U, 0x45U, 0x21U, 0x00U, 0x0BU,
        0x20U, 0x00U, 0x04U, 0x40U, 0x02U, 0x40U, 0x02U, 0x40U, 0x41U, 0xA7U,
        0x08U, 0x41U, 0x20U, 0x42U, 0xF5U, 0x00U, 0x10U, 0x00U, 0x21U, 0x07U,
        0x20U, 0x07U, 0x1AU, 0x0BU, 0x01U, 0x0BU, 0x05U, 0x01U, 0x0BU, 0x0BU,
        0x02U, 0x7EU, 0x02U, 0x7EU, 0x41U, 0xC7U, 0x08U, 0x41U, 0x20U, 0x42U,
        0xF7U, 0x00U, 0x10U, 0x00U, 0x21U, 0x08U, 0x20U, 0x08U, 0x1AU, 0x41U,
        0x01U, 0x41U, 0x01U, 0x10U, 0x0AU, 0x21U, 0x0DU, 0x20U, 0x0DU, 0x1AU,
        0x20U, 0x0BU, 0x41U, 0x80U, 0x01U, 0x6AU, 0x21U, 0x00U, 0x20U, 0x00U,
        0x24U, 0x00U, 0x42U, 0x00U, 0x21U, 0x09U, 0x42U, 0x00U, 0x0BU, 0x0BU,
        0x0BU, 0x0BU, 0x0BU, 0x88U, 0x02U, 0x04U, 0x00U, 0x41U, 0x80U, 0x08U,
        0x0BU, 0x90U, 0x01U, 0x74U, 0x78U, 0x6EU, 0x00U, 0x72U, 0x50U, 0x4DU,
        0x68U, 0x37U, 0x50U, 0x69U, 0x39U, 0x63U, 0x74U, 0x36U, 0x39U, 0x39U,
        0x69U, 0x5AU, 0x55U, 0x54U, 0x57U, 0x61U, 0x79U, 0x74U, 0x4AU, 0x55U,
        0x6FU, 0x48U, 0x63U, 0x4AU, 0x37U, 0x63U, 0x67U, 0x79U, 0x7AU, 0x69U,
        0x4BU, 0x00U, 0x63U, 0x61U, 0x6CU, 0x6CU, 0x62U, 0x61U, 0x63U, 0x6BU,
        0x2EU, 0x63U, 0x3AU, 0x20U, 0x54U, 0x78U, 0x20U, 0x65U, 0x6DU, 0x69U,
        0x74U, 0x74U, 0x65U, 0x64U, 0x20U, 0x73U, 0x75U, 0x63U, 0x63U, 0x65U,
        0x73U, 0x73U, 0x2EU, 0x00U, 0x63U, 0x61U, 0x6CU, 0x6CU, 0x62U, 0x61U,
        0x63U, 0x6BU, 0x2EU, 0x63U, 0x3AU, 0x20U, 0x54U, 0x78U, 0x20U, 0x65U,
        0x6DU, 0x69U, 0x74U, 0x74U, 0x65U, 0x64U, 0x20U, 0x66U, 0x61U, 0x69U,
        0x6CU, 0x75U, 0x72U, 0x65U, 0x2EU, 0x00U, 0x63U, 0x61U, 0x6CU, 0x6CU,
        0x62U, 0x61U, 0x63U, 0x6BU, 0x2EU, 0x63U, 0x3AU, 0x20U, 0x43U, 0x61U,
        0x6CU, 0x6CU, 0x65U, 0x64U, 0x2EU, 0x00U, 0x22U, 0x63U, 0x61U, 0x6CU,
        0x6CU, 0x62U, 0x61U, 0x63U, 0x6BU, 0x2EU, 0x63U, 0x3AU, 0x20U, 0x43U,
        0x61U, 0x6CU, 0x6CU, 0x65U, 0x64U, 0x2EU, 0x22U, 0x00U, 0x41U, 0xA0U,
        0x09U, 0x0BU, 0x5AU, 0x12U, 0x00U, 0x00U, 0x22U, 0x80U, 0x00U, 0x00U,
        0x00U, 0x24U, 0x00U, 0x00U, 0x00U, 0x00U, 0x99U, 0x99U, 0x99U, 0x99U,
        0x99U, 0x20U, 0x1AU, 0x00U, 0x00U, 0x00U, 0x00U, 0x20U, 0x1BU, 0x00U,
        0x00U, 0x00U, 0x00U, 0x61U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,
        0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,
        0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,
        0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,
        0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,
        0x99U, 0x99U, 0x68U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x73U, 0x21U, 0x00U, 0x41U, 0x9BU, 0x0AU, 0x0BU, 0x02U, 0x81U,
        0x14U, 0x00U, 0x41U, 0xB1U, 0x0AU, 0x0BU, 0x02U, 0x83U, 0x14U};

    const std::vector<uint8_t> EmitTenHook = {
        0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x35U,
        0x08U, 0x60U, 0x05U, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU,
        0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U,
        0x7EU, 0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU,
        0x7FU, 0x7EU, 0x01U, 0x7EU, 0x60U, 0x04U, 0x7FU, 0x7FU, 0x7FU, 0x7FU,
        0x01U, 0x7EU, 0x60U, 0x00U, 0x01U, 0x7EU, 0x60U, 0x03U, 0x7FU, 0x7FU,
        0x7FU, 0x01U, 0x7EU, 0x02U, 0xBEU, 0x01U, 0x0CU, 0x03U, 0x65U, 0x6EU,
        0x76U, 0x05U, 0x74U, 0x72U, 0x61U, 0x63U, 0x65U, 0x00U, 0x00U, 0x03U,
        0x65U, 0x6EU, 0x76U, 0x0CU, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x5FU, 0x61U,
        0x63U, 0x63U, 0x6FU, 0x75U, 0x6EU, 0x74U, 0x00U, 0x01U, 0x03U, 0x65U,
        0x6EU, 0x76U, 0x0CU, 0x65U, 0x74U, 0x78U, 0x6EU, 0x5FU, 0x72U, 0x65U,
        0x73U, 0x65U, 0x72U, 0x76U, 0x65U, 0x00U, 0x02U, 0x03U, 0x65U, 0x6EU,
        0x76U, 0x02U, 0x5FU, 0x67U, 0x00U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U,
        0x09U, 0x74U, 0x72U, 0x61U, 0x63U, 0x65U, 0x5FU, 0x6EU, 0x75U, 0x6DU,
        0x00U, 0x04U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0AU, 0x75U, 0x74U, 0x69U,
        0x6CU, 0x5FU, 0x61U, 0x63U, 0x63U, 0x69U, 0x64U, 0x00U, 0x05U, 0x03U,
        0x65U, 0x6EU, 0x76U, 0x0AU, 0x6CU, 0x65U, 0x64U, 0x67U, 0x65U, 0x72U,
        0x5FU, 0x73U, 0x65U, 0x71U, 0x00U, 0x06U, 0x03U, 0x65U, 0x6EU, 0x76U,
        0x0AU, 0x6FU, 0x74U, 0x78U, 0x6EU, 0x5FU, 0x66U, 0x69U, 0x65U, 0x6CU,
        0x64U, 0x00U, 0x07U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0CU, 0x65U, 0x74U,
        0x78U, 0x6EU, 0x5FU, 0x64U, 0x65U, 0x74U, 0x61U, 0x69U, 0x6CU, 0x73U,
        0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0DU, 0x65U, 0x74U, 0x78U,
        0x6EU, 0x5FU, 0x66U, 0x65U, 0x65U, 0x5FU, 0x62U, 0x61U, 0x73U, 0x65U,
        0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x04U, 0x65U, 0x6DU, 0x69U,
        0x74U, 0x00U, 0x05U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U,
        0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x04U, 0x03U, 0x02U, 0x01U, 0x02U,
        0x05U, 0x03U, 0x01U, 0x00U, 0x02U, 0x06U, 0x31U, 0x08U, 0x7FU, 0x01U,
        0x41U, 0xD0U, 0x8DU, 0x04U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0xB0U, 0x0BU,
        0x0BU, 0x7FU, 0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0x7FU, 0x00U, 0x41U,
        0xCBU, 0x0DU, 0x0BU, 0x7FU, 0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0x7FU,
        0x00U, 0x41U, 0xD0U, 0x8DU, 0x04U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0x00U,
        0x0BU, 0x7FU, 0x00U, 0x41U, 0x01U, 0x0BU, 0x07U, 0x08U, 0x01U, 0x04U,
        0x68U, 0x6FU, 0x6FU, 0x6BU, 0x00U, 0x0CU, 0x0AU, 0x9AU, 0x89U, 0x00U,
        0x01U, 0x96U, 0x89U, 0x00U, 0x02U, 0x02U, 0x7FU, 0x01U, 0x7EU, 0x23U,
        0x00U, 0x41U, 0x80U, 0x01U, 0x6BU, 0x22U, 0x01U, 0x24U, 0x00U, 0x20U,
        0x01U, 0x20U, 0x00U, 0x36U, 0x02U, 0x7CU, 0x41U, 0x9AU, 0x0BU, 0x41U,
        0x14U, 0x41U, 0x87U, 0x0BU, 0x41U, 0x13U, 0x41U, 0x00U, 0x10U, 0x00U,
        0x1AU, 0x41U, 0xADU, 0x0CU, 0x41U, 0x14U, 0x10U, 0x01U, 0x1AU, 0x41U,
        0x0AU, 0x10U, 0x02U, 0x1AU, 0x20U, 0x01U, 0x42U, 0x00U, 0x37U, 0x03U,
        0x58U, 0x03U, 0x40U, 0x41U, 0xB9U, 0x80U, 0x80U, 0x80U, 0x78U, 0x41U,
        0x0BU, 0x10U, 0x03U, 0x1AU, 0x02U, 0x40U, 0x20U, 0x01U, 0x29U, 0x03U,
        0x58U, 0x42U, 0x09U, 0x55U, 0x0DU, 0x00U, 0x41U, 0xD6U, 0x08U, 0x41U,
        0x01U, 0x20U, 0x01U, 0x29U, 0x03U, 0x58U, 0x10U, 0x04U, 0x1AU, 0x20U,
        0x01U, 0x29U, 0x03U, 0x58U, 0x22U, 0x03U, 0xA7U, 0x21U, 0x00U, 0x20U,
        0x03U, 0x42U, 0x09U, 0x58U, 0x04U, 0x40U, 0x02U, 0x40U, 0x02U, 0x40U,
        0x02U, 0x40U, 0x02U, 0x40U, 0x02U, 0x40U, 0x02U, 0x40U, 0x02U, 0x40U,
        0x02U, 0x40U, 0x02U, 0x40U, 0x02U, 0x40U, 0x20U, 0x00U, 0x41U, 0x01U,
        0x6BU, 0x0EU, 0x09U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
        0x08U, 0x09U, 0x00U, 0x0BU, 0x41U, 0xC3U, 0x0CU, 0x41U, 0x14U, 0x41U,
        0xB3U, 0x08U, 0x41U, 0x23U, 0x10U, 0x05U, 0x1AU, 0x0BU, 0x41U, 0xC3U,
        0x0CU, 0x41U, 0x14U, 0x41U, 0xE4U, 0x09U, 0x41U, 0x23U, 0x10U, 0x05U,
        0x1AU, 0x0BU, 0x41U, 0xC3U, 0x0CU, 0x41U, 0x14U, 0x41U, 0xFBU, 0x08U,
        0x41U, 0x23U, 0x10U, 0x05U, 0x1AU, 0x0BU, 0x41U, 0xC3U, 0x0CU, 0x41U,
        0x14U, 0x41U, 0xCDU, 0x0AU, 0x41U, 0x23U, 0x10U, 0x05U, 0x1AU, 0x0BU,
        0x41U, 0xC3U, 0x0CU, 0x41U, 0x14U, 0x41U, 0xD8U, 0x08U, 0x41U, 0x23U,
        0x10U, 0x05U, 0x1AU, 0x0BU, 0x41U, 0xC3U, 0x0CU, 0x41U, 0x14U, 0x41U,
        0xAAU, 0x0AU, 0x41U, 0x23U, 0x10U, 0x05U, 0x1AU, 0x0BU, 0x41U, 0xC3U,
        0x0CU, 0x41U, 0x14U, 0x41U, 0x8CU, 0x08U, 0x41U, 0x23U, 0x10U, 0x05U,
        0x1AU, 0x0BU, 0x41U, 0xC3U, 0x0CU, 0x41U, 0x14U, 0x41U, 0xC1U, 0x09U,
        0x41U, 0x23U, 0x10U, 0x05U, 0x1AU, 0x0BU, 0x41U, 0xC3U, 0x0CU, 0x41U,
        0x14U, 0x41U, 0x9EU, 0x09U, 0x41U, 0x23U, 0x10U, 0x05U, 0x1AU, 0x0BU,
        0x41U, 0xC3U, 0x0CU, 0x41U, 0x14U, 0x41U, 0x87U, 0x0AU, 0x41U, 0x23U,
        0x10U, 0x05U, 0x1AU, 0x0BU, 0x20U, 0x01U, 0x10U, 0x06U, 0xA7U, 0x41U,
        0x01U, 0x6AU, 0x36U, 0x02U, 0x54U, 0x41U, 0xC4U, 0x0BU, 0x20U, 0x01U,
        0x28U, 0x02U, 0x54U, 0x22U, 0x00U, 0x41U, 0xFFU, 0x01U, 0x71U, 0x41U,
        0x18U, 0x74U, 0x20U, 0x00U, 0x41U, 0x80U, 0xFEU, 0x03U, 0x71U, 0x41U,
        0x08U, 0x74U, 0x72U, 0x20U, 0x00U, 0x41U, 0x80U, 0x80U, 0xFCU, 0x07U,
        0x71U, 0x41U, 0x08U, 0x76U, 0x72U, 0x20U, 0x00U, 0x41U, 0x80U, 0x80U,
        0x80U, 0x78U, 0x71U, 0x41U, 0x18U, 0x76U, 0x72U, 0x36U, 0x02U, 0x00U,
        0x20U, 0x01U, 0x20U, 0x01U, 0x28U, 0x02U, 0x54U, 0x41U, 0x04U, 0x6AU,
        0x36U, 0x02U, 0x50U, 0x41U, 0xCAU, 0x0BU, 0x20U, 0x01U, 0x28U, 0x02U,
        0x50U, 0x22U, 0x00U, 0x41U, 0xFFU, 0x01U, 0x71U, 0x41U, 0x18U, 0x74U,
        0x20U, 0x00U, 0x41U, 0x80U, 0xFEU, 0x03U, 0x71U, 0x41U, 0x08U, 0x74U,
        0x72U, 0x20U, 0x00U, 0x41U, 0x80U, 0x80U, 0xFCU, 0x07U, 0x71U, 0x41U,
        0x08U, 0x76U, 0x72U, 0x20U, 0x00U, 0x41U, 0x80U, 0x80U, 0x80U, 0x78U,
        0x71U, 0x41U, 0x18U, 0x76U, 0x72U, 0x36U, 0x02U, 0x00U, 0x20U, 0x01U,
        0x42U, 0xC0U, 0x84U, 0x3DU, 0x37U, 0x03U, 0x48U, 0x20U, 0x01U, 0x41U,
        0xCFU, 0x0BU, 0x36U, 0x02U, 0x44U, 0x20U, 0x01U, 0x29U, 0x03U, 0x48U,
        0x42U, 0x38U, 0x88U, 0x42U, 0x3FU, 0x83U, 0x42U, 0x40U, 0x7DU, 0xA7U,
        0x21U, 0x00U, 0x20U, 0x01U, 0x20U, 0x01U, 0x28U, 0x02U, 0x44U, 0x22U,
        0x02U, 0x41U, 0x01U, 0x6AU, 0x36U, 0x02U, 0x44U, 0x20U, 0x02U, 0x20U,
        0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x01U, 0x29U, 0x03U, 0x48U, 0x42U,
        0x30U, 0x88U, 0x42U, 0xFFU, 0x01U, 0x83U, 0xA7U, 0x21U, 0x00U, 0x20U,
        0x01U, 0x20U, 0x01U, 0x28U, 0x02U, 0x44U, 0x22U, 0x02U, 0x41U, 0x01U,
        0x6AU, 0x36U, 0x02U, 0x44U, 0x20U, 0x02U, 0x20U, 0x00U, 0x3AU, 0x00U,
        0x00U, 0x20U, 0x01U, 0x29U, 0x03U, 0x48U, 0x42U, 0x28U, 0x88U, 0x42U,
        0xFFU, 0x01U, 0x83U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x01U, 0x20U, 0x01U,
        0x28U, 0x02U, 0x44U, 0x22U, 0x02U, 0x41U, 0x01U, 0x6AU, 0x36U, 0x02U,
        0x44U, 0x20U, 0x02U, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x01U,
        0x29U, 0x03U, 0x48U, 0x42U, 0x20U, 0x88U, 0x42U, 0xFFU, 0x01U, 0x83U,
        0xA7U, 0x21U, 0x00U, 0x20U, 0x01U, 0x20U, 0x01U, 0x28U, 0x02U, 0x44U,
        0x22U, 0x02U, 0x41U, 0x01U, 0x6AU, 0x36U, 0x02U, 0x44U, 0x20U, 0x02U,
        0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x01U, 0x29U, 0x03U, 0x48U,
        0x42U, 0x18U, 0x88U, 0x42U, 0xFFU, 0x01U, 0x83U, 0xA7U, 0x21U, 0x00U,
        0x20U, 0x01U, 0x20U, 0x01U, 0x28U, 0x02U, 0x44U, 0x22U, 0x02U, 0x41U,
        0x01U, 0x6AU, 0x36U, 0x02U, 0x44U, 0x20U, 0x02U, 0x20U, 0x00U, 0x3AU,
        0x00U, 0x00U, 0x20U, 0x01U, 0x29U, 0x03U, 0x48U, 0x42U, 0x10U, 0x88U,
        0x42U, 0xFFU, 0x01U, 0x83U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x01U, 0x20U,
        0x01U, 0x28U, 0x02U, 0x44U, 0x22U, 0x02U, 0x41U, 0x01U, 0x6AU, 0x36U,
        0x02U, 0x44U, 0x20U, 0x02U, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U,
        0x01U, 0x29U, 0x03U, 0x48U, 0x42U, 0x08U, 0x88U, 0x42U, 0xFFU, 0x01U,
        0x83U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x01U, 0x20U, 0x01U, 0x28U, 0x02U,
        0x44U, 0x22U, 0x02U, 0x41U, 0x01U, 0x6AU, 0x36U, 0x02U, 0x44U, 0x20U,
        0x02U, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x01U, 0x29U, 0x03U,
        0x48U, 0x42U, 0xFFU, 0x01U, 0x83U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x01U,
        0x20U, 0x01U, 0x28U, 0x02U, 0x44U, 0x22U, 0x02U, 0x41U, 0x01U, 0x6AU,
        0x36U, 0x02U, 0x44U, 0x20U, 0x02U, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U,
        0x41U, 0xBEU, 0x0BU, 0x41U, 0x04U, 0x41U, 0x83U, 0x80U, 0x08U, 0x10U,
        0x07U, 0x42U, 0x04U, 0x51U, 0x04U, 0x40U, 0x41U, 0xBDU, 0x0BU, 0x41U,
        0x2EU, 0x3AU, 0x00U, 0x00U, 0x0BU, 0x41U, 0xD7U, 0x0CU, 0x41U, 0xF4U,
        0x00U, 0x10U, 0x08U, 0x1AU, 0x20U, 0x01U, 0x41U, 0xB0U, 0x0BU, 0x41U,
        0x9BU, 0x02U, 0x10U, 0x09U, 0x37U, 0x03U, 0x38U, 0x20U, 0x01U, 0x41U,
        0x80U, 0x0CU, 0x36U, 0x02U, 0x34U, 0x20U, 0x01U, 0x29U, 0x03U, 0x38U,
        0x42U, 0x38U, 0x87U, 0x42U, 0x3FU, 0x83U, 0x42U, 0x40U, 0x7DU, 0xA7U,
        0x21U, 0x00U, 0x20U, 0x01U, 0x20U, 0x01U, 0x28U, 0x02U, 0x34U, 0x22U,
        0x02U, 0x41U, 0x01U, 0x6AU, 0x36U, 0x02U, 0x34U, 0x20U, 0x02U, 0x20U,
        0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x01U, 0x29U, 0x03U, 0x38U, 0x42U,
        0x30U, 0x87U, 0x42U, 0xFFU, 0x01U, 0x83U, 0xA7U, 0x21U, 0x00U, 0x20U,
        0x01U, 0x20U, 0x01U, 0x28U, 0x02U, 0x34U, 0x22U, 0x02U, 0x41U, 0x01U,
        0x6AU, 0x36U, 0x02U, 0x34U, 0x20U, 0x02U, 0x20U, 0x00U, 0x3AU, 0x00U,
        0x00U, 0x20U, 0x01U, 0x29U, 0x03U, 0x38U, 0x42U, 0x28U, 0x87U, 0x42U,
        0xFFU, 0x01U, 0x83U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x01U, 0x20U, 0x01U,
        0x28U, 0x02U, 0x34U, 0x22U, 0x02U, 0x41U, 0x01U, 0x6AU, 0x36U, 0x02U,
        0x34U, 0x20U, 0x02U, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x01U,
        0x29U, 0x03U, 0x38U, 0x42U, 0x20U, 0x87U, 0x42U, 0xFFU, 0x01U, 0x83U,
        0xA7U, 0x21U, 0x00U, 0x20U, 0x01U, 0x20U, 0x01U, 0x28U, 0x02U, 0x34U,
        0x22U, 0x02U, 0x41U, 0x01U, 0x6AU, 0x36U, 0x02U, 0x34U, 0x20U, 0x02U,
        0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x01U, 0x29U, 0x03U, 0x38U,
        0x42U, 0x18U, 0x87U, 0x42U, 0xFFU, 0x01U, 0x83U, 0xA7U, 0x21U, 0x00U,
        0x20U, 0x01U, 0x20U, 0x01U, 0x28U, 0x02U, 0x34U, 0x22U, 0x02U, 0x41U,
        0x01U, 0x6AU, 0x36U, 0x02U, 0x34U, 0x20U, 0x02U, 0x20U, 0x00U, 0x3AU,
        0x00U, 0x00U, 0x20U, 0x01U, 0x29U, 0x03U, 0x38U, 0x42U, 0x10U, 0x87U,
        0x42U, 0xFFU, 0x01U, 0x83U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x01U, 0x20U,
        0x01U, 0x28U, 0x02U, 0x34U, 0x22U, 0x02U, 0x41U, 0x01U, 0x6AU, 0x36U,
        0x02U, 0x34U, 0x20U, 0x02U, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U,
        0x01U, 0x29U, 0x03U, 0x38U, 0x42U, 0x08U, 0x87U, 0x42U, 0xFFU, 0x01U,
        0x83U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x01U, 0x20U, 0x01U, 0x28U, 0x02U,
        0x34U, 0x22U, 0x02U, 0x41U, 0x01U, 0x6AU, 0x36U, 0x02U, 0x34U, 0x20U,
        0x02U, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x01U, 0x29U, 0x03U,
        0x38U, 0x42U, 0xFFU, 0x01U, 0x83U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x01U,
        0x20U, 0x01U, 0x28U, 0x02U, 0x34U, 0x22U, 0x02U, 0x41U, 0x01U, 0x6AU,
        0x36U, 0x02U, 0x34U, 0x20U, 0x02U, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U,
        0x41U, 0xAFU, 0x08U, 0x41U, 0x03U, 0x41U, 0xB0U, 0x0BU, 0x41U, 0x9BU,
        0x02U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x20U, 0x01U, 0x20U, 0x01U,
        0x41U, 0x10U, 0x6AU, 0x41U, 0x20U, 0x41U, 0xB0U, 0x0BU, 0x41U, 0x9BU,
        0x02U, 0x10U, 0x0AU, 0x37U, 0x03U, 0x08U, 0x41U, 0x80U, 0x08U, 0x41U,
        0x0BU, 0x20U, 0x01U, 0x29U, 0x03U, 0x08U, 0x10U, 0x04U, 0x1AU, 0x20U,
        0x01U, 0x20U, 0x01U, 0x29U, 0x03U, 0x58U, 0x42U, 0x01U, 0x7CU, 0x37U,
        0x03U, 0x58U, 0x0CU, 0x01U, 0x0BU, 0x0BU, 0x41U, 0xF0U, 0x0AU, 0x41U,
        0x17U, 0x42U, 0x86U, 0x01U, 0x10U, 0x0BU, 0x1AU, 0x20U, 0x01U, 0x41U,
        0x80U, 0x01U, 0x6AU, 0x24U, 0x00U, 0x42U, 0x00U, 0x0BU, 0x0BU, 0xA6U,
        0x04U, 0x04U, 0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0xAEU, 0x03U, 0x65U,
        0x6DU, 0x69U, 0x74U, 0x5FU, 0x72U, 0x65U, 0x73U, 0x75U, 0x6CU, 0x74U,
        0x00U, 0x72U, 0x4BU, 0x77U, 0x58U, 0x70U, 0x69U, 0x48U, 0x47U, 0x63U,
        0x51U, 0x53U, 0x7AU, 0x4BU, 0x45U, 0x47U, 0x58U, 0x63U, 0x77U, 0x65U,
        0x6AU, 0x35U, 0x45U, 0x71U, 0x32U, 0x43U, 0x51U, 0x74U, 0x4EU, 0x36U,
        0x48U, 0x6FU, 0x54U, 0x44U, 0x72U, 0x00U, 0x74U, 0x78U, 0x6EU, 0x00U,
        0x72U, 0x47U, 0x31U, 0x51U, 0x51U, 0x76U, 0x32U, 0x6EU, 0x68U, 0x32U,
        0x67U, 0x72U, 0x37U, 0x52U, 0x43U, 0x5AU, 0x31U, 0x50U, 0x38U, 0x59U,
        0x59U, 0x63U, 0x42U, 0x55U, 0x4BU, 0x43U, 0x43U, 0x4EU, 0x36U, 0x33U,
        0x33U, 0x6AU, 0x43U, 0x6EU, 0x00U, 0x69U, 0x00U, 0x72U, 0x4CU, 0x38U,
        0x78U, 0x65U, 0x51U, 0x53U, 0x47U, 0x78U, 0x54U, 0x6DU, 0x4BU, 0x58U,
        0x58U, 0x79U, 0x4CU, 0x55U, 0x46U, 0x52U, 0x75U, 0x41U, 0x41U, 0x4BU,
        0x73U, 0x57U, 0x4DU, 0x59U, 0x31U, 0x42U, 0x4DU, 0x70U, 0x51U, 0x62U,
        0x65U, 0x00U, 0x72U, 0x48U, 0x34U, 0x4BU, 0x45U, 0x63U, 0x47U, 0x39U,
        0x64U, 0x45U, 0x77U, 0x47U, 0x77U, 0x70U, 0x6EU, 0x36U, 0x41U, 0x79U,
        0x6FU, 0x57U, 0x4BU, 0x39U, 0x63U, 0x5AU, 0x50U, 0x4CU, 0x4CU, 0x34U,
        0x52U, 0x4CU, 0x53U, 0x6DU, 0x57U, 0x57U, 0x00U, 0x72U, 0x68U, 0x57U,
        0x35U, 0x68U, 0x67U, 0x32U, 0x78U, 0x45U, 0x37U, 0x77U, 0x32U, 0x65U,
        0x77U, 0x65U, 0x70U, 0x62U, 0x68U, 0x57U, 0x74U, 0x67U, 0x5AU, 0x57U,
        0x57U, 0x38U, 0x53U, 0x75U, 0x4EU, 0x36U, 0x75U, 0x57U, 0x39U, 0x37U,
        0x53U, 0x00U, 0x72U, 0x4DU, 0x63U, 0x58U, 0x75U, 0x59U, 0x73U, 0x51U,
        0x33U, 0x4DU, 0x6AU, 0x46U, 0x65U, 0x52U, 0x52U, 0x71U, 0x69U, 0x31U,
        0x47U, 0x76U, 0x61U, 0x73U, 0x4AU, 0x7AU, 0x54U, 0x64U, 0x4CU, 0x37U,
        0x77U, 0x75U, 0x4CU, 0x33U, 0x68U, 0x4EU, 0x00U, 0x72U, 0x50U, 0x4DU,
        0x68U, 0x37U, 0x50U, 0x69U, 0x39U, 0x63U, 0x74U, 0x36U, 0x39U, 0x39U,
        0x69U, 0x5AU, 0x55U, 0x54U, 0x57U, 0x61U, 0x79U, 0x74U, 0x4AU, 0x55U,
        0x6FU, 0x48U, 0x63U, 0x4AU, 0x37U, 0x63U, 0x67U, 0x79U, 0x7AU, 0x69U,
        0x4BU, 0x00U, 0x72U, 0x70U, 0x67U, 0x67U, 0x78U, 0x47U, 0x73U, 0x34U,
        0x79U, 0x46U, 0x61U, 0x46U, 0x45U, 0x47U, 0x54U, 0x68U, 0x78U, 0x42U,
        0x45U, 0x62U, 0x68U, 0x69U, 0x72U, 0x37U, 0x54U, 0x39U, 0x46U, 0x62U,
        0x35U, 0x4CU, 0x51U, 0x4BU, 0x4CU, 0x43U, 0x00U, 0x72U, 0x44U, 0x76U,
        0x4AU, 0x64U, 0x79U, 0x4AU, 0x58U, 0x45U, 0x4DU, 0x63U, 0x4CU, 0x42U,
        0x79U, 0x59U, 0x37U, 0x79U, 0x38U, 0x66U, 0x50U, 0x58U, 0x4BU, 0x6EU,
        0x4BU, 0x36U, 0x67U, 0x39U, 0x42U, 0x70U, 0x41U, 0x79U, 0x68U, 0x62U,
        0x39U, 0x00U, 0x72U, 0x47U, 0x57U, 0x43U, 0x31U, 0x34U, 0x79U, 0x54U,
        0x33U, 0x55U, 0x38U, 0x62U, 0x6BU, 0x75U, 0x41U, 0x78U, 0x32U, 0x63U,
        0x38U, 0x74U, 0x41U, 0x31U, 0x61U, 0x6AU, 0x4CU, 0x55U, 0x4AU, 0x43U,
        0x61U, 0x6EU, 0x64U, 0x70U, 0x35U, 0x39U, 0x00U, 0x65U, 0x6DU, 0x69U,
        0x74U, 0x74U, 0x65U, 0x6EU, 0x2EU, 0x63U, 0x3AU, 0x20U, 0x53U, 0x75U,
        0x63U, 0x63U, 0x65U, 0x73U, 0x73U, 0x66U, 0x75U, 0x6CU, 0x2EU, 0x00U,
        0x65U, 0x6DU, 0x69U, 0x74U, 0x74U, 0x65U, 0x6EU, 0x2EU, 0x63U, 0x3AU,
        0x20U, 0x43U, 0x61U, 0x6CU, 0x6CU, 0x65U, 0x64U, 0x2EU, 0x00U, 0x22U,
        0x65U, 0x6DU, 0x69U, 0x74U, 0x74U, 0x65U, 0x6EU, 0x2EU, 0x63U, 0x3AU,
        0x20U, 0x43U, 0x61U, 0x6CU, 0x6CU, 0x65U, 0x64U, 0x2EU, 0x22U, 0x00U,
        0x41U, 0xB0U, 0x0BU, 0x0BU, 0x5AU, 0x12U, 0x00U, 0x00U, 0x22U, 0x80U,
        0x00U, 0x00U, 0x00U, 0x24U, 0x00U, 0x00U, 0x00U, 0x00U, 0x99U, 0x99U,
        0x99U, 0x99U, 0x99U, 0x20U, 0x1AU, 0x00U, 0x00U, 0x00U, 0x00U, 0x20U,
        0x1BU, 0x00U, 0x00U, 0x00U, 0x00U, 0x61U, 0x99U, 0x99U, 0x99U, 0x99U,
        0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,
        0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,
        0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,
        0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,
        0x99U, 0x99U, 0x99U, 0x99U, 0x68U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x73U, 0x21U, 0x00U, 0x41U, 0xABU, 0x0CU, 0x0BU,
        0x02U, 0x81U, 0x14U, 0x00U, 0x41U, 0xC1U, 0x0CU, 0x0BU, 0x02U, 0x83U,
        0x14U};

    const std::vector<uint8_t> TshHook = {
        0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x28U,
        0x06U, 0x60U, 0x05U, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU,
        0x60U, 0x04U, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x00U,
        0x01U, 0x7EU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU, 0x01U, 0x7EU, 0x60U,
        0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU,
        0x02U, 0x45U, 0x05U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x05U, 0x74U, 0x72U,
        0x61U, 0x63U, 0x65U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0AU,
        0x6FU, 0x74U, 0x78U, 0x6EU, 0x5FU, 0x70U, 0x61U, 0x72U, 0x61U, 0x6DU,
        0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0AU, 0x68U, 0x6FU, 0x6FU,
        0x6BU, 0x5FU, 0x61U, 0x67U, 0x61U, 0x69U, 0x6EU, 0x00U, 0x02U, 0x03U,
        0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U,
        0x00U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U,
        0x04U, 0x03U, 0x02U, 0x01U, 0x05U, 0x05U, 0x03U, 0x01U, 0x00U, 0x02U,
        0x06U, 0x2BU, 0x07U, 0x7FU, 0x01U, 0x41U, 0xC0U, 0x8BU, 0x04U, 0x0BU,
        0x7FU, 0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0xBCU,
        0x0BU, 0x0BU, 0x7FU, 0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0x7FU, 0x00U,
        0x41U, 0xC0U, 0x8BU, 0x04U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0x00U, 0x0BU,
        0x7FU, 0x00U, 0x41U, 0x01U, 0x0BU, 0x07U, 0x08U, 0x01U, 0x04U, 0x68U,
        0x6FU, 0x6FU, 0x6BU, 0x00U, 0x05U, 0x0AU, 0x8EU, 0x84U, 0x00U, 0x01U,
        0x8AU, 0x84U, 0x00U, 0x02U, 0x09U, 0x7EU, 0x05U, 0x7FU, 0x02U, 0x40U,
        0x02U, 0x40U, 0x23U, 0x00U, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x41U, 0x10U,
        0x6BU, 0x21U, 0x0AU, 0x20U, 0x0AU, 0x24U, 0x00U, 0x20U, 0x0AU, 0x20U,
        0x00U, 0x36U, 0x02U, 0x0CU, 0x41U, 0x9EU, 0x0BU, 0x41U, 0x0FU, 0x41U,
        0xC1U, 0x09U, 0x41U, 0x0EU, 0x41U, 0x00U, 0x10U, 0x00U, 0x21U, 0x02U,
        0x20U, 0x02U, 0x1AU, 0x20U, 0x0AU, 0x41U, 0x0BU, 0x6AU, 0x21U, 0x00U,
        0x20U, 0x00U, 0x41U, 0x01U, 0x41U, 0xBDU, 0x09U, 0x41U, 0x03U, 0x10U,
        0x01U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x01U, 0x51U, 0x21U, 0x00U,
        0x20U, 0x00U, 0x41U, 0x01U, 0x71U, 0x21U, 0x00U, 0x20U, 0x00U, 0x45U,
        0x21U, 0x00U, 0x20U, 0x00U, 0x45U, 0x21U, 0x00U, 0x0BU, 0x20U, 0x00U,
        0x04U, 0x40U, 0x02U, 0x40U, 0x02U, 0x40U, 0x10U, 0x02U, 0x21U, 0x03U,
        0x20U, 0x03U, 0x1AU, 0x0BU, 0x01U, 0x0BU, 0x05U, 0x01U, 0x0BU, 0x0BU,
        0x02U, 0x7EU, 0x02U, 0x40U, 0x20U, 0x0AU, 0x28U, 0x02U, 0x0CU, 0x21U,
        0x00U, 0x02U, 0x40U, 0x02U, 0x40U, 0x02U, 0x40U, 0x02U, 0x40U, 0x02U,
        0x40U, 0x20U, 0x00U, 0x0EU, 0x03U, 0x02U, 0x01U, 0x00U, 0x04U, 0x0BU,
        0x02U, 0x40U, 0x02U, 0x40U, 0x02U, 0x40U, 0x41U, 0xDBU, 0x09U, 0x41U,
        0xC3U, 0x00U, 0x41U, 0x80U, 0x08U, 0x41U, 0xC2U, 0x00U, 0x41U, 0x00U,
        0x10U, 0x00U, 0x21U, 0x04U, 0x20U, 0x04U, 0x1AU, 0x0BU, 0x0CU, 0x06U,
        0x0BU, 0x00U, 0x0BU, 0x00U, 0x0BU, 0x02U, 0x40U, 0x02U, 0x40U, 0x02U,
        0x40U, 0x41U, 0x9FU, 0x0AU, 0x41U, 0x3DU, 0x41U, 0xC2U, 0x08U, 0x41U,
        0x3CU, 0x41U, 0x00U, 0x10U, 0x00U, 0x21U, 0x05U, 0x20U, 0x05U, 0x1AU,
        0x0BU, 0x0CU, 0x05U, 0x0BU, 0x00U, 0x0BU, 0x00U, 0x0BU, 0x02U, 0x40U,
        0x02U, 0x40U, 0x02U, 0x40U, 0x41U, 0xDDU, 0x0AU, 0x41U, 0xC0U, 0x00U,
        0x41U, 0xFEU, 0x08U, 0x41U, 0x3FU, 0x41U, 0x00U, 0x10U, 0x00U, 0x21U,
        0x06U, 0x20U, 0x06U, 0x1AU, 0x0BU, 0x01U, 0x0BU, 0x0BU, 0x0BU, 0x0BU,
        0x0BU, 0x02U, 0x7EU, 0x02U, 0x7EU, 0x41U, 0xAEU, 0x0BU, 0x41U, 0x0DU,
        0x41U, 0xCFU, 0x09U, 0x41U, 0x0CU, 0x41U, 0x00U, 0x10U, 0x00U, 0x21U,
        0x07U, 0x20U, 0x07U, 0x1AU, 0x20U, 0x0AU, 0x41U, 0x07U, 0x6AU, 0x21U,
        0x0CU, 0x20U, 0x0CU, 0x21U, 0x00U, 0x20U, 0x0AU, 0x20U, 0x00U, 0x36U,
        0x02U, 0x00U, 0x20U, 0x0AU, 0x28U, 0x02U, 0x0CU, 0x21U, 0x00U, 0x20U,
        0x00U, 0xADU, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x18U, 0x88U, 0x21U,
        0x01U, 0x20U, 0x01U, 0x42U, 0xFFU, 0x01U, 0x83U, 0x21U, 0x01U, 0x20U,
        0x01U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x0AU, 0x28U, 0x02U, 0x00U, 0x21U,
        0x0BU, 0x20U, 0x0BU, 0x20U, 0x00U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x0AU,
        0x28U, 0x02U, 0x0CU, 0x21U, 0x00U, 0x20U, 0x00U, 0xADU, 0x21U, 0x01U,
        0x20U, 0x01U, 0x42U, 0x10U, 0x88U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U,
        0xFFU, 0x01U, 0x83U, 0x21U, 0x01U, 0x20U, 0x01U, 0xA7U, 0x21U, 0x00U,
        0x20U, 0x0AU, 0x28U, 0x02U, 0x00U, 0x21U, 0x0BU, 0x20U, 0x0BU, 0x20U,
        0x00U, 0x3AU, 0x00U, 0x01U, 0x20U, 0x0AU, 0x28U, 0x02U, 0x0CU, 0x21U,
        0x00U, 0x20U, 0x00U, 0xADU, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x08U,
        0x88U, 0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0xFFU, 0x01U, 0x83U, 0x21U,
        0x01U, 0x20U, 0x01U, 0xA7U, 0x21U, 0x00U, 0x20U, 0x0AU, 0x28U, 0x02U,
        0x00U, 0x21U, 0x0BU, 0x20U, 0x0BU, 0x20U, 0x00U, 0x3AU, 0x00U, 0x02U,
        0x20U, 0x0AU, 0x28U, 0x02U, 0x0CU, 0x21U, 0x00U, 0x20U, 0x00U, 0xADU,
        0x21U, 0x01U, 0x20U, 0x01U, 0x42U, 0x00U, 0x88U, 0x21U, 0x01U, 0x20U,
        0x01U, 0x42U, 0xFFU, 0x01U, 0x83U, 0x21U, 0x01U, 0x20U, 0x01U, 0xA7U,
        0x21U, 0x00U, 0x20U, 0x0AU, 0x28U, 0x02U, 0x00U, 0x21U, 0x0BU, 0x20U,
        0x0BU, 0x20U, 0x00U, 0x3AU, 0x00U, 0x03U, 0x20U, 0x0CU, 0x21U, 0x00U,
        0x20U, 0x00U, 0x41U, 0x04U, 0x42U, 0x1CU, 0x10U, 0x03U, 0x21U, 0x08U,
        0x20U, 0x08U, 0x1AU, 0x41U, 0x01U, 0x41U, 0x01U, 0x10U, 0x04U, 0x21U,
        0x0DU, 0x20U, 0x0DU, 0x1AU, 0x20U, 0x0AU, 0x41U, 0x10U, 0x6AU, 0x21U,
        0x00U, 0x20U, 0x00U, 0x24U, 0x00U, 0x42U, 0x00U, 0x21U, 0x09U, 0x42U,
        0x00U, 0x0BU, 0x0BU, 0x0BU, 0x0BU, 0x0BU, 0xC3U, 0x03U, 0x01U, 0x00U,
        0x41U, 0x80U, 0x08U, 0x0BU, 0xBBU, 0x03U, 0x74U, 0x73U, 0x68U, 0x2EU,
        0x63U, 0x3AU, 0x20U, 0x57U, 0x65U, 0x61U, 0x6BU, 0x20U, 0x41U, 0x67U,
        0x61U, 0x69U, 0x6EU, 0x2EU, 0x20U, 0x45U, 0x78U, 0x65U, 0x63U, 0x75U,
        0x74U, 0x65U, 0x20U, 0x41U, 0x46U, 0x54U, 0x45U, 0x52U, 0x20U, 0x74U,
        0x72U, 0x61U, 0x6EU, 0x73U, 0x61U, 0x63U, 0x74U, 0x69U, 0x6FU, 0x6EU,
        0x20U, 0x69U, 0x73U, 0x20U, 0x61U, 0x70U, 0x70U, 0x6CU, 0x69U, 0x65U,
        0x64U, 0x20U, 0x74U, 0x6FU, 0x20U, 0x6CU, 0x65U, 0x64U, 0x67U, 0x65U,
        0x72U, 0x00U, 0x74U, 0x73U, 0x68U, 0x2EU, 0x63U, 0x3AU, 0x20U, 0x57U,
        0x65U, 0x61U, 0x6BU, 0x2EU, 0x20U, 0x45U, 0x78U, 0x65U, 0x63U, 0x75U,
        0x74U, 0x65U, 0x20U, 0x41U, 0x46U, 0x54U, 0x45U, 0x52U, 0x20U, 0x74U,
        0x72U, 0x61U, 0x6EU, 0x73U, 0x61U, 0x63U, 0x74U, 0x69U, 0x6FU, 0x6EU,
        0x20U, 0x69U, 0x73U, 0x20U, 0x61U, 0x70U, 0x70U, 0x6CU, 0x69U, 0x65U,
        0x64U, 0x20U, 0x74U, 0x6FU, 0x20U, 0x6CU, 0x65U, 0x64U, 0x67U, 0x65U,
        0x72U, 0x00U, 0x74U, 0x73U, 0x68U, 0x2EU, 0x63U, 0x3AU, 0x20U, 0x53U,
        0x74U, 0x72U, 0x6FU, 0x6EU, 0x67U, 0x2EU, 0x20U, 0x45U, 0x78U, 0x65U,
        0x63U, 0x75U, 0x74U, 0x65U, 0x20U, 0x42U, 0x45U, 0x46U, 0x4FU, 0x52U,
        0x45U, 0x20U, 0x74U, 0x72U, 0x61U, 0x6EU, 0x73U, 0x61U, 0x63U, 0x74U,
        0x69U, 0x6FU, 0x6EU, 0x20U, 0x69U, 0x73U, 0x20U, 0x61U, 0x70U, 0x70U,
        0x6CU, 0x69U, 0x65U, 0x64U, 0x20U, 0x74U, 0x6FU, 0x20U, 0x6CU, 0x65U,
        0x64U, 0x67U, 0x65U, 0x72U, 0x00U, 0x41U, 0x41U, 0x57U, 0x00U, 0x74U,
        0x73U, 0x68U, 0x2EU, 0x63U, 0x3AU, 0x20U, 0x53U, 0x74U, 0x61U, 0x72U,
        0x74U, 0x2EU, 0x00U, 0x74U, 0x73U, 0x68U, 0x2EU, 0x63U, 0x3AU, 0x20U,
        0x45U, 0x6EU, 0x64U, 0x2EU, 0x00U, 0x22U, 0x74U, 0x73U, 0x68U, 0x2EU,
        0x63U, 0x3AU, 0x20U, 0x57U, 0x65U, 0x61U, 0x6BU, 0x20U, 0x41U, 0x67U,
        0x61U, 0x69U, 0x6EU, 0x2EU, 0x20U, 0x45U, 0x78U, 0x65U, 0x63U, 0x75U,
        0x74U, 0x65U, 0x20U, 0x41U, 0x46U, 0x54U, 0x45U, 0x52U, 0x20U, 0x74U,
        0x72U, 0x61U, 0x6EU, 0x73U, 0x61U, 0x63U, 0x74U, 0x69U, 0x6FU, 0x6EU,
        0x20U, 0x69U, 0x73U, 0x20U, 0x61U, 0x70U, 0x70U, 0x6CU, 0x69U, 0x65U,
        0x64U, 0x20U, 0x74U, 0x6FU, 0x20U, 0x6CU, 0x65U, 0x64U, 0x67U, 0x65U,
        0x72U, 0x22U, 0x00U, 0x22U, 0x74U, 0x73U, 0x68U, 0x2EU, 0x63U, 0x3AU,
        0x20U, 0x57U, 0x65U, 0x61U, 0x6BU, 0x2EU, 0x20U, 0x45U, 0x78U, 0x65U,
        0x63U, 0x75U, 0x74U, 0x65U, 0x20U, 0x41U, 0x46U, 0x54U, 0x45U, 0x52U,
        0x20U, 0x74U, 0x72U, 0x61U, 0x6EU, 0x73U, 0x61U, 0x63U, 0x74U, 0x69U,
        0x6FU, 0x6EU, 0x20U, 0x69U, 0x73U, 0x20U, 0x61U, 0x70U, 0x70U, 0x6CU,
        0x69U, 0x65U, 0x64U, 0x20U, 0x74U, 0x6FU, 0x20U, 0x6CU, 0x65U, 0x64U,
        0x67U, 0x65U, 0x72U, 0x22U, 0x00U, 0x22U, 0x74U, 0x73U, 0x68U, 0x2EU,
        0x63U, 0x3AU, 0x20U, 0x53U, 0x74U, 0x72U, 0x6FU, 0x6EU, 0x67U, 0x2EU,
        0x20U, 0x45U, 0x78U, 0x65U, 0x63U, 0x75U, 0x74U, 0x65U, 0x20U, 0x42U,
        0x45U, 0x46U, 0x4FU, 0x52U, 0x45U, 0x20U, 0x74U, 0x72U, 0x61U, 0x6EU,
        0x73U, 0x61U, 0x63U, 0x74U, 0x69U, 0x6FU, 0x6EU, 0x20U, 0x69U, 0x73U,
        0x20U, 0x61U, 0x70U, 0x70U, 0x6CU, 0x69U, 0x65U, 0x64U, 0x20U, 0x74U,
        0x6FU, 0x20U, 0x6CU, 0x65U, 0x64U, 0x67U, 0x65U, 0x72U, 0x22U, 0x00U,
        0x22U, 0x74U, 0x73U, 0x68U, 0x2EU, 0x63U, 0x3AU, 0x20U, 0x53U, 0x74U,
        0x61U, 0x72U, 0x74U, 0x2EU, 0x22U, 0x00U, 0x22U, 0x74U, 0x73U, 0x68U,
        0x2EU, 0x63U, 0x3AU, 0x20U, 0x45U, 0x6EU, 0x64U, 0x2EU, 0x22U};

    void
    addWeakTSH(jtx::Env& env, jtx::Account const& account)
    {
        using namespace test::jtx;
        env(fset(account, asfTshCollect));
        env.close();
    }

    void
    setTSHHook(
        jtx::Env& env,
        jtx::Account const& account,
        bool const& testStrong)
    {
        using namespace test::jtx;
        auto const tshFlag = testStrong ? overrideFlag : collectFlag;
        env(hook(account, {{hso(TshHook, tshFlag)}}, 0),
            fee(XRP(2)),
            ter(tesSUCCESS));
        env.close();
    }

    void
    setCallbackHook(
        jtx::Env& env,
        jtx::Account const& account,
        bool const& testStrong)
    {
        using namespace test::jtx;
        auto const tshFlag = testStrong ? overrideFlag : collectFlag;
        env(hook(account, {{hso(CallbackHook, tshFlag)}}, 0),
            fee(XRP(2)),
            ter(tesSUCCESS));
        env.close();
    }

    void
    validateTSHStrongWeak(
        Json::Value meta,
        uint64_t const& expected,
        uint64_t const& lineno)
    {
        switch (expected)
        {
            // tshSTRONG
            case 0: {
                auto const executions = meta[sfHookExecutions.jsonName];
                auto const execution = executions[0u][sfHookExecution.jsonName];
                BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
                BEAST_EXPECT(
                    execution[sfHookReturnString.jsonName] == "00000000");
                if (execution[sfHookReturnString.jsonName] != "00000000")
                {
                    std::cout << "testTSHStrongWeak Line: " << lineno << "\n";
                    std::cout
                        << "testTSHStrongWeak Expected: " << expected
                        << " Result: " << execution[sfHookReturnString.jsonName]
                        << "\n";
                    std::cout << "testTSHStrongWeak Meta: " << meta << "\n";
                }
                break;
            }
            // tshWEAK
            case 1: {
                auto const executions = meta[sfHookExecutions.jsonName];
                auto const execution = executions[0u][sfHookExecution.jsonName];
                BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
                BEAST_EXPECT(
                    execution[sfHookReturnString.jsonName] == "00000001");
                if (execution[sfHookReturnString.jsonName] != "00000001")
                {
                    std::cout << "testTSHStrongWeak Line: " << lineno << "\n";
                    std::cout
                        << "testTSHStrongWeak Expected: " << expected
                        << " Result: " << execution[sfHookReturnString.jsonName]
                        << "\n";
                    std::cout << "testTSHStrongWeak Meta: " << meta << "\n";
                }
                break;
            }
            // tshNONE
            case 2: {
                auto const executions = meta[sfHookExecutions.jsonName];
                BEAST_EXPECT(executions.size() == 0);
                if (executions.size() != 0)
                {
                    std::cout << "testTSHStrongWeak Line: " << lineno << "\n";
                    std::cout << "testTSHStrongWeak Expected: " << expected
                              << " Result: " << executions.size() << "\n";
                    std::cout << "testTSHStrongWeak Meta: " << meta << "\n";
                }
                break;
            }

            default:
                break;
        }
    }

    void
    validateTSHFlags(jtx::Env& env, Json::Value meta, uint32_t const& expected)
    {
        auto const executions = meta[sfHookExecutions.jsonName];
        auto const execution = executions[0u][sfHookExecution.jsonName];
        bool const fixV2 = env.current()->rules().enabled(fixXahauV2);
        if (fixV2)
        {
            BEAST_EXPECT(execution[sfFlags.jsonName] == expected);
        }
        else
        {
            BEAST_REQUIRE(!execution[sfFlags.jsonName]);
        }
    }

    void
    testTSHStrongWeak(
        jtx::Env& env,
        int const& expected,
        uint64_t const& lineno)
    {
        auto const hashStr =
            env.tx()->getJson(JsonOptions::none)[jss::hash].asString();
        uint256 const txHash = uint256::fromVoid(strUnHex(hashStr)->data());
        testTSHStrongWeak(env, txHash, expected, lineno);
    }

    void
    testTSHStrongWeak(
        jtx::Env& env,
        uint256 const& txHash,
        int const& expected,
        uint64_t const& lineno)
    {
        Json::Value params;
        params[jss::transaction] = strHex(txHash);
        auto const jrr = env.rpc("json", "tx", to_string(params));
        auto const meta = jrr[jss::result][jss::meta];
        validateTSHStrongWeak(meta, expected, lineno);
    }

    // AccountSet
    // | otxn | tsh | set |
    // |   A  |  A  |  S  |
    void
    testAccountSetTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;
        testcase("account set TSH");

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // account set
            env(fset(account, asfDefaultRipple), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // AccountDelete
    // | otxn | tsh |  delete  |
    // |   A  |  A  |   N/A    |
    // |   A  |  B  |    S     |
    // Account cannot delete with a hook installed
    void
    testAccountDeleteTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;
        testcase("account delete TSH");

        // otxn: account
        // tsh bene
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const bene = Account("bob");
            env.fund(XRP(1000), account, bene);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, bene);

            // set tsh hook
            setTSHHook(env, bene, testStrong);

            // AccountDelete
            incLgrSeqForAccDel(env, account);
            env(acctdelete(account, bene), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // clang-format off
    // AMM
    // | otxn | tsh | Bid | Create | Delete | Clawback | Deposit | Vote | Withdraw |
    // |   A  |  I  |  -  |    W   |    W   |     W    |    W    |   -  |     W    |
    // |   A  |  H  |  -  |    -   |    -   |     W    |    -    |   -  |     -    |
    // clang-format on
    void
    testAMMBidTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;
        testcase("amm bid tsh");

        // otxn: account
        // tsh issuer
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("gw");
            auto const account = Account("alice");
            auto const USD = issuer["USD"];

            env.fund(XRP(30'000), issuer, account);
            env.close();
            env.trust(USD(30'000), account);
            env.close();
            env(pay(issuer, account, USD(10'000)));
            env.close();

            // create AMM
            AMM ammAlice(env, account, XRP(10'000), USD(10'000));

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // bid
            ammAlice.bid({
                .account = account,
                .bidMin = 100,
            });

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }
    }

    // AMMCreate
    void
    testAMMCreateTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;
        testcase("amm create tsh");

        // otxn: account
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("gw");
            auto const account = Account("alice");
            auto const USD = issuer["USD"];

            env.fund(XRP(30'000), issuer, account);
            env.close();
            env.trust(USD(30'000), account);
            env.close();
            env(pay(issuer, account, USD(10'000)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // create AMM
            AMM ammAlice(env, account, XRP(10'000), USD(10'000));

            // verify tsh hook triggered
            if (features[featureIOUIssuerWeakTSH])
            {
                auto const expected = testStrong ? tshNONE : tshWEAK;
                testTSHStrongWeak(env, expected, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }
    }

    // AMMDelete
    void
    testAMMDeleteTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;
        testcase("amm delete tsh");

        // otxn: account
        // tsh issuer, holder
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env(
                *this,
                envconfig([](std::unique_ptr<Config> cfg) {
                    cfg->FEES.reference_fee = XRPAmount(1);
                    return cfg;
                }),
                features);

            auto const issuer = Account("gw");
            auto const account = Account("alice");
            auto const bob = Account("bob");
            auto const USD = issuer["USD"];

            env.fund(XRP(20'000), issuer, account, bob);
            env.close();
            env.trust(USD(10'000), account);
            env.close();
            env(pay(issuer, account, USD(10'000)));
            env.close();

            AMM amm(env, account, XRP(10'000), USD(10'000));
            for (auto i = 0; i < maxDeletableAMMTrustLines + 10; ++i)
            {
                Account const a{std::to_string(i)};
                env.fund(XRP(1'000), a);
                env(trust(a, STAmount{amm.lptIssue(), 10'000}));
                // set tsh collect
                if (!testStrong)
                    env(fset(a, asfTshCollect));
                // set tsh hook
                setTSHHook(env, a, testStrong);
            }
            amm.withdrawAll(account);
            BEAST_EXPECT(amm.ammExists());

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // delete
            amm.ammDelete(bob);

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }
    }

    // AMMClawback
    void
    testAMMClawbackTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;
        testcase("amm clawback tsh");

        // otxn: account
        // tsh holder
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("gw");
            auto const account = Account("alice");
            auto const USD = issuer["USD"];

            env.fund(XRP(30'000), issuer, account);
            env.close();
            env(fset(issuer, asfAllowTrustLineClawback));
            env.close();
            env.trust(USD(30'000), account);
            env.close();
            env(pay(issuer, account, USD(30'000)));
            env.close();

            // create AMM
            AMM ammAlice(env, account, XRP(10'000), USD(10'000));

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // clawback
            env(amm::ammClawback(issuer, account, USD, XRP, USD(1000)));
            env.close();

            // verify tsh hook triggered
            if (features[featureIOUIssuerWeakTSH])
            {
                auto const expected = testStrong ? tshNONE : tshWEAK;
                testTSHStrongWeak(env, expected, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }
    }

    // AMMDeposit
    void
    testAMMDepositTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;
        testcase("amm deposit tsh");

        // otxn: account
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("gw");
            auto const account = Account("alice");
            auto const USD = issuer["USD"];

            env.fund(XRP(30'000), issuer, account);
            env.close();
            env.trust(USD(30'000), account);
            env.close();
            env(pay(issuer, account, USD(30'000)));
            env.close();

            // create AMM
            AMM ammAlice(env, account, XRP(10'000), USD(10'000));

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // deposit
            ammAlice.deposit(account, 10);

            // verify tsh hook triggered
            if (features[featureIOUIssuerWeakTSH])
            {
                auto const expected = testStrong ? tshNONE : tshWEAK;
                testTSHStrongWeak(env, expected, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }
    }

    // AMMVote
    void
    testAMMVoteTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;
        testcase("amm vote tsh");

        // otxn: account
        // tsh issuer
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("gw");
            auto const account = Account("alice");
            auto const USD = issuer["USD"];

            env.fund(XRP(30'000), issuer, account);
            env.close();
            env.trust(USD(30'000), account);
            env.close();
            env(pay(issuer, account, USD(30'000)));
            env.close();

            // create AMM
            AMM ammAlice(env, account, XRP(10'000), USD(10'000));

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // vote
            ammAlice.vote(account, 100);

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }
    }

    // AMMWithdraw
    void
    testAMMWithdrawTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;
        testcase("amm withdraw tsh");

        // otxn: account
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("gw");
            auto const account = Account("alice");
            auto const USD = issuer["USD"];

            env.fund(XRP(30'000), issuer, account);
            env.close();
            env.trust(USD(30'000), account);
            env.close();
            env(pay(issuer, account, USD(30'000)));
            env.close();

            // create AMM
            AMM ammAlice(env, account, XRP(10'000), USD(10'000));

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // withdraw
            ammAlice.withdraw(account, 100);

            // verify tsh hook triggered
            if (features[featureIOUIssuerWeakTSH])
            {
                auto const expected = testStrong ? tshNONE : tshWEAK;
                testTSHStrongWeak(env, expected, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }
    }

    // Check
    // | otxn | tsh | cancel |  create  | cash  |
    // |   A  |  A  |   S    |    S     |  N/A  |
    // |   A  |  D  |   W    |    S     |  N/A  |
    // |   D  |  D  |   S    |   N/A    |   S   |
    // |   D  |  A  |   S    |   N/A    |   S   |
    static uint256
    getCheckIndex(AccountID const& account, std::uint32_t uSequence)
    {
        return keylet::check(account, uSequence).key;
    }

    void
    testCheckCancelTSH(FeatureBitset features)
    {
        testcase("check cancel tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel check
            env(check::cancel(account, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh destination
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // cancel check
            env(check::cancel(account, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // cancel check
            env(check::cancel(dest, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel check
            env(check::cancel(dest, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testCheckCreateTSH(FeatureBitset features)
    {
        testcase("check create tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // create check
            env(check::create(account, dest, XRP(100)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh destination
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // create check
            env(check::create(account, dest, XRP(100)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testCheckCashTSH(FeatureBitset features)
    {
        testcase("check cash tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: dest
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // cash check
            env(check::cash(dest, checkId, XRP(100)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cash check
            env(check::cash(dest, checkId, XRP(100)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh amount issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const account = Account("alice");
            auto const dest = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, dest, gw);
            env.close();
            env.trust(USD(10'000), account);
            env.trust(USD(10'000), dest);
            env.close();
            env(pay(gw, account, USD(10'000)));
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, USD(100)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, gw);

            // set tsh hook
            setTSHHook(env, gw, testStrong);

            // cash check
            env(check::cash(dest, checkId, USD(100)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }
    }

    // ClaimReward
    // | otxn | tsh | claim |
    // |   A  |  A  |   S   |
    // |   A  |  I  |   S   |

    void
    testClaimRewardTSH(FeatureBitset features)
    {
        testcase("claim reward tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account::master;
            env.fund(XRP(1000), account);
            env.close();

            env(hook(issuer, {{hso(jtx::genesis::AcceptHook)}}, 0),
                fee(XRP(1)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // claim reward
            env(reward::claim(account),
                reward::issuer(issuer),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account::master;
            env.fund(XRP(1000), account);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // claim reward
            env(reward::claim(account),
                reward::issuer(issuer),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testClawbackTSH(FeatureBitset _features)
    {
        testcase("clawback tsh");

        using namespace test::jtx;
        using namespace std::literals;

        auto const features = _features | featureMPTokensV1;

        // otxn: IOU issuer
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("gw");
            auto const holder = Account("bob");
            env.fund(XRP(1000), issuer, holder);
            env.close();

            env(fset(issuer, asfAllowTrustLineClawback));
            env.close();

            env.trust(issuer["USD"](1000), holder);
            env(pay(issuer, holder, issuer["USD"](1000)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // clawback
            env(claw(issuer, holder["USD"](1000)), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: IOU issuer
        // tsh holder
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("gw");
            auto const holder = Account("bob");
            env.fund(XRP(1000), issuer, holder);
            env.close();

            env(fset(issuer, asfAllowTrustLineClawback));
            env.close();

            env.trust(issuer["USD"](1000), holder);
            env(pay(issuer, holder, issuer["USD"](1000)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, holder);

            // set tsh hook
            setTSHHook(env, holder, testStrong);

            // clawback
            env(claw(issuer, holder["USD"](1000)), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: MPT issuer
        // tsh holder
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const holder = Account("bob");

            MPTTester mptIssuer(env, issuer, {.holders = {holder}});

            // issuer creates issuance
            mptIssuer.create(
                {.ownerCount = 1, .holderCount = 0, .flags = tfMPTCanClawback});

            // holder creates a MPToken
            mptIssuer.authorize({.account = holder});

            // issuer pays holder 100 tokens
            mptIssuer.pay(issuer, holder, 100);

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, holder);

            // set tsh hook
            setTSHHook(env, holder, testStrong);

            // clawback
            mptIssuer.claw(issuer, holder, 1);

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }
    }

    void
    testCredentialCreateTSH(FeatureBitset features)
    {
        testcase("credential create tsh");

        BEAST_EXPECT(!features[featureCredentials]);
    }

    void
    testCredentialAcceptTSH(FeatureBitset features)
    {
        testcase("credential accept tsh");

        BEAST_EXPECT(!features[featureCredentials]);
    }

    void
    testCredentialDeleteTSH(FeatureBitset features)
    {
        testcase("credential delete tsh");

        BEAST_EXPECT(!features[featureCredentials]);
    }

    // DepositPreauth
    // | otxn  | tsh | preauth |
    // |   A   |  A  |    S    |
    // |   A   |  Au |    S    |

    void
    testDepositPreauthTSH(FeatureBitset features)
    {
        testcase("deposit preauth tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const authed = Account("alice");
            auto const account = Account("bob");
            env.fund(XRP(1000), authed, account);
            env.close();

            // require authorization for deposits.
            env(fset(account, asfDepositAuth));

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // deposit preauth
            env(deposit::auth(account, authed), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh authorize
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const authed = Account("alice");
            auto const account = Account("bob");
            env.fund(XRP(1000), authed, account);
            env.close();

            // require authorization for deposits.
            env(fset(account, asfDepositAuth));

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, authed);

            // set tsh hook
            setTSHHook(env, authed, testStrong);

            // deposit preauth
            env(deposit::auth(account, authed), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testDIDSetTSH(FeatureBitset features)
    {
        testcase("did set tsh");

        BEAST_EXPECT(!features[featureDID]);
    }

    void
    testDIDDeleteTSH(FeatureBitset features)
    {
        testcase("did delete tsh");

        BEAST_EXPECT(!features[featureDID]);
    }

    void
    testEmitFailureTSH(FeatureBitset features)
    {
        testcase("emit failure tsh");

        // pseudo transaction
        pass();
    }

    // Escrow
    // | otxn  | tsh | cancel | cancel(id) |  create  | finish | finish(id)
    // |   A   |  A  |    S   |     S      |     S    |    S   |     S
    // |   A   |  D  |    N   |     N      |     S    |    S   |     S
    // |   D   |  D  |    S   |     S      |    N/A   |    S   |     S
    // |   D   |  A  |    S   |     S      |    N/A   |    S   |     S

    static uint256
    getEscrowIndex(AccountID const& account, std::uint32_t uSequence)
    {
        return keylet::escrow(account, uSequence).key;
    }

    void
    testEscrowCancelTSH(FeatureBitset features)
    {
        testcase("escrow cancel tsh");

        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel escrow
            env(cancel(account, account, seq1), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // cancel escrow
            env(cancel(account, account, seq1), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // cancel escrow
            env(cancel(dest, account, seq1), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel escrow
            env(cancel(dest, account, seq1), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh amount issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const account = Account("alice");
            auto const dest = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, dest, gw);
            env.close();
            env.trust(USD(10'000), account);
            env.trust(USD(10'000), dest);
            env.close();
            env(pay(gw, account, USD(10'000)));
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow(account, dest, USD(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, gw);

            // set tsh hook
            setTSHHook(env, gw, testStrong);

            // cancel escrow
            env(cancel(account, account, seq1), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }
    }

    void
    testEscrowIDCancelTSH(FeatureBitset features)
    {
        testcase("escrow id cancel tsh");

        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel escrow
            Json::Value tx;
            if (!env.current()->rules().enabled(fixXahauV1))
            {
                tx = cancel(account, account, 0);
            }
            else
            {
                tx = cancel(account, account);
            }
            env(tx, escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // cancel escrow
            Json::Value tx;
            if (!env.current()->rules().enabled(fixXahauV1))
            {
                tx = cancel(account, account, 0);
            }
            else
            {
                tx = cancel(account, account);
            }
            env(tx, escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // cancel escrow
            Json::Value tx;
            if (!env.current()->rules().enabled(fixXahauV1))
            {
                tx = cancel(dest, account, 0);
            }
            else
            {
                tx = cancel(dest, account);
            }
            env(tx, escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel escrow
            bool const fixV1 = env.current()->rules().enabled(fixXahauV1);
            Json::Value tx;
            if (!fixV1)
            {
                tx = cancel(dest, account, 0);
            }
            else
            {
                tx = cancel(dest, account);
            }
            env(tx, escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                (fixV1 ? (testStrong ? tshSTRONG : tshSTRONG)
                       : (testStrong ? tshNONE : tshNONE));
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: account
        // tsh amount issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const account = Account("alice");
            auto const dest = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, dest, gw);
            env.close();
            env.trust(USD(10'000), account);
            env.trust(USD(10'000), dest);
            env.close();
            env(pay(gw, account, USD(10'000)));
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow(account, dest, USD(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, gw);

            // set tsh hook
            setTSHHook(env, gw, testStrong);

            // cancel escrow
            Json::Value tx;
            if (!env.current()->rules().enabled(fixXahauV1))
            {
                tx = cancel(account, account, 0);
            }
            else
            {
                tx = cancel(account, account);
            }
            env(tx, escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }
    }

    void
    testEscrowCreateTSH(FeatureBitset features)
    {
        testcase("escrow create tsh");

        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // create escrow
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // create escrow
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh amount issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const account = Account("alice");
            auto const dest = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, dest, gw);
            env.close();
            env.trust(USD(10'000), account);
            env.trust(USD(10'000), dest);
            env.close();
            env(pay(gw, account, USD(10'000)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, gw);

            // set tsh hook
            setTSHHook(env, gw, testStrong);

            // create escrow
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow(account, dest, USD(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }
    }

    void
    testEscrowFinishTSH(FeatureBitset features)
    {
        testcase("escrow finish tsh");

        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // finish escrow
            env(finish(account, account, seq1), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // finish escrow
            env(finish(account, account, seq1), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // finish escrow
            env(finish(dest, account, seq1), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // finish escrow
            env(finish(dest, account, seq1), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh amount issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const account = Account("alice");
            auto const dest = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, dest, gw);
            env.close();
            env.trust(USD(10'000), account);
            env.trust(USD(10'000), dest);
            env.close();
            env(pay(gw, account, USD(10'000)));
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow(account, dest, USD(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, gw);

            // set tsh hook
            setTSHHook(env, gw, testStrong);

            // finish escrow
            env(finish(account, account, seq1), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }
    }

    void
    testEscrowIDFinishTSH(FeatureBitset features)
    {
        testcase("escrow id finish tsh");

        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // finish escrow
            Json::Value tx;
            if (!env.current()->rules().enabled(fixXahauV1))
            {
                tx = finish(account, account, 0);
            }
            else
            {
                tx = finish(account, account);
            }
            env(tx, escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // finish escrow
            bool const fixV1 = env.current()->rules().enabled(fixXahauV1);
            Json::Value tx;
            if (!fixV1)
            {
                tx = finish(account, account, 0);
            }
            else
            {
                tx = finish(account, account);
            }
            env(tx, escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                (fixV1 ? (testStrong ? tshSTRONG : tshSTRONG)
                       : (testStrong ? tshNONE : tshNONE));
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // finish escrow
            Json::Value tx;
            if (!env.current()->rules().enabled(fixXahauV1))
            {
                tx = finish(dest, account, 0);
            }
            else
            {
                tx = finish(dest, account);
            }
            env(tx, escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // finish escrow
            bool const fixV1 = env.current()->rules().enabled(fixXahauV1);
            Json::Value tx;
            if (!fixV1)
            {
                tx = finish(dest, account, 0);
            }
            else
            {
                tx = finish(dest, account);
            }
            env(tx, escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                (fixV1 ? (testStrong ? tshSTRONG : tshSTRONG)
                       : (testStrong ? tshNONE : tshNONE));
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: dest
        // tsh amount issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const account = Account("alice");
            auto const dest = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, dest, gw);
            env.close();
            env.trust(USD(10'000), account);
            env.trust(USD(10'000), dest);
            env.close();
            env(pay(gw, account, USD(10'000)));
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow(account, dest, USD(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, gw);

            // set tsh hook
            setTSHHook(env, gw, testStrong);

            // finish escrow
            bool const fixV1 = env.current()->rules().enabled(fixXahauV1);
            Json::Value tx;
            if (!fixV1)
            {
                tx = finish(dest, account, 0);
            }
            else
            {
                tx = finish(dest, account);
            }
            env(tx, escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }
    }

    void
    testEnableAmendmentTSH(FeatureBitset features)
    {
        testcase("enable amendment tsh");

        // pseudo transaction
        pass();
    }

    // GenesisMint
    // | otxn  | tsh |  mint |
    // |   A   |  A  |   S   |
    // |   A   |  D  |   S   |
    // |   A   |  B  |   W   |

    void
    testGenesisMintTSH(FeatureBitset features)
    {
        testcase("genesis mint tsh");

        // trigger the emitted txn
        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = env.master;
            auto const bene = Account("bob");
            env.fund(XRP(1000), account, bene);
            env.close();

            // burn down the total ledger coins so that genesis mints don't mint
            // above 100B tripping invariant
            env(noop(issuer), fee(XRP(10'000'000ULL)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // set mint hook on master
            env(hook(issuer, {{hso(genesis::MintTestHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            env(invoke::invoke(
                    account,
                    issuer,
                    genesis::makeBlob({
                        {bene.id(),
                         XRP(123).value(),
                         std::nullopt,
                         std::nullopt},
                    })),
                fee(XRP(10)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = env.master;
            auto const bene = Account("bob");
            env.fund(XRP(1000), account, bene);
            env.close();

            // burn down the total ledger coins so that genesis mints don't mint
            // above 100B tripping invariant
            env(noop(issuer), fee(XRP(10'000'000ULL)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook && set mint hook on master
            env(hook(
                    issuer,
                    {{hso(TshHook, overrideFlag),
                      hso(genesis::MintTestHook, overrideFlag)}},
                    0),
                fee(XRP(2)),
                ter(tesSUCCESS));
            env.close();

            env(invoke::invoke(
                    account,
                    issuer,
                    genesis::makeBlob({
                        {bene.id(),
                         XRP(123).value(),
                         std::nullopt,
                         std::nullopt},
                    })),
                fee(XRP(10)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh bene
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = env.master;
            auto const bene = Account("bob");
            env.fund(XRP(1000), account, bene);
            env.close();

            // burn down the total ledger coins so that genesis mints don't mint
            // above 100B tripping invariant
            env(noop(issuer), fee(XRP(10'000'000ULL)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, bene);

            // set tsh hook
            setTSHHook(env, bene, testStrong);

            // set mint hook on master
            env(hook(issuer, {{hso(genesis::MintTestHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            env(invoke::invoke(
                    account,
                    issuer,
                    genesis::makeBlob({
                        {bene.id(),
                         XRP(123).value(),
                         std::nullopt,
                         std::nullopt},
                    })),
                fee(XRP(10)),
                ter(tesSUCCESS));
            env.close();

            // get the emitted txn id
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const emissions = meta[sfHookEmissions.jsonName];
            auto const emission = emissions[0u][sfHookEmission.jsonName];
            auto const txId = emission[sfEmittedTxnID.jsonName];
            env.close();

            // verify tsh hook triggered
            Json::Value params1;
            params1[jss::transaction] = txId;
            auto const jrr1 = env.rpc("json", "tx", to_string(params1));
            auto const meta1 = jrr1[jss::result][jss::meta];
            auto const expected = testStrong ? tshNONE : tshWEAK;
            validateTSHStrongWeak(meta1, expected, __LINE__);
        }
    }

    // Import
    // | otxn  | tsh | import |
    // |   A   |  A  |   S    |
    // |   A   |  I  |   S    |
    // * Issuer cannot import on itself

    void
    testImportTSH(FeatureBitset features)
    {
        testcase("import tsh");

        using namespace test::jtx;
        using namespace std::literals;

        std::vector<std::string> const keys = {
            "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC"
            "1"};

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkVLConfig(
                    21337, keys, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("bob");
            env.fund(XRP(1000), account, issuer);
            env.close();

            // burn down the total ledger coins so that genesis mints don't mint
            // above 100B tripping invariant
            env(noop(env.master), fee(XRP(10'000'000ULL)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // import
            env(import::import(
                    account, import::loadXpop(ImportTCAccountSet::w_seed)),
                import::issuer(issuer),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkVLConfig(
                    21337, keys, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("bob");
            env.fund(XRP(1000), account, issuer);
            env.close();

            // burn down the total ledger coins so that genesis mints don't mint
            // above 100B tripping invariant
            env(noop(env.master), fee(XRP(10'000'000ULL)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // import
            env(import::import(
                    account, import::loadXpop(ImportTCAccountSet::w_seed)),
                import::issuer(issuer),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            bool const fixV2 = env.current()->rules().enabled(fixXahauV2);
            auto const expected =
                (fixV2 ? (testStrong ? tshNONE : tshWEAK)
                       : (testStrong ? tshSTRONG : tshSTRONG));
            testTSHStrongWeak(env, expected, __LINE__);
        }
    }

    // Invoke
    // | otxn  | tsh | invoke |
    // |   A   |  A  |    S   |
    // |   A   |  D  |    S   |

    void
    testInvokeTSH(FeatureBitset features)
    {
        testcase("invoke tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // ttINVOKE
            env(invoke::invoke(account),
                invoke::dest(dest),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // ttINVOKE
            env(invoke::invoke(account),
                invoke::dest(dest),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // from FixNFTokenPageLinks_test.cpp
    // A helper function that generates 96 nfts packed into three pages
    // of 32 each.  Returns a sorted vector of the NFTokenIDs packed into
    // the pages.
    std::vector<uint256>
    genPackedTokens(test::jtx::Env& env, test::jtx::Account const& owner)
    {
        using namespace test::jtx;

        std::vector<uint256> nfts;
        nfts.reserve(96);

        // We want to create fully packed NFT pages.  This is a little
        // tricky since the system currently in place is inclined to
        // assign consecutive tokens to only 16 entries per page.
        //
        // By manipulating the internal form of the taxon we can force
        // creation of NFT pages that are completely full.  This lambda
        // tells us the taxon value we should pass in in order for the
        // internal representation to match the passed in value.
        auto internalTaxon = [this, &env](
                                 Account const& acct,
                                 std::uint32_t taxon) -> std::uint32_t {
            std::uint32_t tokenSeq = [this, &env, &acct]() {
                auto const le = env.le(acct);
                if (BEAST_EXPECT(le))
                    return le->at(~sfMintedNFTokens).value_or(0u);
                return 0u;
            }();

            // If fixNFTokenRemint amendment is on, we must
            // add FirstNFTokenSequence.
            if (env.current()->rules().enabled(fixNFTokenRemint))
                tokenSeq += env.le(acct)
                                ->at(~sfFirstNFTokenSequence)
                                .value_or(env.seq(acct));

            return toUInt32(nft::cipheredTaxon(tokenSeq, nft::toTaxon(taxon)));
        };

        for (std::uint32_t i = 0; i < 96; ++i)
        {
            // In order to fill the pages we use the taxon to break them
            // into groups of 16 entries.  By having the internal
            // representation of the taxon go...
            //   0, 3, 2, 5, 4, 7...
            // in sets of 16 NFTs we can get each page to be fully
            // populated.
            std::uint32_t const intTaxon = (i / 16) + (i & 0b10000 ? 2 : 0);
            uint32_t const extTaxon = internalTaxon(owner, intTaxon);
            nfts.push_back(
                token::getNextID(env, owner, extTaxon, tfTransferable));
            env(token::mint(owner, extTaxon), txflags(tfTransferable));
            env.close();
        }

        // Sort the NFTs so they are listed in storage order, not
        // creation order.
        std::sort(nfts.begin(), nfts.end());

        // Verify that the owner does indeed have exactly three pages
        // of NFTs with 32 entries in each page.
        {
            Json::Value params;
            params[jss::account] = owner.human();
            auto resp = env.rpc("json", "account_objects", to_string(params));

            Json::Value const& acctObjs =
                resp[jss::result][jss::account_objects];

            int pageCount = 0;
            for (Json::UInt i = 0; i < acctObjs.size(); ++i)
            {
                if (BEAST_EXPECT(
                        acctObjs[i].isMember(sfNFTokens.jsonName) &&
                        acctObjs[i][sfNFTokens.jsonName].isArray()))
                {
                    BEAST_EXPECT(acctObjs[i][sfNFTokens.jsonName].size() == 32);
                    ++pageCount;
                }
            }
            // If this check fails then the internal NFT directory logic
            // has changed.
            BEAST_EXPECT(pageCount == 3);
        }
        return nfts;
    };

    void
    testLedgerStateFixTSH(FeatureBitset features)
    {
        testcase("ledger state fix tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong

        // otxn: account
        // tsh owner
        // w/s: weak
        for (auto const& [testStrong, testOtxnAccount] :
             std::vector<std::pair<bool, bool>>{
                 {true, true}, {false, true}, {true, false}, {false, false}})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features - fixNFTokenPageLinks};

            Account const alice("alice");
            Account const bob("bob");
            Account const carol("carol");
            Account const daria("daria");

            Account const& hook = testOtxnAccount ? daria : alice;

            env.fund(XRP(1000), alice, bob, carol, daria);

            //**********************************************************************
            // Step 1A: Create damaged NFToken directories:
            //   o One where there is only one page, but without the final
            //   index.
            //**********************************************************************

            // alice generates three packed pages.
            std::vector<uint256> aliceNFTs = genPackedTokens(env, alice);

            // alice burns all the tokens in the first and last pages.
            for (int i = 0; i < 32; ++i)
            {
                env(token::burn(alice, {aliceNFTs[i]}));
                env.close();
            }
            aliceNFTs.erase(aliceNFTs.begin(), aliceNFTs.begin() + 32);
            for (int i = 0; i < 32; ++i)
            {
                env(token::burn(alice, {aliceNFTs.back()}));
                aliceNFTs.pop_back();
                env.close();
            }

            //**********************************************************************
            // Step 1B: Create damaged NFToken directories:
            //   o One with multiple pages and a missing final page.
            //**********************************************************************

            // bob generates three packed pages.
            std::vector<uint256> bobNFTs = genPackedTokens(env, bob);

            // bob burns all the tokens in the very last page.
            for (int i = 0; i < 32; ++i)
            {
                env(token::burn(bob, {bobNFTs.back()}));
                bobNFTs.pop_back();
                env.close();
            }

            //**********************************************************************
            // Step 1C: Create damaged NFToken directories:
            //   o One with links missing in the middle of the chain.
            //**********************************************************************

            // carol generates three packed pages.
            std::vector<uint256> carolNFTs = genPackedTokens(env, carol);

            // carol sells all of the tokens in the very last page to daria.
            std::vector<uint256> dariaNFTs;
            dariaNFTs.reserve(32);
            for (int i = 0; i < 32; ++i)
            {
                uint256 const offerIndex =
                    keylet::nftoffer(carol, env.seq(carol)).key;
                env(token::createOffer(carol, carolNFTs.back(), XRP(0)),
                    txflags(tfSellNFToken));
                env.close();

                env(token::acceptSellOffer(daria, offerIndex));
                env.close();

                dariaNFTs.push_back(carolNFTs.back());
                carolNFTs.pop_back();
            }

            // At this point carol's NFT directory has the same problem that
            // bob's has: the last page is missing.  Now we make things more
            // complicated by putting the last page back.  carol buys their NFTs
            // back from daria.
            for (uint256 const& nft : dariaNFTs)
            {
                uint256 const offerIndex =
                    keylet::nftoffer(carol, env.seq(carol)).key;
                env(token::createOffer(carol, nft, drops(1)),
                    token::owner(daria));
                env.close();

                env(token::acceptBuyOffer(daria, offerIndex));
                env.close();

                carolNFTs.push_back(nft);
            }

            //**********************************************************************
            // Step 2: Enable the fixNFTokenPageLinks amendment.
            //**********************************************************************
            env.enableFeature(fixNFTokenPageLinks);
            env.close();

            //**********************************************************************
            // Step 3A: Repair the one-page directory (alice's)
            //**********************************************************************

            // The server "remembers" daria's failed nftPageLinks transaction
            // signature.  So we need to advance daria's sequence number before
            // daria can submit a similar transaction.
            env(noop(daria));

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, hook);

            // set tsh hook
            setTSHHook(env, hook, testStrong);

            // daria fixes the links in alice's NFToken directory.
            env(ledgerStateFix::nftPageLinks(daria, alice), fee(XRP(100)));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                testOtxnAccount ? tshSTRONG : (testStrong ? tshNONE : tshWEAK);
            testTSHStrongWeak(env, expected, __LINE__);
        }
    }

    void
    testMPTokenIssuanceCreateTSH(FeatureBitset features)
    {
        testcase("mp token issuance create tsh");

        BEAST_EXPECT(!features[featureMPTokensV1]);
    }

    void
    testMPTokenIssuanceDestroyTSH(FeatureBitset features)
    {
        testcase("mp token issuance destroy tsh");

        BEAST_EXPECT(!features[featureMPTokensV1]);
    }

    void
    testMPTokenIssuanceSetTSH(FeatureBitset features)
    {
        testcase("mp token issuance set tsh");

        BEAST_EXPECT(!features[featureMPTokensV1]);
    }

    void
    testMPTokenAuthorizeTSH(FeatureBitset features)
    {
        testcase("mp token authorize tsh");

        BEAST_EXPECT(!features[featureMPTokensV1]);
    }

    void
    testNFTokenMintTSH(FeatureBitset features)
    {
        testcase("nftoken mint tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // mint nft
            env(token::mint(account), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("bob");
            env.fund(XRP(1000), account, issuer);
            env.close();

            // set NFTokenMinter
            env(token::setMinter(issuer, account));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // mint nft
            env(token::mint(account), token::issuer(issuer), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        //
        // after NFTokenMintOffer amendment
        //
        BEAST_EXPECT(!features[featureNFTokenMintOffer]);
    }

    void
    testNFTokenBurnTSH(FeatureBitset features)
    {
        testcase("nftoken burn tsh");

        using namespace test::jtx;
        using namespace std::literals;

        auto const mintFlags = tfTransferable | tfBurnable;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            auto const nftid = token::getNextID(env, account, 0, mintFlags);
            env(token::mint(account), txflags(mintFlags));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // burn nft
            env(token::burn(account, nftid), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh owner
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            auto const nftid = token::getNextID(env, issuer, 0, mintFlags);
            env(token::mint(issuer), txflags(mintFlags));
            env.close();

            auto const offerIndex =
                keylet::nftoffer(issuer, env.seq(issuer)).key;
            env(token::createOffer(issuer, nftid, XRP(1)),
                txflags(tfSellNFToken),
                fee(XRP(1)));
            env.close();
            env(token::acceptSellOffer(owner, offerIndex), fee(XRP(1)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // burn nft
            env(token::burn(issuer, nftid), token::owner(owner), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                testStrong || !features[featureIOUIssuerWeakTSH] ? tshNONE
                                                                 : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: account
        // tsh issuer
        // nft flag: not tfStrongTSH
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            auto const nftid = token::getNextID(env, issuer, 0, mintFlags);
            env(token::mint(issuer), txflags(mintFlags));
            env.close();

            auto const offerIndex =
                keylet::nftoffer(issuer, env.seq(issuer)).key;
            env(token::createOffer(issuer, nftid, XRP(1)),
                txflags(tfSellNFToken),
                fee(XRP(1)));
            env.close();
            env(token::acceptSellOffer(owner, offerIndex), fee(XRP(1)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // burn nft
            env(token::burn(owner, nftid), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                testStrong || !features[featureIOUIssuerWeakTSH] ? tshNONE
                                                                 : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: account
        // tsh issuer
        // nft flag: tfStrongTSH
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            auto const nftid =
                token::getNextID(env, issuer, 0, mintFlags | tfStrongTSH);
            env(token::mint(issuer), txflags(mintFlags | tfStrongTSH));
            env.close();

            auto const offerIndex =
                keylet::nftoffer(issuer, env.seq(issuer)).key;
            env(token::createOffer(issuer, nftid, XRP(1)),
                txflags(tfSellNFToken),
                fee(XRP(1)));
            env.close();
            env(token::acceptSellOffer(owner, offerIndex), fee(XRP(1)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // burn nft
            env(token::burn(owner, nftid), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testNFTokenCreateOfferTSH(FeatureBitset features)
    {
        testcase("nftoken create offer tsh");

        using namespace test::jtx;
        using namespace std::literals;

        auto const mintFlags = tfTransferable;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), account, owner);
            env.close();

            auto const nftid = token::getNextID(env, account, 0, mintFlags);
            env(token::mint(account), txflags(mintFlags));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // create offer
            env(token::createOffer(account, nftid, XRP(1)),
                txflags(tfSellNFToken),
                fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh owner
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const owner = Account("bob");
            auto const issuer = Account("carol");
            env.fund(XRP(1000), account, owner, issuer);
            env.close();

            auto const nftid = token::getNextID(env, issuer, 0, mintFlags);
            env(token::mint(issuer), txflags(mintFlags));
            env.close();

            auto const offerIndex =
                keylet::nftoffer(issuer, env.seq(issuer)).key;
            env(token::createOffer(issuer, nftid, XRP(1)),
                txflags(tfSellNFToken));
            env.close();
            env(token::acceptSellOffer(owner, offerIndex), fee(XRP(1)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // create buy offer to owner
            env(token::createOffer(account, nftid, XRP(1)),
                token::owner(owner),
                fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: account
        // nft flag: not tfStrongTSH
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const owner = Account("bob");
            auto const issuer = Account("carol");
            env.fund(XRP(1000), account, owner, issuer);
            env.close();

            auto const nftid = token::getNextID(env, issuer, 0, mintFlags);
            env(token::mint(issuer), txflags(mintFlags));
            env.close();

            auto const offerIndex =
                keylet::nftoffer(issuer, env.seq(issuer)).key;
            env(token::createOffer(issuer, nftid, XRP(1)),
                txflags(tfSellNFToken));
            env.close();
            env(token::acceptSellOffer(owner, offerIndex), fee(XRP(1)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // create buy offer to owner
            env(token::createOffer(account, nftid, XRP(1)),
                token::owner(owner),
                fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }
        // otxn: account
        // nft flag: tfStrongTSH
        // tsh issuer
        // w/s: string
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const owner = Account("bob");
            auto const issuer = Account("carol");
            env.fund(XRP(1000), account, owner, issuer);
            env.close();

            auto const nftid =
                token::getNextID(env, issuer, 0, mintFlags | tfStrongTSH);
            env(token::mint(issuer), txflags(mintFlags | tfStrongTSH));
            env.close();

            auto const offerIndex =
                keylet::nftoffer(issuer, env.seq(issuer)).key;
            env(token::createOffer(issuer, nftid, XRP(1)),
                txflags(tfSellNFToken));
            env.close();
            env(token::acceptSellOffer(owner, offerIndex), fee(XRP(1)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // create buy offer to owner
            env(token::createOffer(account, nftid, XRP(1)),
                token::owner(owner),
                fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh destination
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const destination = Account("bob");
            env.fund(XRP(1000), destination, issuer);
            env.close();

            auto const nftid = token::getNextID(env, issuer, 0, mintFlags);
            env(token::mint(issuer), txflags(mintFlags));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, destination);

            // set tsh hook
            setTSHHook(env, destination, testStrong);

            env(token::createOffer(issuer, nftid, XRP(1)),
                token::destination(destination),
                txflags(tfSellNFToken));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }

        // otxn: account
        // tsh amount issuer
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.fund(XRP(1000), issuer, gw);
            env.close();

            env.trust(USD(10000), issuer);
            env.close();
            env(pay(gw, issuer, USD(10000)));
            env.close();

            auto const nftid = token::getNextID(env, issuer, 0, mintFlags);
            env(token::mint(issuer), txflags(mintFlags));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, gw);

            // set tsh hook
            setTSHHook(env, gw, testStrong);

            env(token::createOffer(issuer, nftid, USD(1)),
                txflags(tfSellNFToken));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }
    }

    void
    testNFTokenCancelOfferTSH(FeatureBitset features)
    {
        testcase("nftoken cancel offer tsh");

        using namespace test::jtx;
        using namespace std::literals;

        auto const mintFlags = tfTransferable;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const gw = Account("gw");
            env.fund(XRP(1000), account, gw);
            env.close();

            auto const nftid = token::getNextID(env, account, 0, mintFlags);
            env(token::mint(account), txflags(mintFlags));
            env.close();

            auto const offerIndex =
                keylet::nftoffer(account, env.seq(account)).key;
            env(token::createOffer(account, nftid, XRP(1)),
                txflags(tfSellNFToken));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel offer
            env(token::cancelOffer(account, {offerIndex}), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: issuer
        // tsh owner
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            auto const nftid = token::getNextID(env, issuer, 0, mintFlags);
            env(token::mint(issuer), txflags(mintFlags));
            env.close();

            auto const offerIndex =
                keylet::nftoffer(issuer, env.seq(issuer)).key;
            env(token::createOffer(issuer, nftid, XRP(1)),
                token::destination(owner),
                txflags(tfSellNFToken));
            env.close();
            env(token::acceptSellOffer(owner, offerIndex), fee(XRP(1)));
            env.close();
            auto const offerIndex2 =
                keylet::nftoffer(owner, env.seq(owner)).key;
            env(token::createOffer(owner, nftid, XRP(1)),
                token::destination(buyer),
                txflags(tfSellNFToken));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // cancel offer
            env(token::cancelOffer(buyer, {offerIndex2}), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                testStrong || !features[featureIOUIssuerWeakTSH] ? tshNONE
                                                                 : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: owner
        // tsh destination
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            auto const nftid = token::getNextID(env, issuer, 0, mintFlags);
            env(token::mint(issuer), txflags(mintFlags));
            env.close();

            auto const offerIndex =
                keylet::nftoffer(issuer, env.seq(issuer)).key;
            env(token::createOffer(issuer, nftid, XRP(1)),
                token::destination(owner),
                txflags(tfSellNFToken));
            env.close();
            env(token::acceptSellOffer(owner, offerIndex), fee(XRP(1)));
            env.close();
            auto const offerIndex2 =
                keylet::nftoffer(owner, env.seq(owner)).key;
            env(token::createOffer(owner, nftid, XRP(1)),
                token::destination(buyer),
                txflags(tfSellNFToken));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // cancel offer
            env(token::cancelOffer(owner, {offerIndex2}), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                testStrong || !features[featureIOUIssuerWeakTSH] ? tshNONE
                                                                 : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: account
        // tsh nft issuer
        // w/s: weak (Regardless of tfStrongTSH)
        for (auto const& [testStrong, strongIssuerTSH] :
             std::vector<std::pair<bool, bool>>{
                 {true, true}, {true, false}, {false, true}, {false, false}})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            auto const nftid = token::getNextID(
                env,
                issuer,
                0,
                mintFlags | (strongIssuerTSH ? tfStrongTSH : 0));
            env(token::mint(issuer),
                txflags(mintFlags | (strongIssuerTSH ? tfStrongTSH : 0)));
            env.close();

            auto const offerIndex =
                keylet::nftoffer(issuer, env.seq(issuer)).key;
            env(token::createOffer(issuer, nftid, XRP(1)),
                token::destination(owner),
                txflags(tfSellNFToken));
            env.close();
            env(token::acceptSellOffer(owner, offerIndex), fee(XRP(1)));
            env.close();
            auto const offerIndex2 =
                keylet::nftoffer(owner, env.seq(owner)).key;
            env(token::createOffer(owner, nftid, XRP(1)),
                txflags(tfSellNFToken));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // cancel offer
            env(token::cancelOffer(owner, {offerIndex2}), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                testStrong || !features[featureIOUIssuerWeakTSH] ? tshNONE
                                                                 : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }
    }

    void
    testNFTokenAcceptOfferTSH(FeatureBitset features)
    {
        testcase("nftoken accept offer tsh");

        using namespace test::jtx;
        using namespace std::literals;

        auto const mintFlags = tfTransferable;

        // tsh: account(strong), issuer(weak/strong), owner(strong),
        // destination(strong)

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("bob");
            env.fund(XRP(1000), account, issuer);
            env.close();

            auto const nftid = token::getNextID(env, issuer, 0, mintFlags);
            env(token::mint(issuer), txflags(mintFlags));
            env.close();

            auto const offerIndex =
                keylet::nftoffer(issuer, env.seq(issuer)).key;
            env(token::createOffer(issuer, nftid, XRP(1)),
                txflags(tfSellNFToken));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // accept offer
            env(token::acceptSellOffer(account, offerIndex), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // nft flag: not tfStrongTSH
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            auto const nftid = token::getNextID(env, issuer, 0, mintFlags);
            env(token::mint(issuer), txflags(mintFlags));
            env.close();

            auto const offerIndex =
                keylet::nftoffer(issuer, env.seq(issuer)).key;
            env(token::createOffer(issuer, nftid, XRP(1)),
                txflags(tfSellNFToken));
            env.close();

            env(token::acceptSellOffer(owner, offerIndex), fee(XRP(1)));
            env.close();
            auto const offerIndex2 =
                keylet::nftoffer(buyer, env.seq(buyer)).key;
            env(token::createOffer(buyer, nftid, XRP(1)), token::owner(owner));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // accept offer
            env(token::acceptBuyOffer(owner, offerIndex2), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                testStrong || !features[featureIOUIssuerWeakTSH] ? tshNONE
                                                                 : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: owner
        // nft flag: tfStrongTSH
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            auto const nftid =
                token::getNextID(env, issuer, 0, mintFlags | tfStrongTSH);
            env(token::mint(issuer), txflags(mintFlags | tfStrongTSH));
            env.close();

            auto const offerIndex =
                keylet::nftoffer(issuer, env.seq(issuer)).key;
            env(token::createOffer(issuer, nftid, XRP(1)),
                txflags(tfSellNFToken));
            env.close();

            env(token::acceptSellOffer(owner, offerIndex), fee(XRP(1)));
            env.close();
            auto const offerIndex2 =
                keylet::nftoffer(buyer, env.seq(buyer)).key;
            env(token::createOffer(buyer, nftid, XRP(1)), token::owner(owner));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // accept offer
            env(token::acceptBuyOffer(owner, offerIndex2), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};
            auto const account = Account("alice");
            auto const destination = Account("bob");
            env.fund(XRP(1000), account, destination);
            env.close();

            auto const nftid = token::getNextID(env, account, 0, mintFlags);
            env(token::mint(account), txflags(mintFlags));
            env.close();

            auto const offerIndex =
                keylet::nftoffer(account, env.seq(account)).key;
            env(token::createOffer(account, nftid, XRP(1)),
                txflags(tfSellNFToken));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // accept offer
            env(token::acceptSellOffer(destination, offerIndex), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh destination
        // w/s: strong
        {
            // If sfDestination holds an Offer, it meets the sfOwner criteria.
            // If it doesn't hold an Offer, it falls under the Otxn Account.
            // Because of this, we might not be able to run TSH tests regarding
            // sfDestination.
        }
    }

    void
    testNFTokenModifyTSH(FeatureBitset features)
    {
        testcase("nftoken modify tsh");

        BEAST_EXPECT(!features[featureDynamicNFT]);
    }

    // Offer
    // | otxn  | tsh |  cancel | create |
    // |   A   |  A  |    S    |    S   |
    // |   A   |  C  |   N/A   |    N   |

    void
    testOfferCancelTSH(FeatureBitset features)
    {
        testcase("offer cancel tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const gw = Account{"gateway"};
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, gw);
            env.close();

            // gw create offer
            env(offer(gw, USD(1000), XRP(1000)));
            env.close();

            // create offer
            auto const offerSeq = env.seq(account);
            env(offer(account, USD(1000), XRP(1000)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel offer
            env(offer_cancel(account, offerSeq), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testOfferCreateTSH(FeatureBitset features)
    {
        testcase("offer create tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const gw = Account{"gateway"};
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, gw);
            env.close();

            // gw create offer
            env(offer(gw, USD(1000), XRP(1000)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // create offer
            env(offer(account, USD(1000), XRP(1000)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh cross
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const cross = Account("bob");
            auto const gw = Account{"gateway"};
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, cross, gw);
            env.close();
            env.trust(USD(100000), account);
            env.trust(USD(100000), cross);
            env.close();
            env(pay(gw, cross, USD(10000)));
            env.close();

            // gw create offer
            env(offer(cross, XRP(1000), USD(1000)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, cross);

            // set tsh hook
            setTSHHook(env, cross, testStrong);

            // create offer
            env(offer(account, USD(1000), XRP(1000)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: account
        // tsh cross issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("carol");
            auto const cross = Account("bob");
            auto const gw = Account{"alice"};

            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, cross, gw);
            env.close();
            env.trust(USD(100000), account);
            env.trust(USD(100000), cross);
            env.close();
            env(pay(gw, cross, USD(10000)));
            env.close();

            // cross create offer
            env(offer(cross, XRP(1000), USD(1000)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, gw);

            // set tsh hook
            setTSHHook(env, gw, testStrong);

            // create offer
            env(offer(account, USD(1000), XRP(1000)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }
    }

    void
    testOracleSetTSH(FeatureBitset features)
    {
        testcase("oracle set tsh");

        using namespace test::jtx;
        using namespace std::literals;
        using namespace oracle;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // set oracle
            env.close(std::chrono::seconds(300));
            Oracle oracle(
                env,
                {.owner = account,
                 .series = {{"XRP", "USD", 740, 1}},
                 .fee = 10000});

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testOracleDeleteTSH(FeatureBitset features)
    {
        testcase("oracle delete tsh");

        using namespace test::jtx;
        using namespace std::literals;
        using namespace oracle;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            // set oracle
            env.close(std::chrono::seconds(300));
            Oracle oracle(
                env,
                {
                    .owner = account,
                    .series = {{"XRP", "USD", 740, 1}},
                });

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // delete oracle
            oracle.remove(oracle::RemoveArg{
                .documentID = oracle.documentID(),
                .fee = 10000,
            });

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // Payment
    // | otxn  | tsh | payment |
    // |   A   |  A  |    S    |
    // |   A   |  D  |    S    |
    // |   A   |  C  |    W    |

    void
    testPaymentTSH(FeatureBitset features)
    {
        testcase("payment tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // payment
            env(pay(account, dest, XRP(1)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // payment
            env(pay(account, dest, XRP(1)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh cross
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const cross = Account("bob");
            auto const dest = Account("carol");
            auto const gw = Account{"gateway"};
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, cross, dest, gw);
            env.close();

            // setup rippling
            auto const USDA = account["USD"];
            auto const USDB = cross["USD"];
            auto const USDC = dest["USD"];
            env.trust(USDA(10), cross);
            env.trust(USDB(10), dest);

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, cross);

            // set tsh hook
            setTSHHook(env, cross, testStrong);

            // payment
            env(pay(account, dest, USDB(10)), paths(USDA), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: account
        // tsh cross issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const cross = Account("bob");
            auto const dest = Account("carol");
            auto const gw = Account{"gateway"};
            env.fund(XRP(1000), account, cross, dest);
            env.close();

            // setup rippling
            auto const USDA = account["USD"];
            auto const USDB = cross["USD"];
            auto const USDC = dest["USD"];
            env.trust(USDA(10), cross);
            env.trust(USDB(10), dest);

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, cross);

            // set tsh hook
            setTSHHook(env, cross, testStrong);

            // payment
            env(pay(account, dest, USDB(10)), paths(USDA), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }
    }

    // PaymentChannel
    // | otxn  | tsh | claim |  create  |  fund  |
    // |   A   |  A  |   S   |    S     |    S   |
    // |   A   |  D  |   W   |    S     |    W   |
    // |   D   |  D  |   S   |   N/A    |   N/A  |
    // |   D   |  A  |   S   |   N/A    |   N/A  |

    static uint256
    channel(
        jtx::Account const& account,
        jtx::Account const& dst,
        std::uint32_t seqProxyValue)
    {
        auto const k = keylet::payChan(account, dst, seqProxyValue);
        return k.key;
    }

    static Buffer
    signClaimAuth(
        PublicKey const& pk,
        SecretKey const& sk,
        uint256 const& channel,
        STAmount const& authAmt)
    {
        Serializer msg;
        serializePayChanAuthorization(msg, channel, authAmt.xrp());
        return sign(pk, sk, msg.slice());
    }

    void
    testPaymentChannelClaimTSH(FeatureBitset features)
    {
        testcase("payment channel claim tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            env(paychan::claim(account, chan, reqBal, authAmt),
                txflags(tfClose),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            env(paychan::claim(account, chan, reqBal, authAmt),
                txflags(tfClose),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            auto const sig =
                signClaimAuth(account.pk(), account.sk(), chan, authAmt);
            env(paychan::claim(
                    dest, chan, reqBal, authAmt, Slice(sig), account.pk()),
                txflags(tfClose),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            auto const sig =
                signClaimAuth(account.pk(), account.sk(), chan, authAmt);
            env(paychan::claim(
                    dest, chan, reqBal, authAmt, Slice(sig), account.pk()),
                txflags(tfClose),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh amount issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, dest, gw);
            env.close();
            env.trust(USD(10'000), account);
            env.trust(USD(10'000), dest);
            env.close();
            env(pay(gw, account, USD(10'000)));
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, USD(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, gw);

            // set tsh hook
            setTSHHook(env, gw, testStrong);

            auto const delta = USD(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + USD(1);

            // claim paychannel
            env(paychan::claim(account, chan, reqBal, authAmt),
                txflags(tfClose),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }
    }

    void
    testPaymentChannelCreateTSH(FeatureBitset features)
    {
        testcase("payment channel create tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh amount issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, dest, gw);
            env.close();
            env.trust(USD(10'000), account);
            env.trust(USD(10'000), dest);
            env.close();
            env(pay(gw, account, USD(10'000)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, gw);

            // set tsh hook
            setTSHHook(env, gw, testStrong);

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            env(paychan::create(account, dest, USD(10), settleDelay, pk),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }
    }

    void
    testPaymentChannelFundTSH(FeatureBitset features)
    {
        testcase("payment channel fund tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // fund paychannel
            env(paychan::fund(account, chan, XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // fund paychannel
            env(paychan::fund(account, chan, XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: account
        // tsh amount issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, dest, gw);
            env.close();
            env.trust(USD(10'000), account);
            env.trust(USD(10'000), dest);
            env.close();
            env(pay(gw, account, USD(10'000)));
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, USD(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, gw);

            // set tsh hook
            setTSHHook(env, gw, testStrong);

            // fund paychannel
            env(paychan::fund(account, chan, USD(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }
    }

    void
    testPermissionedDomainSetTSH(FeatureBitset features)
    {
        testcase("permissioned domain set tsh");

        BEAST_EXPECT(!features[featurePermissionedDomains]);
    }

    void
    testPermissionedDomainDeleteTSH(FeatureBitset features)
    {
        testcase("permissioned domain delete tsh");

        BEAST_EXPECT(!features[featurePermissionedDomains]);
    }

    void
    testSetFeeTSH(FeatureBitset features)
    {
        testcase("set fee tsh");

        // pseudo transaction
        pass();
    }

    // SetHook
    // | otxn  | tsh | set |
    // |   A   |  A  |  S  |
    void
    testSetHookTSH(FeatureBitset features)
    {
        testcase("set hook tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            auto hook1 = hso(TshHook, overrideFlag);
            hook1[jss::HookOn] =
                "00000000000000000000000000000000000000000000000000000000004000"
                "00";
            env(hook(account, {{hook1}}, 0), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // SetRegularKey
    // | otxn  | tsh | srk |
    // |   A   |  A  |  S  |
    // |   A   |  D  |  S  |
    void
    testSetRegularKeyTSH(FeatureBitset features)
    {
        testcase("set regular key tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // set regular key
            env(regkey(account, dest), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // set regular key
            env(regkey(account, dest), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testSetRemarksTSH(FeatureBitset features)
    {
        testcase("set remarks tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // set remarks
            env(remarks::setRemarks(
                    account,
                    keylet::account(account.id()).key,
                    {{"CAFE", "DEADBEEF", 0}}),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh: object issuer
        // w/s:none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("issuer");
            env.fund(XRP(1000), account, issuer);
            env.close();

            std::string const uri("https://example.com");
            env(remit::remit(issuer, account), remit::uri(uri), fee(XRP(1)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // set remarks
            env(remarks::setRemarks(
                    issuer,
                    keylet::uritoken(issuer, Blob(uri.begin(), uri.end())).key,
                    {{"CAFE", "DEADBEEF", 0}}),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }
    }

    // SignerListSet
    // | otxn  | tsh | sls |
    // |   A   |  A  |  S  |
    // |   A   |  S  |  S  |
    void
    testSignerListSetTSH(FeatureBitset features)
    {
        testcase("signer list set tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const signer1 = Account{"bob"};
            auto const signer2 = Account{"carol"};
            env.fund(XRP(1000), account, signer1, signer2);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // signer list set
            env(signers(account, 2, {{signer1, 1}, {signer2, 1}}),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh signer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const signer1 = Account{"bob"};
            auto const signer2 = Account{"carol"};
            env.fund(XRP(1000), account, signer1, signer2);
            env.close();

            // set tsh hook
            env(hook(signer1, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, signer2);

            // set tsh hook
            setTSHHook(env, signer2, testStrong);

            // signer list set
            env(signers(account, 2, {{signer1, 1}, {signer2, 1}}),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution1 = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution1[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution1[sfHookReturnString.jsonName] == "00000000");
            auto const execution2 = executions[1u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution2[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution2[sfHookReturnString.jsonName] == "00000000");
        }
    }

    // Ticket
    // | otxn  | tsh | create |
    // |   A   |  A  |   S    |
    void
    testTicketCreateTSH(FeatureBitset features)
    {
        testcase("ticket create tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // ticket create
            env(ticket::create(account, 2), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // TrustSet
    // | otxn  | tsh | trustset |
    // |   A   |  A  |     S    |
    // |   A   |  I  |     W    |
    void
    testTrustSetTSH(FeatureBitset features)
    {
        testcase("trust set tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account{"gw"};
            auto const USD = issuer["USD"];
            env.fund(XRP(1000), account, issuer);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // trust set
            env(trust(account, USD(1000)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account{"gw"};
            auto const USD = issuer["USD"];
            env.fund(XRP(1000), account, issuer);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // trust set
            env(trust(account, USD(1000)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }
    }

    void
    testUNLModifyTSH(FeatureBitset features)
    {
        testcase("unl modify tsh");

        // pseudo transaction
        pass();
    }

    void
    testUNLReportTSH(FeatureBitset features)
    {
        testcase("unl report tsh");

        // pseudo transaction
        pass();
    }

    // | otxn | tfBurnable | tsh |   mint |  burn  |  buy  |  sell  | cancel
    // |   O  |    false   |  O  |   N/A  |   S    |  N/A  |   S    |   S
    // |   O  |    false   |  I  |   N/A  |   N    |  N/A  |   W    |   N/A
    // |   O  |    false   |  B  |   N/A  |   N/A  |  N/A  |   N    |   N
    // |   O  |    true    |  B  |   N/A  |   N/A  |  N/A  |   N    |   N
    // |   O  |    true    |  O  |   N/A  |   S    |  N/A  |   S    |   S
    // |   O  |    true    |  I  |   N/A  |   N    |  N/A  |   S    |   N/A
    // |   I  |    false   |  O  |   N/A  |   N/A  |  N/A  |   N/A  |   N/A
    // |   I  |    false   |  I  |   S    |   N/A  |  N/A  |   N/A  |   N/A
    // |   I  |    false   |  B  |   S    |   N/A  |  N/A  |   N/A  |   N/A
    // |   I  |    true    |  O  |   N/A  |   N    |  N/A  |   N/A  |   N/A
    // |   I  |    true    |  I  |   S    |   S    |  N/A  |   N/A  |   N/A
    // |   I  |    true    |  B  |   S    |   N/A  |  N/A  |   N/A  |   N/A
    // |   B  |    true    |  O  |   N/A  |   N/A  |  S    |   N/A  |   N/A
    // |   B  |    true    |  B  |   N/A  |   N/A  |  S    |   N/A  |   N/A
    // |   B  |    false   |  I  |   N/A  |   N/A  |  W    |   N/A  |   N/A
    // |   B  |    true    |  I  |   N/A  |   N/A  |  S    |   N/A  |   N/A

    void
    testURITokenMintTSH(FeatureBitset features)
    {
        testcase("uritoken mint tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: issuer
        // flag: not burnable
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: issuer
        // flag: not burnable
        // tsh buyer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            bool const fixV2 = env.current()->rules().enabled(fixXahauV2);
            auto const expected =
                (fixV2 ? (testStrong ? tshSTRONG : tshSTRONG)
                       : (testStrong ? tshNONE : tshNONE));
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: issuer
        // flag: burnable
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: issuer
        // flag: burnable
        // tsh buyer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            bool const fixV2 = env.current()->rules().enabled(fixXahauV2);
            auto const expected =
                (fixV2 ? (testStrong ? tshSTRONG : tshSTRONG)
                       : (testStrong ? tshNONE : tshNONE));
            testTSHStrongWeak(env, expected, __LINE__);
        }
    }

    void
    testURITokenBurnTSH(FeatureBitset features)
    {
        testcase("uritoken burn tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: owner
        // flag: not burnable
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // flag: not burnable
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            bool const fixV1 = env.current()->rules().enabled(fixXahauV1);
            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const expected =
                (fixV1
                     ? (testStrong ? tshNONE
                                   : (withIOUIssuerWeakTSH ? tshWEAK : tshNONE))
                     : (testStrong ? tshSTRONG : tshSTRONG));
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: owner
        // flag: burnable
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // flag: burnable
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            bool const fixV1 = env.current()->rules().enabled(fixXahauV1);
            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);
            auto const expected =
                (fixV1
                     ? (testStrong ? tshNONE
                                   : (withIOUIssuerWeakTSH ? tshWEAK : tshNONE))
                     : (testStrong ? tshSTRONG : tshSTRONG));
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: issuer
        // flag: burnable
        // tsh owner
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // ttURITOKEN_BURN
            env(uritoken::burn(issuer, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            bool const fixV1 = env.current()->rules().enabled(fixXahauV1);
            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const expected =
                (fixV1
                     ? (testStrong ? tshNONE
                                   : (withIOUIssuerWeakTSH ? tshWEAK : tshNONE))
                     : (testStrong ? tshSTRONG : tshSTRONG));
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: issuer
        // flag: burnable
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // ttURITOKEN_BURN
            env(uritoken::burn(issuer, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testURITokenBuyTSH(FeatureBitset features)
    {
        testcase("uritoken buy tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: buyer
        // flag: not burnable
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // buy uritoken
            env(uritoken::buy(buyer, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: buyer
        // flag: burnable
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // buy uritoken
            env(uritoken::buy(buyer, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: buyer
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // buy uritoken
            env(uritoken::buy(buyer, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: buyer
        // tsh buyer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // buy uritoken
            env(uritoken::buy(buyer, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: buyer
        // tsh amount issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const issuer = Account("alice");
            auto const buyer = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.fund(XRP(1000), issuer, buyer, gw);
            env.close();
            env.trust(USD(10'000), buyer);
            env.close();
            env(pay(gw, buyer, USD(10'000)));
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(USD(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, gw);

            // set tsh hook
            setTSHHook(env, gw, testStrong);

            // buy uritoken
            env(uritoken::buy(buyer, hexid),
                uritoken::amt(USD(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }
    }

    void
    testURITokenCancelSellOfferTSH(FeatureBitset features)
    {
        testcase("uritoken cancel sell offer tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: owner
        // flag: not burnable
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // flag: not burnable
        // tsh buyer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }

        // otxn: owner
        // flag: burnable
        // tsh buyer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }

        // otxn: owner
        // flag: burnable
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testURITokenCreateSellOfferTSH(FeatureBitset features)
    {
        testcase("uritoken create sell offer tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: owner
        // flag: not burnable
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // flag: not burnable
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: owner
        // flag: not burnable
        // tsh buyer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // flag: not burnable
        // tsh buyer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // flag: burnable
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // flag: burnable
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testXChainCreateClaimIDTSH(FeatureBitset features)
    {
        testcase("xchain create claim id tsh");

        BEAST_EXPECT(!features[featureXChainBridge]);
    }

    void
    testXChainCommitTSH(FeatureBitset features)
    {
        testcase("xchain commit tsh");

        BEAST_EXPECT(!features[featureXChainBridge]);
    }

    void
    testXChainClaimTSH(FeatureBitset features)
    {
        testcase("xchain claim tsh");

        BEAST_EXPECT(!features[featureXChainBridge]);
    }

    void
    testXChainAccountCreateCommitTSH(FeatureBitset features)
    {
        testcase("xchain account create commit tsh");

        BEAST_EXPECT(!features[featureXChainBridge]);
    }

    void
    testXChainAddClaimAttestationTSH(FeatureBitset features)
    {
        testcase("xchain add claim attestation tsh");

        BEAST_EXPECT(!features[featureXChainBridge]);
    }

    void
    testXChainAddAccountCreateAttestationTSH(FeatureBitset features)
    {
        testcase("xchain add account create attestation tsh");

        BEAST_EXPECT(!features[featureXChainBridge]);
    }

    void
    testXChainModifyBridgeTSH(FeatureBitset features)
    {
        testcase("xchain modify bridge tsh");

        BEAST_EXPECT(!features[featureXChainBridge]);
    }

    void
    testXChainCreateBridgeTSH(FeatureBitset features)
    {
        testcase("xchain create bridge tsh");

        BEAST_EXPECT(!features[featureXChainBridge]);
    }

    void
    testEmittedTxnReliability(FeatureBitset features)
    {
        testcase("emitted txn reliability");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{
            *this,
            network::makeNetworkConfig(21337, "10", "1000000", "200000"),
            features};

        auto const account = Account("alice");
        auto const dest = Account("bob");
        env.fund(XRP(1000), account, dest);
        env.close();

        // set emit hook
        setCallbackHook(env, account, true);

        // ttINVOKE
        env(invoke::invoke(account), fee(XRP(2)), ter(tesSUCCESS));
        env.close();

        Blob txBlob;
        auto meta = env.meta();
        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        for (auto const& node : meta->getFieldArray(sfAffectedNodes))
        {
            SField const& metaType = node.getFName();
            uint16_t nodeType = node.getFieldU16(sfLedgerEntryType);
            if (metaType == sfCreatedNode && nodeType == ltEMITTED_TXN)
            {
                auto const& nf = const_cast<ripple::STObject&>(node)
                                     .getField(sfNewFields)
                                     .downcast<STObject>();
                auto const& et = const_cast<ripple::STObject&>(nf)
                                     .getField(sfEmittedTxn)
                                     .downcast<STObject>();

                txBlob = et.getSerializer().getData();
                break;
            }
        }
        env.close();

        auto const preDest = env.balance(dest);
        bool const withFix = env.current()->rules().enabled(fixXahauV2);

        env.app().openLedger().modify([&](OpenView& view, beast::Journal j) {
            auto const tx =
                std::make_unique<STTx>(Slice{txBlob.data(), txBlob.size()});
            auto result =
                ripple::apply(env.app(), view, *tx, tapNONE, env.journal);

            bool const applyResult = withFix ? false : true;
            if (withFix)
            {
                BEAST_EXPECT(result.ter == tefNONDIR_EMIT);
            }
            else
            {
                BEAST_EXPECT(result.ter == tesSUCCESS);
            }
            BEAST_EXPECT(result.applied == applyResult);
            return result.applied;
        });

        env.close();

        auto const postDest = env.balance(dest);
        auto const postValue = withFix ? XRP(0) : XRP(1);
        BEAST_EXPECT(postDest == preDest + postValue);

        for (size_t i = 0; i < 4; i++)
        {
            Json::Value params1;
            params1[jss::tx_blob] = strHex(Slice{txBlob.data(), txBlob.size()});
            auto const jrr1 = env.rpc("json", "inject", to_string(params1));
            env.close();
        }

        auto const postDest1 = env.balance(dest);
        auto const postValue1 = withFix ? XRP(0) : XRP(2);
        BEAST_EXPECT(postDest1 == postDest + postValue1);
    }

    void
    testEmittedFlags(FeatureBitset features)
    {
        testcase("emitted flags");

        using namespace test::jtx;
        using namespace std::literals;

        // weak
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account{"gw"};
            auto const USD = issuer["USD"];
            env.fund(XRP(1000), account, issuer);
            env.close();

            addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, false);

            // trust set
            env(trust(account, USD(1000)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            validateTSHFlags(env, meta, 0);
        }

        // strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account{"gw"};
            auto const USD = issuer["USD"];
            env.fund(XRP(1000), account, issuer);
            env.close();

            // set tsh hook
            setTSHHook(env, account, true);

            // trust set
            env(trust(account, USD(1000)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            validateTSHFlags(env, meta, 1);
        }

        // callback
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            // set tsh hook
            setCallbackHook(env, account, true);

            // invoke
            env(invoke::invoke(account), fee(XRP(2)), ter(tesSUCCESS));
            env.close();

            // get the emitted txn id
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const emissions = meta[sfHookEmissions.jsonName];
            auto const emission = emissions[0u][sfHookEmission.jsonName];
            auto const txId = emission[sfEmittedTxnID.jsonName];
            env.close();

            // verify tsh hook triggered
            Json::Value params1;
            params1[jss::transaction] = txId;
            auto const jrr1 = env.rpc("json", "tx", to_string(params1));
            auto const meta1 = jrr1[jss::result][jss::meta];
            validateTSHFlags(env, meta1, 2);
        }

        // weak callback
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account{"gw"};
            auto const USD = issuer["USD"];
            env.fund(XRP(1000), account, issuer);
            env.close();

            addWeakTSH(env, issuer);

            // set tsh hook
            setCallbackHook(env, issuer, false);

            // trust set
            env(trust(account, USD(1000)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // get the emitted txn id
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const emissions = meta[sfHookEmissions.jsonName];
            auto const emission = emissions[0u][sfHookEmission.jsonName];
            auto const txId = emission[sfEmittedTxnID.jsonName];
            env.close();

            // verify tsh hook triggered
            Json::Value params1;
            params1[jss::transaction] = txId;
            auto const jrr1 = env.rpc("json", "tx", to_string(params1));
            auto const meta1 = jrr1[jss::result][jss::meta];
            validateTSHFlags(env, meta1, 2);
        }

        // again as weak
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            // set tsh hook
            setTSHHook(env, account, true);

            // invoke
            auto tx = invoke::invoke(account);
            tx[jss::HookParameters] = Json::Value{Json::arrayValue};
            tx[jss::HookParameters][0U] = Json::Value{};
            tx[jss::HookParameters][0U][jss::HookParameter] = Json::Value{};
            tx[jss::HookParameters][0U][jss::HookParameter]
              [jss::HookParameterName] = "414157";
            tx[jss::HookParameters][0U][jss::HookParameter]
              [jss::HookParameterValue] = "01";
            env(tx, fee(XRP(2)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            validateTSHFlags(env, meta, 5);
        }
    }

    // Remit
    // | otxn  | tsh | remit w/amt |
    // |   A   |  A  |     S       |
    // |   A   |  D  |     S       |
    // |   A   |  I  |     W       |

    // | otxn  | tsh | burnable |  remit w/uri  |
    // |   A   |  I  |    F     |       W       |
    // |   A   |  I  |    T     |       S       |

    void
    testRemitTSH(FeatureBitset features)
    {
        testcase("remit tsh");

        using namespace test::jtx;
        using namespace std::literals;
        /*
            sfAmounts
        */

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // payment
            env(remit::remit(account, dest), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // payment
            env(remit::remit(account, dest), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh inform
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            auto const inform = Account("carol");
            env.fund(XRP(1000), account, dest, inform);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, inform);

            // set tsh hook
            setTSHHook(env, inform, testStrong);

            // payment
            env(remit::remit(account, dest),
                remit::inform(inform),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: account
        // tsh iou issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};
            bool const withIOUIssuerWeakTSH =
                env.current()->rules().enabled(featureIOUIssuerWeakTSH);

            auto const account = Account("alice");
            auto const dest = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, dest, gw);
            env.close();
            env.trust(USD(100'000), account);
            env.close();
            env(pay(gw, account, USD(10'000)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, gw);

            // set tsh hook
            setTSHHook(env, gw, testStrong);

            // payment
            env(remit::remit(account, dest),
                remit::amts({USD(100)}),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            if (withIOUIssuerWeakTSH && !testStrong)
            {
                testTSHStrongWeak(env, tshWEAK, __LINE__);
            }
            else
            {
                testTSHStrongWeak(env, tshNONE, __LINE__);
            }
        }

        /*
            sfURITokenIDs
        */

        // otxn: account
        // tsh issuer
        // burnable: true
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            auto const issuer = Account("carol");
            env.fund(XRP(1000), account, dest, issuer);
            env.close();

            // mint uritoken
            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            env(uritoken::mint(issuer, uri),
                txflags(tfBurnable),
                ter(tesSUCCESS));

            // sell uritoken
            env(uritoken::sell(issuer, strHex(tid)),
                uritoken::amt(XRP(1)),
                uritoken::dest(account),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(account, strHex(tid)),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // payment
            env(remit::remit(account, dest),
                remit::token_ids({strHex(tid)}),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshSTRONG : tshSTRONG;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: account
        // tsh issuer
        // burnable: false
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            auto const issuer = Account("carol");
            env.fund(XRP(1000), account, dest, issuer);
            env.close();

            // mint uritoken
            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            env(uritoken::mint(issuer, uri), ter(tesSUCCESS));

            // sell uritoken
            env(uritoken::sell(issuer, strHex(tid)),
                uritoken::amt(XRP(1)),
                uritoken::dest(account),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(account, strHex(tid)),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // payment
            env(remit::remit(account, dest),
                remit::token_ids({strHex(tid)}),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }
    }

    // CronSet
    // | otxn | tsh | cset |
    // |   A  |  A  |  S   |
    void
    testCronSetTSH(FeatureBitset features)
    {
        testcase("cron set tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cron set
            env(cron::set(account),
                cron::startTime(0),
                cron::delay(100),
                cron::repeat(1),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // | otxn | tsh | cron |
    // |   -  |  O  |  W   |
    void
    testCronTSH(FeatureBitset features)
    {
        testcase("cron tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: -
        // tsh owner
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            auto const baseTime =
                env.current()->parentCloseTime().time_since_epoch().count();
            // cron set
            env(cron::set(account),
                cron::startTime(baseTime + 100),
                cron::delay(100),
                cron::repeat(1),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // proceed ledger
            env.close(100s);

            // close ledger
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            auto const txs = env.closed()->txs;
            BEAST_EXPECT(std::distance(txs.begin(), txs.end()) == 1);
            auto const tx = txs.begin()->first;
            BEAST_EXPECT(tx->getTxnType() == ttCRON);
            testTSHStrongWeak(env, tx->getTransactionID(), expected, __LINE__);
        }
    }
    void
    testEmissionOrdering(FeatureBitset features)
    {
        testcase("emission ordering");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{
            *this,
            network::makeNetworkConfig(21337, "10", "1000000", "200000"),
            features};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const dave = Account("dave");
        auto const elsa = Account("elsa");
        auto const frank = Account("frank");
        auto const grace = Account("grace");
        auto const heidi = Account("heidi");
        auto const ivan = Account("ivan");
        auto const judy = Account("judy");
        env.fund(
            XRP(1000),
            alice,
            bob,
            carol,
            dave,
            elsa,
            frank,
            grace,
            heidi,
            ivan,
            judy);
        env.close();

        // set tsh hook
        env(hook(alice, {{hso(EmitTenHook, overrideFlag)}}, 0),
            fee(XRP(2)),
            ter(tesSUCCESS));
        env.close();

        // invoke
        env(invoke::invoke(alice), fee(XRP(2)), ter(tesSUCCESS));
        env.close();

        // validate the emitted txn ids
        std::vector<std::string> const txIds = {
            "9610F73CDD6590EB6B3C82E5EC55D4B4C80CD7128B98AA556F7EC9DD96AE7056",
            "2F4582A29272390C0C25A80D4A3BCE5A14ACE6D86D8D0CB2C57719EB6FA881AE",
            "89A301CFEF0DD781AB9032A6A2DCE0937BC0119D2CDD06033B8B2FD80968E519",
            "DD8721B59024E168480B4DF8F8E93778601F0BD2E77FC991F3DA1182F5AD8B1E",
            "5D735C2EE3CB8289F8E11621FDC9565F9D6D67F3AE59D65332EACE591D67945F",
            "F02470E01731C968881AF4CBDEC90BB9E1F7AB0BE1CC22AF15451FB6D191096D",
            "8AD65E541DECD49B1693F8C17DFD8A2B906F49C673C4FD2034FF772E2BE50C30",
            "9F225229059CCC6257814D03C107884CF588C1C246A89ADFC16E50DF671B834C",
            "13C2A54A14BADF3648CED05175E1CCAD713F7E5EA56D9735CF8813CD5551F281",
            "87C60F41A96554587CED289F83F52DEE3CF670EEB189B067E6066B9A06056ADF",
        };
        Json::Value params;
        params[jss::transaction] =
            env.tx()->getJson(JsonOptions::none)[jss::hash];
        auto const jrr = env.rpc("json", "tx", to_string(params));
        auto const meta = jrr[jss::result][jss::meta];
        auto const emissions = meta[sfHookEmissions.jsonName];
        for (size_t i = 0; i <= 9; i++)
        {
            auto const emitted = emissions[i][sfHookEmission.jsonName];
            BEAST_EXPECT(emitted[sfEmittedTxnID.jsonName] == txIds[i]);
        }
        env.close();

        // NOTE: emitted txns are emitted in reverse
        // LIFO: Last In First Out
        std::vector<jtx::Account> const accounts = {
            judy,
            ivan,
            heidi,
            grace,
            frank,
            elsa,
            dave,
            carol,
            bob,
            alice,
        };

        // verify emitted txns
        for (size_t i = 0; i <= 9; i++)
        {
            Json::Value params;
            params[jss::transaction] = txIds[i];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            BEAST_EXPECT(meta[jss::delivered_amount] == "1000000");
            for (auto const& node : meta[sfAffectedNodes.jsonName])
            {
                auto const nodeType = node[sfLedgerEntryType.jsonName];
                if (nodeType == ltEMITTED_TXN)
                {
                    auto const& nf = node[sfFinalFields.jsonName];
                    auto const& et = nf[sfEmittedTxn.jsonName];
                    BEAST_EXPECT(
                        et[sfDestination.jsonName] == accounts[i].human());
                    break;
                }
            }
        }
    }

    void
    testTSH(FeatureBitset features)
    {
#pragma push_macro("TRANSACTION")
#undef TRANSACTION

#define TRANSACTION(tag, value, name, fields) test##name##TSH(features);

#include <xrpl/protocol/detail/transactions.macro>

#undef TRANSACTION
#pragma pop_macro("TRANSACTION")
    }

    void
    testEmittedTxn(FeatureBitset features)
    {
        testEmittedTxnReliability(features);
        testEmittedFlags(features);
        testEmissionOrdering(features);
    }

public:
    void
    run(std::uint32_t instance, bool last = false)
    {
        using namespace test::jtx;
        static FeatureBitset const all{supported_amendments()};

        static std::array<FeatureBitset, 4> const feats{
            all,
            all - fixXahauV1 - fixXahauV2 - featureIOUIssuerWeakTSH,
            all - fixXahauV2 - featureIOUIssuerWeakTSH,
            all - featureIOUIssuerWeakTSH,
        };

        if (BEAST_EXPECT(instance < feats.size()))
        {
            testTSH(feats[instance]);
        }
        if (instance == 0)
        {
            testEmittedTxn(feats[instance]);
        }
        BEAST_EXPECT(!last || instance == feats.size() - 1);
    }
    void
    run() override
    {
        run(0);
    }
};
#define SETHOOKTSH_TEST(i, last)                         \
    class SetHookTSH##i##_test : public SetHookTSH0_test \
    {                                                    \
        void                                             \
        run() override                                   \
        {                                                \
            SetHookTSH0_test::run(i, last);              \
        }                                                \
    };

SETHOOKTSH_TEST(1, false)
SETHOOKTSH_TEST(2, false)
SETHOOKTSH_TEST(3, true)

BEAST_DEFINE_TESTSUITE_PRIO(SetHookTSH0, app, ripple, 2);
BEAST_DEFINE_TESTSUITE_PRIO(SetHookTSH1, app, ripple, 2);
BEAST_DEFINE_TESTSUITE_PRIO(SetHookTSH2, app, ripple, 2);
BEAST_DEFINE_TESTSUITE_PRIO(SetHookTSH3, app, ripple, 2);

}  // namespace test
}  // namespace ripple
