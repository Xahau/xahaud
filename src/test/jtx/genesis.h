//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL Labs

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

#ifndef RIPPLE_TEST_JTX_GENESIS_H_INCLUDED
#define RIPPLE_TEST_JTX_GENESIS_H_INCLUDED

#include <test/jtx/Account.h>
#include <test/jtx/Env.h>

namespace ripple {
namespace test {
namespace jtx {

/** Genesis operations. */
namespace genesis {

static const std::vector<uint8_t> MintTestHook = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x35U, 0x08U,
    0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7FU,
    0x01U, 0x7EU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU, 0x01U, 0x7EU, 0x60U, 0x05U,
    0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x00U, 0x01U, 0x7EU,
    0x60U, 0x04U, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x02U, 0xDAU, 0x01U,
    0x0EU, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0CU, 0x65U, 0x74U, 0x78U, 0x6EU, 0x5FU,
    0x72U, 0x65U, 0x73U, 0x65U, 0x72U, 0x76U, 0x65U, 0x00U, 0x00U, 0x03U, 0x65U,
    0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U,
    0x0CU, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x5FU, 0x61U, 0x63U, 0x63U, 0x6FU, 0x75U,
    0x6EU, 0x74U, 0x00U, 0x02U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x09U, 0x6FU, 0x74U,
    0x78U, 0x6EU, 0x5FU, 0x73U, 0x6CU, 0x6FU, 0x74U, 0x00U, 0x00U, 0x03U, 0x65U,
    0x6EU, 0x76U, 0x0DU, 0x73U, 0x6CU, 0x6FU, 0x74U, 0x5FU, 0x73U, 0x75U, 0x62U,
    0x66U, 0x69U, 0x65U, 0x6CU, 0x64U, 0x00U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U,
    0x08U, 0x72U, 0x6FU, 0x6CU, 0x6CU, 0x62U, 0x61U, 0x63U, 0x6BU, 0x00U, 0x04U,
    0x03U, 0x65U, 0x6EU, 0x76U, 0x04U, 0x73U, 0x6CU, 0x6FU, 0x74U, 0x00U, 0x03U,
    0x03U, 0x65U, 0x6EU, 0x76U, 0x05U, 0x74U, 0x72U, 0x61U, 0x63U, 0x65U, 0x00U,
    0x05U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0CU, 0x65U, 0x74U, 0x78U, 0x6EU, 0x5FU,
    0x64U, 0x65U, 0x74U, 0x61U, 0x69U, 0x6CU, 0x73U, 0x00U, 0x02U, 0x03U, 0x65U,
    0x6EU, 0x76U, 0x0DU, 0x65U, 0x74U, 0x78U, 0x6EU, 0x5FU, 0x66U, 0x65U, 0x65U,
    0x5FU, 0x62U, 0x61U, 0x73U, 0x65U, 0x00U, 0x02U, 0x03U, 0x65U, 0x6EU, 0x76U,
    0x0AU, 0x6CU, 0x65U, 0x64U, 0x67U, 0x65U, 0x72U, 0x5FU, 0x73U, 0x65U, 0x71U,
    0x00U, 0x06U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x04U, 0x65U, 0x6DU, 0x69U, 0x74U,
    0x00U, 0x07U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x09U, 0x74U, 0x72U, 0x61U, 0x63U,
    0x65U, 0x5FU, 0x6EU, 0x75U, 0x6DU, 0x00U, 0x04U, 0x03U, 0x65U, 0x6EU, 0x76U,
    0x06U, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x04U, 0x03U, 0x02U,
    0x01U, 0x00U, 0x05U, 0x03U, 0x01U, 0x00U, 0x02U, 0x06U, 0x28U, 0x06U, 0x7FU,
    0x01U, 0x41U, 0xD0U, 0xDDU, 0x07U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0xCFU, 0xDDU,
    0x03U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0x7FU, 0x00U, 0x41U,
    0xD0U, 0xDDU, 0x07U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0x7FU,
    0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0x07U, 0x08U, 0x01U, 0x04U, 0x68U, 0x6FU,
    0x6FU, 0x6BU, 0x00U, 0x0EU, 0x0AU, 0xEDU, 0x83U, 0x00U, 0x01U, 0xE9U, 0x83U,
    0x00U, 0x02U, 0x01U, 0x7FU, 0x02U, 0x7EU, 0x23U, 0x00U, 0x41U, 0x20U, 0x6BU,
    0x22U, 0x00U, 0x24U, 0x00U, 0x41U, 0x01U, 0x10U, 0x00U, 0x1AU, 0x41U, 0x01U,
    0x41U, 0x01U, 0x10U, 0x01U, 0x1AU, 0x41U, 0xC7U, 0x08U, 0x41U, 0x14U, 0x10U,
    0x02U, 0x1AU, 0x41U, 0x01U, 0x10U, 0x03U, 0x1AU, 0x41U, 0x01U, 0x41U, 0x9AU,
    0x80U, 0x1CU, 0x41U, 0x02U, 0x10U, 0x04U, 0x42U, 0x02U, 0x52U, 0x04U, 0x40U,
    0x41U, 0xE0U, 0xDCU, 0x03U, 0x41U, 0x1DU, 0x42U, 0xC1U, 0x00U, 0x10U, 0x05U,
    0x1AU, 0x0BU, 0x41U, 0xCFU, 0x09U, 0x41U, 0x91U, 0xD3U, 0x03U, 0x41U, 0x02U,
    0x10U, 0x06U, 0x22U, 0x02U, 0x42U, 0x00U, 0x57U, 0x04U, 0x40U, 0x41U, 0xE0U,
    0xDCU, 0x03U, 0x41U, 0x1DU, 0x42U, 0xC6U, 0x00U, 0x10U, 0x05U, 0x1AU, 0x0BU,
    0x41U, 0xCFU, 0x09U, 0x2DU, 0x00U, 0x00U, 0x21U, 0x01U, 0x41U, 0xCFU, 0x09U,
    0x41U, 0x99U, 0x01U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x01U, 0x41U, 0xC1U, 0x01U,
    0x4FU, 0x04U, 0x40U, 0x02U, 0x40U, 0x41U, 0xD0U, 0x09U, 0x41U, 0x99U, 0x01U,
    0x3AU, 0x00U, 0x00U, 0x20U, 0x01U, 0x41U, 0xF1U, 0x01U, 0x49U, 0x0DU, 0x00U,
    0x41U, 0xD1U, 0x09U, 0x41U, 0x99U, 0x01U, 0x3AU, 0x00U, 0x00U, 0x0BU, 0x0BU,
    0x41U, 0xFDU, 0xDCU, 0x03U, 0x41U, 0x05U, 0x41U, 0x80U, 0x08U, 0x20U, 0x02U,
    0xA7U, 0x41U, 0xCFU, 0x01U, 0x6AU, 0x22U, 0x01U, 0x41U, 0x01U, 0x10U, 0x07U,
    0x1AU, 0x41U, 0xDBU, 0x08U, 0x41U, 0xF4U, 0x00U, 0x10U, 0x08U, 0x42U, 0x00U,
    0x57U, 0x04U, 0x40U, 0x41U, 0xE0U, 0xDCU, 0x03U, 0x41U, 0x1DU, 0x42U, 0xDAU,
    0x00U, 0x10U, 0x05U, 0x1AU, 0x0BU, 0x41U, 0x9AU, 0x08U, 0x41U, 0x80U, 0x08U,
    0x20U, 0x01U, 0x10U, 0x09U, 0x22U, 0x02U, 0x42U, 0x38U, 0x86U, 0x20U, 0x02U,
    0x42U, 0x38U, 0x88U, 0x42U, 0x3FU, 0x83U, 0x84U, 0x20U, 0x02U, 0x42U, 0x28U,
    0x86U, 0x42U, 0x80U, 0x80U, 0x80U, 0x80U, 0x80U, 0x80U, 0xC0U, 0xFFU, 0x00U,
    0x83U, 0x84U, 0x20U, 0x02U, 0x42U, 0x18U, 0x86U, 0x42U, 0x80U, 0x80U, 0x80U,
    0x80U, 0x80U, 0xE0U, 0x3FU, 0x83U, 0x84U, 0x20U, 0x02U, 0x42U, 0x08U, 0x86U,
    0x42U, 0x80U, 0x80U, 0x80U, 0x80U, 0xF0U, 0x1FU, 0x83U, 0x84U, 0x20U, 0x02U,
    0x42U, 0x08U, 0x88U, 0x42U, 0x80U, 0x80U, 0x80U, 0xF8U, 0x0FU, 0x83U, 0x84U,
    0x20U, 0x02U, 0x42U, 0x18U, 0x88U, 0x42U, 0x80U, 0x80U, 0xFCU, 0x07U, 0x83U,
    0x84U, 0x20U, 0x02U, 0x42U, 0x28U, 0x88U, 0x42U, 0x80U, 0xFEU, 0x03U, 0x83U,
    0x84U, 0x42U, 0xC0U, 0x00U, 0x84U, 0x37U, 0x03U, 0x00U, 0x41U, 0x98U, 0x08U,
    0x10U, 0x0AU, 0x22U, 0x03U, 0x42U, 0x05U, 0x7CU, 0x22U, 0x02U, 0x3CU, 0x00U,
    0x00U, 0x41U, 0x92U, 0x08U, 0x20U, 0x03U, 0x42U, 0x01U, 0x7CU, 0x22U, 0x03U,
    0x3CU, 0x00U, 0x00U, 0x41U, 0x97U, 0x08U, 0x20U, 0x02U, 0x42U, 0x08U, 0x88U,
    0x3CU, 0x00U, 0x00U, 0x41U, 0x96U, 0x08U, 0x20U, 0x02U, 0x42U, 0x10U, 0x88U,
    0x3CU, 0x00U, 0x00U, 0x41U, 0x95U, 0x08U, 0x20U, 0x02U, 0x42U, 0x18U, 0x88U,
    0x3CU, 0x00U, 0x00U, 0x41U, 0x91U, 0x08U, 0x20U, 0x03U, 0x42U, 0x08U, 0x88U,
    0x3CU, 0x00U, 0x00U, 0x41U, 0x90U, 0x08U, 0x20U, 0x03U, 0x42U, 0x10U, 0x88U,
    0x3CU, 0x00U, 0x00U, 0x41U, 0x8FU, 0x08U, 0x20U, 0x03U, 0x42U, 0x18U, 0x88U,
    0x3CU, 0x00U, 0x00U, 0x41U, 0x82U, 0xDDU, 0x03U, 0x41U, 0x06U, 0x41U, 0x80U,
    0x08U, 0x20U, 0x01U, 0x41U, 0x01U, 0x10U, 0x07U, 0x1AU, 0x41U, 0x88U, 0xDDU,
    0x03U, 0x41U, 0x0BU, 0x20U, 0x00U, 0x41U, 0x20U, 0x41U, 0x80U, 0x08U, 0x20U,
    0x01U, 0x10U, 0x0BU, 0x22U, 0x02U, 0x10U, 0x0CU, 0x1AU, 0x20U, 0x02U, 0x42U,
    0x7FU, 0x57U, 0x04U, 0x40U, 0x41U, 0x94U, 0xDDU, 0x03U, 0x41U, 0x17U, 0x42U,
    0xF7U, 0x00U, 0x10U, 0x05U, 0x1AU, 0x0BU, 0x41U, 0xABU, 0xDDU, 0x03U, 0x41U,
    0x24U, 0x42U, 0xFAU, 0x00U, 0x10U, 0x0DU, 0x1AU, 0x20U, 0x00U, 0x41U, 0x20U,
    0x6AU, 0x24U, 0x00U, 0x20U, 0x02U, 0x0BU, 0x0BU, 0xC4U, 0x01U, 0x04U, 0x00U,
    0x41U, 0x80U, 0x08U, 0x0BU, 0x24U, 0x12U, 0x00U, 0x60U, 0x22U, 0x80U, 0x00U,
    0x00U, 0x00U, 0x24U, 0x00U, 0x00U, 0x00U, 0x00U, 0x20U, 0x1AU, 0x00U, 0x00U,
    0x00U, 0x00U, 0x20U, 0x1BU, 0x00U, 0x00U, 0x00U, 0x00U, 0x68U, 0x40U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x73U, 0x21U, 0x00U, 0x41U, 0xC5U,
    0x08U, 0x0BU, 0x16U, 0x81U, 0x14U, 0xB5U, 0xF7U, 0x62U, 0x79U, 0x8AU, 0x53U,
    0xD5U, 0x43U, 0xA0U, 0x14U, 0xCAU, 0xF8U, 0xB2U, 0x97U, 0xCFU, 0xF8U, 0xF2U,
    0xF9U, 0x37U, 0xE8U, 0x00U, 0x41U, 0xCFU, 0x09U, 0x0BU, 0x02U, 0xF0U, 0x60U,
    0x00U, 0x41U, 0xE0U, 0xDCU, 0x03U, 0x0BU, 0x6EU, 0x4DU, 0x69U, 0x6EU, 0x74U,
    0x54U, 0x65U, 0x73U, 0x74U, 0x3AU, 0x20U, 0x41U, 0x73U, 0x73U, 0x65U, 0x72U,
    0x74U, 0x69U, 0x6FU, 0x6EU, 0x20U, 0x66U, 0x61U, 0x69U, 0x6CU, 0x75U, 0x72U,
    0x65U, 0x2EU, 0x00U, 0x54U, 0x78U, 0x6EU, 0x3AU, 0x00U, 0x65U, 0x6DU, 0x69U,
    0x74U, 0x3AU, 0x00U, 0x65U, 0x6DU, 0x69U, 0x74U, 0x5FU, 0x72U, 0x65U, 0x73U,
    0x75U, 0x6CU, 0x74U, 0x00U, 0x4DU, 0x69U, 0x6EU, 0x74U, 0x54U, 0x65U, 0x73U,
    0x74U, 0x3AU, 0x20U, 0x45U, 0x6DU, 0x69U, 0x74U, 0x20U, 0x66U, 0x61U, 0x69U,
    0x6CU, 0x65U, 0x64U, 0x2EU, 0x00U, 0x4DU, 0x69U, 0x6EU, 0x74U, 0x54U, 0x65U,
    0x73U, 0x74U, 0x3AU, 0x20U, 0x45U, 0x6DU, 0x69U, 0x74U, 0x74U, 0x65U, 0x64U,
    0x20U, 0x74U, 0x78U, 0x6EU, 0x20U, 0x73U, 0x75U, 0x63U, 0x63U, 0x65U, 0x73U,
    0x73U, 0x66U, 0x75U, 0x6CU, 0x6CU, 0x79U, 0x2EU};

static const std::vector<uint8_t> AcceptHook = {
    0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x13U, 0x03U,
    0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
    0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x02U, 0x17U, 0x02U, 0x03U,
    0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU,
    0x76U, 0x06U, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U,
    0x02U, 0x01U, 0x02U, 0x05U, 0x03U, 0x01U, 0x00U, 0x02U, 0x06U, 0x21U, 0x05U,
    0x7FU, 0x01U, 0x41U, 0x80U, 0x88U, 0x04U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0x80U,
    0x08U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0x7FU, 0x00U, 0x41U,
    0x80U, 0x88U, 0x04U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0x07U,
    0x08U, 0x01U, 0x04U, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x00U, 0x02U, 0x0AU, 0xA2U,
    0x80U, 0x00U, 0x01U, 0x9EU, 0x80U, 0x00U, 0x01U, 0x01U, 0x7EU, 0x41U, 0x01U,
    0x41U, 0x01U, 0x10U, 0x80U, 0x80U, 0x80U, 0x80U, 0x00U, 0x1AU, 0x41U, 0x00U,
    0x41U, 0x00U, 0x42U, 0x00U, 0x10U, 0x81U, 0x80U, 0x80U, 0x80U, 0x00U, 0x1AU,
    0x20U, 0x01U, 0x0BU};

struct GenMint
{
    std::optional<std::string> dest;
    std::optional<jtx::PrettyAmount> amt;
    std::optional<std::string> marks;
    std::optional<std::string> flags;

    GenMint(
        AccountID const& dst,
        jtx::PrettyAmount const& x,
        std::optional<std::string> m = std::nullopt,
        std::optional<std::string> f = std::nullopt)
        : dest(toBase58(dst)), amt(x), marks(m), flags(f)
    {
    }
};

Json::Value
mint(jtx::Account const& account, std::vector<genesis::GenMint> mints);

Json::Value
setMintHook(jtx::Account const& account);

Json::Value
setAcceptHook(jtx::Account const& account);

std::string
makeBlob(std::vector<std::tuple<
             std::optional<AccountID>,
             std::optional<STAmount>,
             std::optional<uint256>,
             std::optional<uint256>>> entries);

}  // namespace genesis

}  // namespace jtx

}  // namespace test
}  // namespace ripple

#endif  // RIPPLE_TEST_JTX_GENESIS_H_INCLUDED
