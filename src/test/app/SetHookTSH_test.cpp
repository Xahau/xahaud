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

#include <ripple/app/hook/Enum.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/app/tx/apply.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/PayChan.h>
#include <ripple/protocol/jss.h>
#include <test/app/Import_json.h>
#include <test/jtx.h>
#include <vector>

namespace ripple {
namespace test {

#define BEAST_REQUIRE(x)     \
    {                        \
        BEAST_EXPECT(!!(x)); \
        if (!(x))            \
            return;          \
    }

struct SetHookTSH_test : public beast::unit_test::suite
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
        Json::Value params;
        params[jss::transaction] =
            env.tx()->getJson(JsonOptions::none)[jss::hash];
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

    // Check
    // | otxn | tsh | cancel |  create  | cash  |
    // |   A  |  A  |   S    |    S     |  N/A  |
    // |   A  |  D  |   N    |    S     |  N/A  |
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
        // w/s: none
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
            env(check::cancel(account, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
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
            auto const issuer = Account("issuer");
            env.fund(XRP(1000), account, issuer);
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
            auto const issuer = Account("issuer");
            env.fund(XRP(1000), account, issuer);
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
            auto createTx = escrow::create(account, dest, XRP(10));
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
            env(escrow::cancel(account, account, seq1),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: none
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
            auto createTx = escrow::create(account, dest, XRP(10));
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
            env(escrow::cancel(account, account, seq1),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
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
            auto createTx = escrow::create(account, dest, XRP(10));
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
            env(escrow::cancel(dest, account, seq1),
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

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow::create(account, dest, XRP(10));
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
            env(escrow::cancel(dest, account, seq1),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
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
            auto createTx = escrow::create(account, dest, XRP(10));
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
                tx = escrow::cancel(account, account, 0);
            }
            else
            {
                tx = escrow::cancel(account, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: none
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
            auto createTx = escrow::create(account, dest, XRP(10));
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
                tx = escrow::cancel(account, account, 0);
            }
            else
            {
                tx = escrow::cancel(account, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
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
            auto createTx = escrow::create(account, dest, XRP(10));
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
                tx = escrow::cancel(dest, account, 0);
            }
            else
            {
                tx = escrow::cancel(dest, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
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
            auto createTx = escrow::create(account, dest, XRP(10));
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
                tx = escrow::cancel(dest, account, 0);
            }
            else
            {
                tx = escrow::cancel(dest, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                (fixV1 ? (testStrong ? tshSTRONG : tshSTRONG)
                       : (testStrong ? tshNONE : tshNONE));
            testTSHStrongWeak(env, expected, __LINE__);
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
            auto createTx = escrow::create(account, dest, XRP(10));
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
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
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
            auto createTx = escrow::create(account, dest, XRP(10));
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
            env(escrow::finish(account, account, seq1),
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

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(account, dest, XRP(10));
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
            env(escrow::finish(account, account, seq1),
                fee(XRP(1)),
                ter(tesSUCCESS));
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
            auto createTx = escrow::create(account, dest, XRP(10));
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
            env(escrow::finish(dest, account, seq1),
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

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(account, dest, XRP(10));
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
            env(escrow::finish(dest, account, seq1),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
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
            auto createTx = escrow::create(account, dest, XRP(10));
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
                tx = escrow::finish(account, account, 0);
            }
            else
            {
                tx = escrow::finish(account, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
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
            auto createTx = escrow::create(account, dest, XRP(10));
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
                tx = escrow::finish(account, account, 0);
            }
            else
            {
                tx = escrow::finish(account, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
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
            auto createTx = escrow::create(account, dest, XRP(10));
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
                tx = escrow::finish(dest, account, 0);
            }
            else
            {
                tx = escrow::finish(dest, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
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
            auto createTx = escrow::create(account, dest, XRP(10));
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
                tx = escrow::finish(dest, account, 0);
            }
            else
            {
                tx = escrow::finish(dest, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                (fixV1 ? (testStrong ? tshSTRONG : tshSTRONG)
                       : (testStrong ? tshNONE : tshNONE));
            testTSHStrongWeak(env, expected, __LINE__);
        }
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
        // w/s: none
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

    // SignersListSet
    // | otxn  | tsh | sls |
    // |   A   |  A  |  S  |
    // |   A   |  S  |  S  |
    void
    testSignersListSetTSH(FeatureBitset features)
    {
        testcase("signers list set tsh");

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

            // signers list set
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

            // signers list set
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
        // w/s: none
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
            auto const expected =
                (fixV1 ? (testStrong ? tshNONE : tshNONE)
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
        // w/s: none
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
            auto const expected =
                (fixV1 ? (testStrong ? tshNONE : tshNONE)
                       : (testStrong ? tshSTRONG : tshSTRONG));
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: issuer
        // flag: burnable
        // tsh owner
        // w/s: none
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
            auto const expected =
                (fixV1 ? (testStrong ? tshNONE : tshNONE)
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
        // w/s: none
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
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }

        // otxn: owner
        // flag: burnable
        // tsh buyer
        // w/s: none
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
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
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

        bool didApply;
        TER terRes;

        env.app().openLedger().modify([&](OpenView& view, beast::Journal j) {
            auto const tx =
                std::make_unique<STTx>(Slice{txBlob.data(), txBlob.size()});
            std::tie(terRes, didApply) =
                ripple::apply(env.app(), view, *tx, tapNONE, env.journal);

            bool const applyResult = withFix ? false : true;
            if (withFix)
            {
                BEAST_EXPECT(terRes == tefNONDIR_EMIT);
            }
            else
            {
                BEAST_EXPECT(terRes == tesSUCCESS);
            }
            BEAST_EXPECT(didApply == applyResult);
            return didApply;
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
        testAccountSetTSH(features);
        testAccountDeleteTSH(features);
        testCheckCancelTSH(features);
        testCheckCashTSH(features);
        testCheckCreateTSH(features);
        testClaimRewardTSH(features);
        testDepositPreauthTSH(features);
        testEscrowCancelTSH(features);
        testEscrowIDCancelTSH(features);
        testEscrowCreateTSH(features);
        testEscrowFinishTSH(features);
        testEscrowIDFinishTSH(features);
        testGenesisMintTSH(features);
        testImportTSH(features);
        testInvokeTSH(features);
        testOfferCancelTSH(features);
        testOfferCreateTSH(features);
        testPaymentTSH(features);
        testPaymentChannelClaimTSH(features);
        testPaymentChannelCreateTSH(features);
        testPaymentChannelFundTSH(features);
        testSetHookTSH(features);
        testSetRegularKeyTSH(features);
        testSignersListSetTSH(features);
        testTicketCreateTSH(features);
        testTrustSetTSH(features);
        testURITokenMintTSH(features);
        testURITokenBurnTSH(features);
        testURITokenBuyTSH(features);
        testURITokenCancelSellOfferTSH(features);
        testURITokenCreateSellOfferTSH(features);
        testRemitTSH(features);
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
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testTSH(sa - fixXahauV1 - fixXahauV2);
        testTSH(sa - fixXahauV2);
        testTSH(sa);
        testEmittedTxn(sa - fixXahauV2);
        testEmittedTxn(sa);
    }
};

BEAST_DEFINE_TESTSUITE(SetHookTSH, app, ripple);

}  // namespace test
}  // namespace ripple