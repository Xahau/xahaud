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

#include <ripple/core/ConfigSections.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/PayChan.h>
#include <ripple/protocol/jss.h>
#include <test/app/Import_json.h>
#include <test/jtx.h>
#include <vector>

namespace ripple {
namespace test {

struct SetHookTSH_test : public beast::unit_test::suite
{
private:
    // helper
    void static overrideFlag(Json::Value& jv)
    {
        jv[jss::Flags] = hsfOVERRIDE;
    }
    void static collectFlag(Json::Value& jv)
    {
        jv[jss::Flags] = hsfOVERRIDE | hsfCOLLECT;
    }

    const std::vector<uint8_t> TshHook = {
        0x00U, 0x61U, 0x73U, 0x6dU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x1cU,
        0x04U, 0x60U, 0x05U, 0x7fU, 0x7fU, 0x7fU, 0x7fU, 0x7fU, 0x01U, 0x7eU,
        0x60U, 0x03U, 0x7fU, 0x7fU, 0x7eU, 0x01U, 0x7eU, 0x60U, 0x02U, 0x7fU,
        0x7fU, 0x01U, 0x7fU, 0x60U, 0x01U, 0x7fU, 0x01U, 0x7eU, 0x02U, 0x23U,
        0x03U, 0x03U, 0x65U, 0x6eU, 0x76U, 0x05U, 0x74U, 0x72U, 0x61U, 0x63U,
        0x65U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6eU, 0x76U, 0x06U, 0x61U, 0x63U,
        0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U, 0x65U, 0x6eU, 0x76U,
        0x02U, 0x5fU, 0x67U, 0x00U, 0x02U, 0x03U, 0x02U, 0x01U, 0x03U, 0x05U,
        0x03U, 0x01U, 0x00U, 0x02U, 0x06U, 0x2bU, 0x07U, 0x7fU, 0x01U, 0x41U,
        0xc0U, 0x8bU, 0x04U, 0x0bU, 0x7fU, 0x00U, 0x41U, 0x80U, 0x08U, 0x0bU,
        0x7fU, 0x00U, 0x41U, 0xb8U, 0x0bU, 0x0bU, 0x7fU, 0x00U, 0x41U, 0x80U,
        0x08U, 0x0bU, 0x7fU, 0x00U, 0x41U, 0xc0U, 0x8bU, 0x04U, 0x0bU, 0x7fU,
        0x00U, 0x41U, 0x00U, 0x0bU, 0x7fU, 0x00U, 0x41U, 0x01U, 0x0bU, 0x07U,
        0x08U, 0x01U, 0x04U, 0x68U, 0x6fU, 0x6fU, 0x6bU, 0x00U, 0x03U, 0x0aU,
        0xf3U, 0x81U, 0x00U, 0x01U, 0xefU, 0x81U, 0x00U, 0x01U, 0x01U, 0x7fU,
        0x23U, 0x00U, 0x41U, 0x10U, 0x6bU, 0x22U, 0x01U, 0x24U, 0x00U, 0x20U,
        0x01U, 0x20U, 0x00U, 0x36U, 0x02U, 0x0cU, 0x41U, 0x9aU, 0x0bU, 0x41U,
        0x0fU, 0x41U, 0xbdU, 0x09U, 0x41U, 0x0eU, 0x41U, 0x00U, 0x10U, 0x00U,
        0x1aU, 0x02U, 0x40U, 0x02U, 0x40U, 0x02U, 0x40U, 0x02U, 0x40U, 0x20U,
        0x01U, 0x28U, 0x02U, 0x0cU, 0x0eU, 0x03U, 0x00U, 0x01U, 0x02U, 0x03U,
        0x0bU, 0x41U, 0xd9U, 0x0aU, 0x41U, 0xc0U, 0x00U, 0x41U, 0xfeU, 0x08U,
        0x41U, 0x3fU, 0x41U, 0x00U, 0x10U, 0x00U, 0x1aU, 0x0cU, 0x02U, 0x0bU,
        0x41U, 0x9bU, 0x0aU, 0x41U, 0x3dU, 0x41U, 0xc2U, 0x08U, 0x41U, 0x3cU,
        0x41U, 0x00U, 0x10U, 0x00U, 0x1aU, 0x0cU, 0x01U, 0x0bU, 0x41U, 0xd7U,
        0x09U, 0x41U, 0xc3U, 0x00U, 0x41U, 0x80U, 0x08U, 0x41U, 0xc2U, 0x00U,
        0x41U, 0x00U, 0x10U, 0x00U, 0x1aU, 0x0bU, 0x41U, 0xaaU, 0x0bU, 0x41U,
        0x0dU, 0x41U, 0xcbU, 0x09U, 0x41U, 0x0cU, 0x41U, 0x00U, 0x10U, 0x00U,
        0x1aU, 0x20U, 0x01U, 0x20U, 0x01U, 0x41U, 0x08U, 0x6aU, 0x22U, 0x00U,
        0x36U, 0x02U, 0x04U, 0x20U, 0x01U, 0x28U, 0x02U, 0x04U, 0x20U, 0x01U,
        0x35U, 0x02U, 0x0cU, 0x42U, 0x18U, 0x88U, 0x42U, 0xffU, 0x01U, 0x83U,
        0x3cU, 0x00U, 0x00U, 0x20U, 0x01U, 0x28U, 0x02U, 0x04U, 0x20U, 0x01U,
        0x35U, 0x02U, 0x0cU, 0x42U, 0x10U, 0x88U, 0x42U, 0xffU, 0x01U, 0x83U,
        0x3cU, 0x00U, 0x01U, 0x20U, 0x01U, 0x28U, 0x02U, 0x04U, 0x20U, 0x01U,
        0x35U, 0x02U, 0x0cU, 0x42U, 0x08U, 0x88U, 0x42U, 0xffU, 0x01U, 0x83U,
        0x3cU, 0x00U, 0x02U, 0x20U, 0x01U, 0x28U, 0x02U, 0x04U, 0x20U, 0x01U,
        0x35U, 0x02U, 0x0cU, 0x42U, 0xffU, 0x01U, 0x83U, 0x3cU, 0x00U, 0x03U,
        0x20U, 0x00U, 0x41U, 0x04U, 0x42U, 0x18U, 0x10U, 0x01U, 0x1aU, 0x41U,
        0x01U, 0x41U, 0x01U, 0x10U, 0x02U, 0x1aU, 0x20U, 0x01U, 0x41U, 0x10U,
        0x6aU, 0x24U, 0x00U, 0x42U, 0x00U, 0x0bU, 0x0bU, 0xbfU, 0x03U, 0x01U,
        0x00U, 0x41U, 0x80U, 0x08U, 0x0bU, 0xb7U, 0x03U, 0x74U, 0x73U, 0x68U,
        0x2eU, 0x63U, 0x3aU, 0x20U, 0x57U, 0x65U, 0x61U, 0x6bU, 0x20U, 0x41U,
        0x67U, 0x61U, 0x69U, 0x6eU, 0x2eU, 0x20U, 0x45U, 0x78U, 0x65U, 0x63U,
        0x75U, 0x74U, 0x65U, 0x20U, 0x41U, 0x46U, 0x54U, 0x45U, 0x52U, 0x20U,
        0x74U, 0x72U, 0x61U, 0x6eU, 0x73U, 0x61U, 0x63U, 0x74U, 0x69U, 0x6fU,
        0x6eU, 0x20U, 0x69U, 0x73U, 0x20U, 0x61U, 0x70U, 0x70U, 0x6cU, 0x69U,
        0x65U, 0x64U, 0x20U, 0x74U, 0x6fU, 0x20U, 0x6cU, 0x65U, 0x64U, 0x67U,
        0x65U, 0x72U, 0x00U, 0x74U, 0x73U, 0x68U, 0x2eU, 0x63U, 0x3aU, 0x20U,
        0x57U, 0x65U, 0x61U, 0x6bU, 0x2eU, 0x20U, 0x45U, 0x78U, 0x65U, 0x63U,
        0x75U, 0x74U, 0x65U, 0x20U, 0x41U, 0x46U, 0x54U, 0x45U, 0x52U, 0x20U,
        0x74U, 0x72U, 0x61U, 0x6eU, 0x73U, 0x61U, 0x63U, 0x74U, 0x69U, 0x6fU,
        0x6eU, 0x20U, 0x69U, 0x73U, 0x20U, 0x61U, 0x70U, 0x70U, 0x6cU, 0x69U,
        0x65U, 0x64U, 0x20U, 0x74U, 0x6fU, 0x20U, 0x6cU, 0x65U, 0x64U, 0x67U,
        0x65U, 0x72U, 0x00U, 0x74U, 0x73U, 0x68U, 0x2eU, 0x63U, 0x3aU, 0x20U,
        0x53U, 0x74U, 0x72U, 0x6fU, 0x6eU, 0x67U, 0x2eU, 0x20U, 0x45U, 0x78U,
        0x65U, 0x63U, 0x75U, 0x74U, 0x65U, 0x20U, 0x42U, 0x45U, 0x46U, 0x4fU,
        0x52U, 0x45U, 0x20U, 0x74U, 0x72U, 0x61U, 0x6eU, 0x73U, 0x61U, 0x63U,
        0x74U, 0x69U, 0x6fU, 0x6eU, 0x20U, 0x69U, 0x73U, 0x20U, 0x61U, 0x70U,
        0x70U, 0x6cU, 0x69U, 0x65U, 0x64U, 0x20U, 0x74U, 0x6fU, 0x20U, 0x6cU,
        0x65U, 0x64U, 0x67U, 0x65U, 0x72U, 0x00U, 0x74U, 0x73U, 0x68U, 0x2eU,
        0x63U, 0x3aU, 0x20U, 0x53U, 0x74U, 0x61U, 0x72U, 0x74U, 0x2eU, 0x00U,
        0x74U, 0x73U, 0x68U, 0x2eU, 0x63U, 0x3aU, 0x20U, 0x45U, 0x6eU, 0x64U,
        0x2eU, 0x00U, 0x22U, 0x74U, 0x73U, 0x68U, 0x2eU, 0x63U, 0x3aU, 0x20U,
        0x57U, 0x65U, 0x61U, 0x6bU, 0x20U, 0x41U, 0x67U, 0x61U, 0x69U, 0x6eU,
        0x2eU, 0x20U, 0x45U, 0x78U, 0x65U, 0x63U, 0x75U, 0x74U, 0x65U, 0x20U,
        0x41U, 0x46U, 0x54U, 0x45U, 0x52U, 0x20U, 0x74U, 0x72U, 0x61U, 0x6eU,
        0x73U, 0x61U, 0x63U, 0x74U, 0x69U, 0x6fU, 0x6eU, 0x20U, 0x69U, 0x73U,
        0x20U, 0x61U, 0x70U, 0x70U, 0x6cU, 0x69U, 0x65U, 0x64U, 0x20U, 0x74U,
        0x6fU, 0x20U, 0x6cU, 0x65U, 0x64U, 0x67U, 0x65U, 0x72U, 0x22U, 0x00U,
        0x22U, 0x74U, 0x73U, 0x68U, 0x2eU, 0x63U, 0x3aU, 0x20U, 0x57U, 0x65U,
        0x61U, 0x6bU, 0x2eU, 0x20U, 0x45U, 0x78U, 0x65U, 0x63U, 0x75U, 0x74U,
        0x65U, 0x20U, 0x41U, 0x46U, 0x54U, 0x45U, 0x52U, 0x20U, 0x74U, 0x72U,
        0x61U, 0x6eU, 0x73U, 0x61U, 0x63U, 0x74U, 0x69U, 0x6fU, 0x6eU, 0x20U,
        0x69U, 0x73U, 0x20U, 0x61U, 0x70U, 0x70U, 0x6cU, 0x69U, 0x65U, 0x64U,
        0x20U, 0x74U, 0x6fU, 0x20U, 0x6cU, 0x65U, 0x64U, 0x67U, 0x65U, 0x72U,
        0x22U, 0x00U, 0x22U, 0x74U, 0x73U, 0x68U, 0x2eU, 0x63U, 0x3aU, 0x20U,
        0x53U, 0x74U, 0x72U, 0x6fU, 0x6eU, 0x67U, 0x2eU, 0x20U, 0x45U, 0x78U,
        0x65U, 0x63U, 0x75U, 0x74U, 0x65U, 0x20U, 0x42U, 0x45U, 0x46U, 0x4fU,
        0x52U, 0x45U, 0x20U, 0x74U, 0x72U, 0x61U, 0x6eU, 0x73U, 0x61U, 0x63U,
        0x74U, 0x69U, 0x6fU, 0x6eU, 0x20U, 0x69U, 0x73U, 0x20U, 0x61U, 0x70U,
        0x70U, 0x6cU, 0x69U, 0x65U, 0x64U, 0x20U, 0x74U, 0x6fU, 0x20U, 0x6cU,
        0x65U, 0x64U, 0x67U, 0x65U, 0x72U, 0x22U, 0x00U, 0x22U, 0x74U, 0x73U,
        0x68U, 0x2eU, 0x63U, 0x3aU, 0x20U, 0x53U, 0x74U, 0x61U, 0x72U, 0x74U,
        0x2eU, 0x22U, 0x00U, 0x22U, 0x74U, 0x73U, 0x68U, 0x2eU, 0x63U, 0x3aU,
        0x20U, 0x45U, 0x6eU, 0x64U, 0x2eU, 0x22U};

    const std::vector<uint8_t> MintTestHook = {
        0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x35U,
        0x08U, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x02U, 0x7FU, 0x7FU,
        0x01U, 0x7FU, 0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x03U,
        0x7FU, 0x7FU, 0x7FU, 0x01U, 0x7EU, 0x60U, 0x03U, 0x7FU, 0x7FU, 0x7EU,
        0x01U, 0x7EU, 0x60U, 0x05U, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x7FU, 0x01U,
        0x7EU, 0x60U, 0x00U, 0x01U, 0x7EU, 0x60U, 0x04U, 0x7FU, 0x7FU, 0x7FU,
        0x7FU, 0x01U, 0x7EU, 0x02U, 0xDAU, 0x01U, 0x0EU, 0x03U, 0x65U, 0x6EU,
        0x76U, 0x0CU, 0x65U, 0x74U, 0x78U, 0x6EU, 0x5FU, 0x72U, 0x65U, 0x73U,
        0x65U, 0x72U, 0x76U, 0x65U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU, 0x76U,
        0x02U, 0x5FU, 0x67U, 0x00U, 0x01U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x0CU,
        0x68U, 0x6FU, 0x6FU, 0x6BU, 0x5FU, 0x61U, 0x63U, 0x63U, 0x6FU, 0x75U,
        0x6EU, 0x74U, 0x00U, 0x02U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x09U, 0x6FU,
        0x74U, 0x78U, 0x6EU, 0x5FU, 0x73U, 0x6CU, 0x6FU, 0x74U, 0x00U, 0x00U,
        0x03U, 0x65U, 0x6EU, 0x76U, 0x0DU, 0x73U, 0x6CU, 0x6FU, 0x74U, 0x5FU,
        0x73U, 0x75U, 0x62U, 0x66U, 0x69U, 0x65U, 0x6CU, 0x64U, 0x00U, 0x03U,
        0x03U, 0x65U, 0x6EU, 0x76U, 0x08U, 0x72U, 0x6FU, 0x6CU, 0x6CU, 0x62U,
        0x61U, 0x63U, 0x6BU, 0x00U, 0x04U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x04U,
        0x73U, 0x6CU, 0x6FU, 0x74U, 0x00U, 0x03U, 0x03U, 0x65U, 0x6EU, 0x76U,
        0x05U, 0x74U, 0x72U, 0x61U, 0x63U, 0x65U, 0x00U, 0x05U, 0x03U, 0x65U,
        0x6EU, 0x76U, 0x0CU, 0x65U, 0x74U, 0x78U, 0x6EU, 0x5FU, 0x64U, 0x65U,
        0x74U, 0x61U, 0x69U, 0x6CU, 0x73U, 0x00U, 0x02U, 0x03U, 0x65U, 0x6EU,
        0x76U, 0x0DU, 0x65U, 0x74U, 0x78U, 0x6EU, 0x5FU, 0x66U, 0x65U, 0x65U,
        0x5FU, 0x62U, 0x61U, 0x73U, 0x65U, 0x00U, 0x02U, 0x03U, 0x65U, 0x6EU,
        0x76U, 0x0AU, 0x6CU, 0x65U, 0x64U, 0x67U, 0x65U, 0x72U, 0x5FU, 0x73U,
        0x65U, 0x71U, 0x00U, 0x06U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x04U, 0x65U,
        0x6DU, 0x69U, 0x74U, 0x00U, 0x07U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x09U,
        0x74U, 0x72U, 0x61U, 0x63U, 0x65U, 0x5FU, 0x6EU, 0x75U, 0x6DU, 0x00U,
        0x04U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U, 0x63U, 0x65U,
        0x70U, 0x74U, 0x00U, 0x04U, 0x03U, 0x02U, 0x01U, 0x00U, 0x05U, 0x03U,
        0x01U, 0x00U, 0x02U, 0x06U, 0x28U, 0x06U, 0x7FU, 0x01U, 0x41U, 0xD0U,
        0xDDU, 0x07U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0xCFU, 0xDDU, 0x03U, 0x0BU,
        0x7FU, 0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0xD0U,
        0xDDU, 0x07U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0x7FU,
        0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0x07U, 0x08U, 0x01U, 0x04U, 0x68U,
        0x6FU, 0x6FU, 0x6BU, 0x00U, 0x0EU, 0x0AU, 0xEDU, 0x83U, 0x00U, 0x01U,
        0xE9U, 0x83U, 0x00U, 0x02U, 0x01U, 0x7FU, 0x02U, 0x7EU, 0x23U, 0x00U,
        0x41U, 0x20U, 0x6BU, 0x22U, 0x00U, 0x24U, 0x00U, 0x41U, 0x01U, 0x10U,
        0x00U, 0x1AU, 0x41U, 0x01U, 0x41U, 0x01U, 0x10U, 0x01U, 0x1AU, 0x41U,
        0xC7U, 0x08U, 0x41U, 0x14U, 0x10U, 0x02U, 0x1AU, 0x41U, 0x01U, 0x10U,
        0x03U, 0x1AU, 0x41U, 0x01U, 0x41U, 0x9AU, 0x80U, 0x1CU, 0x41U, 0x02U,
        0x10U, 0x04U, 0x42U, 0x02U, 0x52U, 0x04U, 0x40U, 0x41U, 0xE0U, 0xDCU,
        0x03U, 0x41U, 0x1DU, 0x42U, 0xC1U, 0x00U, 0x10U, 0x05U, 0x1AU, 0x0BU,
        0x41U, 0xCFU, 0x09U, 0x41U, 0x91U, 0xD3U, 0x03U, 0x41U, 0x02U, 0x10U,
        0x06U, 0x22U, 0x02U, 0x42U, 0x00U, 0x57U, 0x04U, 0x40U, 0x41U, 0xE0U,
        0xDCU, 0x03U, 0x41U, 0x1DU, 0x42U, 0xC6U, 0x00U, 0x10U, 0x05U, 0x1AU,
        0x0BU, 0x41U, 0xCFU, 0x09U, 0x2DU, 0x00U, 0x00U, 0x21U, 0x01U, 0x41U,
        0xCFU, 0x09U, 0x41U, 0x99U, 0x01U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x01U,
        0x41U, 0xC1U, 0x01U, 0x4FU, 0x04U, 0x40U, 0x02U, 0x40U, 0x41U, 0xD0U,
        0x09U, 0x41U, 0x99U, 0x01U, 0x3AU, 0x00U, 0x00U, 0x20U, 0x01U, 0x41U,
        0xF1U, 0x01U, 0x49U, 0x0DU, 0x00U, 0x41U, 0xD1U, 0x09U, 0x41U, 0x99U,
        0x01U, 0x3AU, 0x00U, 0x00U, 0x0BU, 0x0BU, 0x41U, 0xFDU, 0xDCU, 0x03U,
        0x41U, 0x05U, 0x41U, 0x80U, 0x08U, 0x20U, 0x02U, 0xA7U, 0x41U, 0xCFU,
        0x01U, 0x6AU, 0x22U, 0x01U, 0x41U, 0x01U, 0x10U, 0x07U, 0x1AU, 0x41U,
        0xDBU, 0x08U, 0x41U, 0xF4U, 0x00U, 0x10U, 0x08U, 0x42U, 0x00U, 0x57U,
        0x04U, 0x40U, 0x41U, 0xE0U, 0xDCU, 0x03U, 0x41U, 0x1DU, 0x42U, 0xDAU,
        0x00U, 0x10U, 0x05U, 0x1AU, 0x0BU, 0x41U, 0x9AU, 0x08U, 0x41U, 0x80U,
        0x08U, 0x20U, 0x01U, 0x10U, 0x09U, 0x22U, 0x02U, 0x42U, 0x38U, 0x86U,
        0x20U, 0x02U, 0x42U, 0x38U, 0x88U, 0x42U, 0x3FU, 0x83U, 0x84U, 0x20U,
        0x02U, 0x42U, 0x28U, 0x86U, 0x42U, 0x80U, 0x80U, 0x80U, 0x80U, 0x80U,
        0x80U, 0xC0U, 0xFFU, 0x00U, 0x83U, 0x84U, 0x20U, 0x02U, 0x42U, 0x18U,
        0x86U, 0x42U, 0x80U, 0x80U, 0x80U, 0x80U, 0x80U, 0xE0U, 0x3FU, 0x83U,
        0x84U, 0x20U, 0x02U, 0x42U, 0x08U, 0x86U, 0x42U, 0x80U, 0x80U, 0x80U,
        0x80U, 0xF0U, 0x1FU, 0x83U, 0x84U, 0x20U, 0x02U, 0x42U, 0x08U, 0x88U,
        0x42U, 0x80U, 0x80U, 0x80U, 0xF8U, 0x0FU, 0x83U, 0x84U, 0x20U, 0x02U,
        0x42U, 0x18U, 0x88U, 0x42U, 0x80U, 0x80U, 0xFCU, 0x07U, 0x83U, 0x84U,
        0x20U, 0x02U, 0x42U, 0x28U, 0x88U, 0x42U, 0x80U, 0xFEU, 0x03U, 0x83U,
        0x84U, 0x42U, 0xC0U, 0x00U, 0x84U, 0x37U, 0x03U, 0x00U, 0x41U, 0x98U,
        0x08U, 0x10U, 0x0AU, 0x22U, 0x03U, 0x42U, 0x05U, 0x7CU, 0x22U, 0x02U,
        0x3CU, 0x00U, 0x00U, 0x41U, 0x92U, 0x08U, 0x20U, 0x03U, 0x42U, 0x01U,
        0x7CU, 0x22U, 0x03U, 0x3CU, 0x00U, 0x00U, 0x41U, 0x97U, 0x08U, 0x20U,
        0x02U, 0x42U, 0x08U, 0x88U, 0x3CU, 0x00U, 0x00U, 0x41U, 0x96U, 0x08U,
        0x20U, 0x02U, 0x42U, 0x10U, 0x88U, 0x3CU, 0x00U, 0x00U, 0x41U, 0x95U,
        0x08U, 0x20U, 0x02U, 0x42U, 0x18U, 0x88U, 0x3CU, 0x00U, 0x00U, 0x41U,
        0x91U, 0x08U, 0x20U, 0x03U, 0x42U, 0x08U, 0x88U, 0x3CU, 0x00U, 0x00U,
        0x41U, 0x90U, 0x08U, 0x20U, 0x03U, 0x42U, 0x10U, 0x88U, 0x3CU, 0x00U,
        0x00U, 0x41U, 0x8FU, 0x08U, 0x20U, 0x03U, 0x42U, 0x18U, 0x88U, 0x3CU,
        0x00U, 0x00U, 0x41U, 0x82U, 0xDDU, 0x03U, 0x41U, 0x06U, 0x41U, 0x80U,
        0x08U, 0x20U, 0x01U, 0x41U, 0x01U, 0x10U, 0x07U, 0x1AU, 0x41U, 0x88U,
        0xDDU, 0x03U, 0x41U, 0x0BU, 0x20U, 0x00U, 0x41U, 0x20U, 0x41U, 0x80U,
        0x08U, 0x20U, 0x01U, 0x10U, 0x0BU, 0x22U, 0x02U, 0x10U, 0x0CU, 0x1AU,
        0x20U, 0x02U, 0x42U, 0x7FU, 0x57U, 0x04U, 0x40U, 0x41U, 0x94U, 0xDDU,
        0x03U, 0x41U, 0x17U, 0x42U, 0xF7U, 0x00U, 0x10U, 0x05U, 0x1AU, 0x0BU,
        0x41U, 0xABU, 0xDDU, 0x03U, 0x41U, 0x24U, 0x42U, 0xFAU, 0x00U, 0x10U,
        0x0DU, 0x1AU, 0x20U, 0x00U, 0x41U, 0x20U, 0x6AU, 0x24U, 0x00U, 0x20U,
        0x02U, 0x0BU, 0x0BU, 0xC4U, 0x01U, 0x04U, 0x00U, 0x41U, 0x80U, 0x08U,
        0x0BU, 0x24U, 0x12U, 0x00U, 0x60U, 0x22U, 0x80U, 0x00U, 0x00U, 0x00U,
        0x24U, 0x00U, 0x00U, 0x00U, 0x00U, 0x20U, 0x1AU, 0x00U, 0x00U, 0x00U,
        0x00U, 0x20U, 0x1BU, 0x00U, 0x00U, 0x00U, 0x00U, 0x68U, 0x40U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x73U, 0x21U, 0x00U, 0x41U,
        0xC5U, 0x08U, 0x0BU, 0x16U, 0x81U, 0x14U, 0xB5U, 0xF7U, 0x62U, 0x79U,
        0x8AU, 0x53U, 0xD5U, 0x43U, 0xA0U, 0x14U, 0xCAU, 0xF8U, 0xB2U, 0x97U,
        0xCFU, 0xF8U, 0xF2U, 0xF9U, 0x37U, 0xE8U, 0x00U, 0x41U, 0xCFU, 0x09U,
        0x0BU, 0x02U, 0xF0U, 0x60U, 0x00U, 0x41U, 0xE0U, 0xDCU, 0x03U, 0x0BU,
        0x6EU, 0x4DU, 0x69U, 0x6EU, 0x74U, 0x54U, 0x65U, 0x73U, 0x74U, 0x3AU,
        0x20U, 0x41U, 0x73U, 0x73U, 0x65U, 0x72U, 0x74U, 0x69U, 0x6FU, 0x6EU,
        0x20U, 0x66U, 0x61U, 0x69U, 0x6CU, 0x75U, 0x72U, 0x65U, 0x2EU, 0x00U,
        0x54U, 0x78U, 0x6EU, 0x3AU, 0x00U, 0x65U, 0x6DU, 0x69U, 0x74U, 0x3AU,
        0x00U, 0x65U, 0x6DU, 0x69U, 0x74U, 0x5FU, 0x72U, 0x65U, 0x73U, 0x75U,
        0x6CU, 0x74U, 0x00U, 0x4DU, 0x69U, 0x6EU, 0x74U, 0x54U, 0x65U, 0x73U,
        0x74U, 0x3AU, 0x20U, 0x45U, 0x6DU, 0x69U, 0x74U, 0x20U, 0x66U, 0x61U,
        0x69U, 0x6CU, 0x65U, 0x64U, 0x2EU, 0x00U, 0x4DU, 0x69U, 0x6EU, 0x74U,
        0x54U, 0x65U, 0x73U, 0x74U, 0x3AU, 0x20U, 0x45U, 0x6DU, 0x69U, 0x74U,
        0x74U, 0x65U, 0x64U, 0x20U, 0x74U, 0x78U, 0x6EU, 0x20U, 0x73U, 0x75U,
        0x63U, 0x63U, 0x65U, 0x73U, 0x73U, 0x66U, 0x75U, 0x6CU, 0x6CU, 0x79U,
        0x2EU};

    // AccountSet
    // | otxn | tsh | claim |
    // |   A  |  A  |   S   |

    void
    testAccountSetTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{
            *this,
            network::makeNetworkConfig(21337, "10", "1000000", "200000"),
            features};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        // set tsh collect on bob
        env(fset(bob, asfTshCollect));
        env.close();

        // set tsh hook on bob
        env(hook(bob, {{hso(TshHook, collectFlag)}}, 0),
             fee(XRP(1)),
             ter(tesSUCCESS));
        env.close();

         // account set
        // env(acctdelete(alice, bob), ter(tesSUCCESS));

        incLgrSeqForAccDel(env, alice);
        env(acctdelete(alice, bob), fee(XRP(2)), ter(tesSUCCESS));
         env.close();

        // get the emitted txn id
        // verify tsh hook triggered
         Json::Value params;
        params[jss::transaction] =
        env.tx()->getJson(JsonOptions::none)[jss::hash];
        params[jss::transaction] =
            env.tx()->getJson(JsonOptions::none)[jss::hash];
         auto const jrr = env.rpc("json", "tx", to_string(params));
        std::cout << "jrr: " << jrr << "\n";
         auto const meta = jrr[jss::result][jss::meta];
        auto const emissions = meta[sfHookEmissions.jsonName];
        auto const emission = emissions[0u][sfHookEmission.jsonName];
        auto const txId = emission[sfEmittedTxnID.jsonName];
        auto const executions = meta[sfHookExecutions.jsonName];
        auto const execution = executions[0u][sfHookExecution.jsonName];
        BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
        BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
    }

    // AccountDelete
    // | otxn | tsh | claim |
    // |   A  |  A  |   S   |
    // |   A  |  D  |   S   |

    void
    testAccountDeleteTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{
            *this,
            network::makeNetworkConfig(21337, "10", "1000000", "200000"),
            features};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        // set tsh collect on bob
        env(fset(bob, asfTshCollect));
        env.close();

        // set tsh hook on bob
        env(hook(bob, {{hso(TshHook, collectFlag)}}, 0),
             fee(XRP(1)),
             ter(tesSUCCESS));
        env.close();

         // ttACCOUNT_DELETE
        // env(acctdelete(alice, bob), ter(tesSUCCESS));

        incLgrSeqForAccDel(env, alice);
        env(acctdelete(alice, bob), fee(XRP(2)), ter(tesSUCCESS));
         env.close();

        // get the emitted txn id
        // verify tsh hook triggered
         Json::Value params;
        params[jss::transaction] =
        env.tx()->getJson(JsonOptions::none)[jss::hash];
        params[jss::transaction] =
            env.tx()->getJson(JsonOptions::none)[jss::hash];
         auto const jrr = env.rpc("json", "tx", to_string(params));
        std::cout << "jrr: " << jrr << "\n";
         auto const meta = jrr[jss::result][jss::meta];
        auto const emissions = meta[sfHookEmissions.jsonName];
        auto const emission = emissions[0u][sfHookEmission.jsonName];
        auto const txId = emission[sfEmittedTxnID.jsonName];
        auto const executions = meta[sfHookExecutions.jsonName];
        auto const execution = executions[0u][sfHookExecution.jsonName];
        BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
        BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
    }

    // Check
    // | otxn | tsh | cancel | create | cash |
    // |   A  |  A  |   S    |   S    |  N   |
    // |   A  |  D  |   W    |   S    |  N   |
    // |   D  |  D  |   S    |   N    |  S   |
    // |   D  |  A  |   W    |   N    |  W   |

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
            env(check::create(account, dest, XRP(100)),
                ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel check
            env(check::cancel(account, checkId), fee(XRP(1)),
            ter(tesSUCCESS)); env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] ==
            "00000000");
        }

        // otxn: account
        // tsh destination
        // w/s: weak
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
            env(fset(dest, asfTshCollect));
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)),
                ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel check
            env(check::cancel(account, checkId), fee(XRP(1)),
            ter(tesSUCCESS)); env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] ==
            "00000001");
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
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
            env(check::create(account, dest, XRP(100)),
                ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel check
            env(check::cancel(dest, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] ==
            "00000000");
        }

        // otxn: dest
        // tsh account
        // w/s: weak
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
            env(fset(account, asfTshCollect));
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)),
                ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel check
            env(check::cancel(dest, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] ==
            "00000001");
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
            env(check::create(account, dest, XRP(100)),
                ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cash check
            env(check::cash(dest, checkId, XRP(100)), fee(XRP(1)),
            ter(tesSUCCESS)); env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            std::cout << "jrr: " << jrr << "\n";
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] ==
            "00000000");
        }

        // otxn: dest
        // tsh account
        // w/s: weak
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
            env(fset(account, asfTshCollect));
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)),
                ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cash check
            env(check::cash(dest, checkId, XRP(100)), fee(XRP(1)),
            ter(tesSUCCESS)); env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            std::cout << "jrr: " << jrr << "\n";
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] ==
            "00000001");
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
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // create check
            env(check::create(account, dest, XRP(100)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] ==
            "00000000");
        }

        // otxn: account
        // tsh destination
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // create check
            env(check::create(account, dest, XRP(100)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] ==
            "00000000");
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
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("issuer");
            env.fund(XRP(1000), account, issuer);
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // claim reward
            env(reward::claim(account, issuer), fee(XRP(1)),
            ter(tesSUCCESS)); env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] ==
            "00000000");
        }

        // otxn: account
        // tsh issuer
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("issuer");
            env.fund(XRP(1000), account, issuer);
            env.close();

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // claim reward
            env(reward::claim(account, issuer), fee(XRP(1)),
            ter(tesSUCCESS)); env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] ==
            "00000000");
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // deposit preauth
            env(deposit::auth(account, authed), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh authorize
        // w/s: strong
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

            // set tsh hook
            env(hook(authed, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // deposit preauth
            env(deposit::auth(account, authed), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }
    }

    // Escrow
    // | otxn  | tsh | cancel | create | finish |
    // |   A   |  A  |    S   |    S   |    S   |
    // |   A   |  D  |    W   |    S   |    W   |
    // |   D   |  D  |    S   |    N   |    S   |
    // |   D   |  A  |    W   |    N   |    W   |

    static Json::Value
    escrowCreate(
        jtx::Account const& account,
        jtx::Account const& to,
        STAmount const& amount)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::EscrowCreate;
        jv[jss::Flags] = tfUniversal;
        jv[jss::Account] = account.human();
        jv[jss::Destination] = to.human();
        jv[jss::Amount] = amount.getJson(JsonOptions::none);
        return jv;
    }

    static Json::Value
    escrowFinish(
        jtx::Account const& account,
        jtx::Account const& from,
        std::uint32_t seq)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::EscrowFinish;
        jv[jss::Flags] = tfUniversal;
        jv[jss::Account] = account.human();
        jv[sfOwner.jsonName] = from.human();
        jv[sfOfferSequence.jsonName] = seq;
        return jv;
    }

    static Json::Value
    escrowCancel(
        jtx::Account const& account,
        jtx::Account const& from,
        std::uint32_t seq)
    {
        Json::Value jv;
        jv[jss::TransactionType] = jss::EscrowCancel;
        jv[jss::Flags] = tfUniversal;
        jv[jss::Account] = account.human();
        jv[sfOwner.jsonName] = from.human();
        jv[sfOfferSequence.jsonName] = seq;
        return jv;
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
            auto createTx = escrowCreate(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel escrow
            env(escrowCancel(account, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: strong
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
            env(fset(dest, asfTshCollect));
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrowCreate(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel escrow
            env(escrowCancel(account, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
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
            auto createTx = escrowCreate(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel escrow
            env(escrowCancel(dest, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: dest
        // tsh account
        // w/s: strong
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
            env(fset(account, asfTshCollect));
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrowCreate(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel escrow
            env(escrowCancel(dest, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
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
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // create escrow
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrowCreate(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // create escrow
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrowCreate(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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
            auto createTx = escrowCreate(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // finish escrow
            env(escrowFinish(account, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: weak
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
            env(fset(dest, asfTshCollect));
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrowCreate(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // finish escrow
            env(escrowFinish(account, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
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
            auto createTx = escrowCreate(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // finish escrow
            env(escrowFinish(dest, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: dest
        // tsh account
        // w/s: weak
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
            env(fset(account, asfTshCollect));
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrowCreate(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // finish escrow
            env(escrowFinish(dest, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set mint hook on master
            env(hook(issuer, {{hso(MintTestHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            env(invoke::invoke(
                    account,
                    issuer,
                    invoke::makeBlob({
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
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution1 = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution1[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution1[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh issuer
        // w/s: weak
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

            // set tsh hook && set mint hook on master
            env(hook(issuer, {{hso(TshHook, overrideFlag), hso(MintTestHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            env(invoke::invoke(
                    account,
                    issuer,
                    invoke::makeBlob({
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
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution1 = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution1[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution1[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh bene
        // w/s: weak
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
            env(fset(bene, asfTshCollect));
            env.close();

            // set tsh hook
            env(hook(bene, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set mint hook on master
            env(hook(issuer, {{hso(MintTestHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            env(invoke::invoke(
                    account,
                    issuer,
                    invoke::makeBlob({
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
            auto const executions1 = meta1[sfHookExecutions.jsonName];
            auto const execution1 = executions1[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution1[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution1[sfHookReturnString.jsonName] == "00000001");
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
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkVLConfig(
                    21337, "10", "1000000", "200000", keys),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("bob");
            env.fund(XRP(1000), account, issuer);
            env.close();

            // burn down the total ledger coins so that genesis mints don't mint
            // above 100B tripping invariant
            env(noop(env.master), fee(XRP(10'000'000ULL)));
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // import
            env(import::import(
                    account,
                    import::loadXpop(ImportTCAccountSet::w_seed),
                    issuer),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // get the emitted txn id
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution1 = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution1[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution1[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh issuer
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkVLConfig(
                    21337, "10", "1000000", "200000", keys),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("bob");
            env.fund(XRP(1000), account, issuer);
            env.close();

            // burn down the total ledger coins so that genesis mints don't mint
            // above 100B tripping invariant
            env(noop(env.master), fee(XRP(10'000'000ULL)));
            env.close();

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // import
            env(import::import(
                    account,
                    import::loadXpop(ImportTCAccountSet::w_seed),
                    issuer),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // get the emitted txn id
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution1 = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution1[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution1[sfHookReturnString.jsonName] == "00000000");
        }
    }

    // Invoke
    // | otxn  | tsh | invoke |
    // |   A   |  A  |    S   |
    // |   A   |  D  |    W   |

    void
    testInvokeTSH(FeatureBitset features)
    {
        testcase("invoke tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttINVOKE
            env(invoke::invoke(account, dest), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttINVOKE
            env(invoke::invoke(account, dest), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }
    }

    // Offer
    // | otxn  | tsh | cancel | create |
    // |   A   |  A  |    S   |    S   |
    // |   A   |  C  |    N  |    W   |

    void
    testOfferCancelTSH(FeatureBitset features)
    {
        testcase("offer cancel tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel offer
            env(offer_cancel(account, offerSeq), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // create offer
            env(offer(account, USD(1000), XRP(1000)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh cross
        // w/s: weak
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

            // set tsh collect on bob
            env(fset(cross, asfTshCollect));

            // gw create offer
            env(offer(gw, USD(1000), XRP(1000)));
            env.close();

            // set tsh hook
            env(hook(cross, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // create offer
            env(offer(account, USD(1000), XRP(1000)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
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
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // payment
            env(pay(account, dest, XRP(1)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // payment
            env(pay(account, dest, XRP(1)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh cross
        // w/s: weak
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
            env(fset(cross, asfTshCollect));

            // set tsh hook
            env(hook(cross, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // payment
            env(pay(account, dest, USDB(10)), paths(USDA), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
        }
    }

    // PaymentChannel
    // | otxn  | tsh | claim | create | fund |
    // |   A   |  A  |   S   |   S    |   S  |
    // |   A   |  D  |   W   |   W    |   W  |
    // |   D   |  D  |   S   |   W    |   W  |
    // |   D   |  A  |   W   |   W    |   W  |

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
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            env(paychan::claim(account, chan, reqBal, authAmt, account.pk()),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // // otxn: account
        // // tsh account
        // // w/s: strong
        // {
        //     test::jtx::Env env{
        //         *this,
        //         network::makeNetworkConfig(21337, "10", "1000000", "200000"),
        //         features};

        //     auto const account = Account("alice");
        //     auto const dest = Account{"bob"};
        //     env.fund(XRP(1000), account, dest);
        //     env.close();

        //     // create paychannel
        //     auto const pk = account.pk();
        //     auto const settleDelay = 100s;
        //     auto const chan = channel(account, bob, env.seq(account));
        //     env(paychan::create(account, bob, XRP(10), settleDelay, pk),
        //         fee(XRP(1)),
        //         ter(tesSUCCESS));
        //     env.close();

        //     // set tsh hook
        //     env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
        //         fee(XRP(1)),
        //         ter(tesSUCCESS));
        //     env.close();

        //     auto const delta = XRP(1);
        //     auto const reqBal = delta;
        //     auto const authAmt = reqBal XRP(1);

        //     // claim paychannel
        //     auto const sig =
        //         signClaimAuth(account.pk(), account.sk(), chan, authAmt);
        //     env(paychan::claim(
        //             bob, chan, reqBal, authAmt, Slice(sig), account.pk()),
        //         fee(XRP(1)),
        //         ter(tesSUCCESS));
        //     env.close();

        //     // verify tsh hook triggered
        //     Json::Value params;
        //     params[jss::transaction] =
        //         env.tx()->getJson(JsonOptions::none)[jss::hash];
        //     auto const jrr = env.rpc("json", "tx", to_string(params));
        //     auto const meta = jrr[jss::result][jss::meta];
        //     auto const executions = meta[sfHookExecutions.jsonName];
        //     auto const execution = executions[0u][sfHookExecution.jsonName];
        //     BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
        //     BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        // }

        // test::jtx::Env env{
        //     *this,
        //     network::makeNetworkConfig(21337, "10", "1000000", "200000"),
        //     features};

        // auto const alice = Account("alice");
        // auto const bob = Account("bob");
        // auto const gw = Account{"gateway"};
        // auto const USD = gw["USD"];
        // env.fund(XRP(1000), alice, bob, gw);
        // env.close();

        // // ttPAYCHAN_CREATE
        // auto const pk = alice.pk();
        // auto const settleDelay = 100s;
        // auto const chan = channel(alice, bob, env.seq(alice));
        // env(paychan::create(alice, bob, XRP(10), settleDelay, pk),
        // fee(XRP(1))); env.close();

        // // set tsh collect on bob
        // env(fset(bob, asfTshCollect));

        // // set tsh hook on alice
        // env(hook(bob, {{hso(TshHook, collectFlag)}}, 0),
        //     fee(XRP(1)),
        //     ter(tesSUCCESS));
        // env.close();

        // auto const delta = XRP(1);
        // auto const reqBal = delta;
        // auto const authAmt = reqBal XRP(1);

        // // Strong Execution Destination
        // {

        //     // ttPAYCHAN_CLAIM  Account
        //     auto const sig = signClaimAuth(alice.pk(), alice.sk(), chan,
        //     authAmt); env(paychan::claim(bob, chan, reqBal, authAmt,
        //     Slice(sig), alice.pk()), fee(XRP(1))); env.close();

        //     // verify tsh hook triggered
        //     Json::Value params;
        //     params[jss::transaction] =
        //         env.tx()->getJson(JsonOptions::none)[jss::hash];
        //     auto const jrr = env.rpc("json", "tx", to_string(params));
        //     auto const meta = jrr[jss::result][jss::meta];
        //     auto const executions = meta[sfHookExecutions.jsonName];
        //     auto const execution =
        // executions[0u][sfHookExecution.jsonName];
        //     BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
        //     BEAST_EXPECT(execution[sfHookReturnString.jsonName] ==
        //     "00000000");
        // }

        // Weak Execution Destination
        // {
        //     // ttPAYCHAN_CLAIM  Account
        //     env(paychan::claim(alice, chan, reqBal, authAmt), fee(XRP(1)));
        //     env.close();

        //     // verify tsh hook triggered
        //     Json::Value params;
        //     params[jss::transaction] =
        //         env.tx()->getJson(JsonOptions::none)[jss::hash];
        //     auto const jrr = env.rpc("json", "tx", to_string(params));
        //     auto const meta = jrr[jss::result][jss::meta];
        //     auto const executions = meta[sfHookExecutions.jsonName];
        //     auto const execution = executions[0u][sfHookExecution.jsonName];
        //     BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
        //     BEAST_EXPECT(execution[sfHookReturnString.jsonName] ==
        //     "00000001");
        // }
    }

    // | otxn | tfBurnable | tsh | mint | burn | buy | sell | cancel
    // |   O  |    false   |  O  |   N  |   S  |  S  |   S  |   S
    // |   O  |    false   |  I  |   N  |   W  |  W  |   W  |   N
    // |   O  |    false   |  B  |   N  |   N  |  N  |   S  |   W
    // |   O  |    true    |  B  |   N  |   N  |  N  |   S  |   W
    // |   O  |    true    |  O  |   N  |   S  |  S  |   S  |   S
    // |   O  |    true    |  I  |   N  |   W  |  S  |   S  |   N
    // |   I  |    false   |  O  |   N  |   N  |  N  |   N  |   N
    // |   I  |    false   |  I  |   S  |   N  |  N  |   N  |   N
    // |   I  |    false   |  B  |   S  |   N  |  N  |   N  |   N
    // |   I  |    true    |  O  |   N  |   W  |  N  |   N  |   N
    // |   I  |    true    |  I  |   S  |   S  |  N  |   N  |   N
    // |   I  |    true    |  B  |   S  |   N  |  N  |   N  |   N
    // |   B  |    true    |  O  |   N  |   N  |  ?  |   N  |   N
    // |   B  |    true    |  B  |   N  |   N  |  ?  |   N  |   N

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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: not burnable
        // tsh issuer
        // w/s: weak
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

            env(fset(issuer, asfTshCollect));
            env.close();

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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            std::cout << "RESULT: " << jrr << "\n";
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
        }

        // otxn: owner
        // flag: burnable
        // tsh owner
        // w/s: strong
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: burnable
        // tsh issuer
        // w/s: weak
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

            env(fset(issuer, asfTshCollect));
            env.close();

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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            std::cout << "RESULT: " << jrr << "\n";
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
        }

        // otxn: Issuer
        // flag: Burnable
        // tsh issuer
        // w/s: strong
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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttURITOKEN_BURN
            env(uritoken::burn(issuer, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: Issuer
        // flag: Burnable
        // tsh owner
        // w/s: weak
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

            env(fset(owner, asfTshCollect));
            env.close();

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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttURITOKEN_BURN
            env(uritoken::burn(issuer, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            std::cout << "RESULT: " << jrr << "\n";
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
        }
    }

    void
    testURITokenBuyTSH(FeatureBitset features)
    {
        testcase("uritoken buy tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: buyer
        // tsh owner
        // w/s: strong
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(buyer, hexid),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: buyer
        // tsh buyer
        // w/s: strong
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

            // set tsh hook
            env(hook(buyer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(buyer, hexid),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: not burnable
        // tsh owner
        // w/s: strong
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: not burnable
        // tsh issuer
        // w/s: weak
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

            env(fset(issuer, asfTshCollect));
            env.close();

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            std::cout << "RESULT: " << jrr << "\n";
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
        }

        // otxn: owner
        // flag: burnable
        // tsh owner
        // w/s: strong
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: burnable
        // tsh issuer
        // w/s: strong
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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            std::cout << "RESULT: " << jrr << "\n";
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: not burnable
        // tsh buyer
        // w/s: weak
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

            env(fset(issuer, asfTshCollect));
            env.close();

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

            // set tsh hook
            env(hook(buyer, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            std::cout << "RESULT: " << jrr << "\n";
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
        }

        // otxn: owner
        // flag: burnable
        // tsh buyer
        // w/s: weak
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

            env(fset(issuer, asfTshCollect));
            env.close();

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

            // set tsh hook
            env(hook(buyer, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            std::cout << "RESULT: " << jrr << "\n";
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
        }

        // otxn: owner
        // flag: not burnable
        // tsh owner
        // w/s: strong
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: not burnable
        // tsh issuer
        // w/s: weak
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

            env(fset(issuer, asfTshCollect));
            env.close();

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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
        }

        // otxn: owner
        // flag: not burnable
        // tsh buyer
        // w/s: strong
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

            // set tsh hook
            env(hook(buyer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: not burnable
        // tsh buyer
        // w/s: strong
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

            // set tsh hook
            env(hook(buyer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            std::cout << "RESULT: " << jrr << "\n";
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: burnable
        // tsh owner
        // w/s: strong
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: burnable
        // tsh issuer
        // w/s: strong
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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            std::cout << "RESULT: " << jrr << "\n";
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }
    }

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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: issuer
        // flag: not burnable
        // tsh buyer
        // w/s: strong
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

            // set tsh hook
            env(hook(buyer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            std::cout << "RESULT: " << jrr << "\n";
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: issuer
        // flag: burnable
        // tsh issuer
        // w/s: strong
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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: issuer
        // flag: burnable
        // tsh buyer
        // w/s: strong
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

            // set tsh hook
            env(hook(buyer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            std::cout << "RESULT: " << jrr << "\n";
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }
    }

    Json::Value
    getAcceptHook()
    {
        static const std::vector<uint8_t> AcceptHook = {
            0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,
            0x13U, 0x03U, 0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U,
            0x03U, 0x7FU, 0x7FU, 0x7EU, 0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU,
            0x01U, 0x7EU, 0x02U, 0x17U, 0x02U, 0x03U, 0x65U, 0x6EU, 0x76U,
            0x02U, 0x5FU, 0x67U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6EU, 0x76U,
            0x06U, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x01U,
            0x03U, 0x02U, 0x01U, 0x02U, 0x05U, 0x03U, 0x01U, 0x00U, 0x02U,
            0x06U, 0x21U, 0x05U, 0x7FU, 0x01U, 0x41U, 0x80U, 0x88U, 0x04U,
            0x0BU, 0x7FU, 0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0x7FU, 0x00U,
            0x41U, 0x80U, 0x08U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0x80U, 0x88U,
            0x04U, 0x0BU, 0x7FU, 0x00U, 0x41U, 0x80U, 0x08U, 0x0BU, 0x07U,
            0x08U, 0x01U, 0x04U, 0x68U, 0x6FU, 0x6FU, 0x6BU, 0x00U, 0x02U,
            0x0AU, 0xA2U, 0x80U, 0x00U, 0x01U, 0x9EU, 0x80U, 0x00U, 0x01U,
            0x01U, 0x7EU, 0x41U, 0x01U, 0x41U, 0x01U, 0x10U, 0x80U, 0x80U,
            0x80U, 0x80U, 0x00U, 0x1AU, 0x41U, 0x00U, 0x41U, 0x00U, 0x42U,
            0x00U, 0x10U, 0x81U, 0x80U, 0x80U, 0x80U, 0x00U, 0x1AU, 0x20U,
            0x01U, 0x0BU};
        using namespace jtx;
        Json::Value hook;
        hook[jss::Hook] = Json::objectValue;
        hook[jss::Hook][jss::HookOn] =
            "0000000000000000000000000000000000000000000000000000000000000000";
        hook[jss::Hook][jss::HookNamespace] =
            "0000000000000000000000000000000000000000000000000000000000000000";
        hook[jss::Hook][jss::HookApiVersion] = 0;
        hook[jss::Hook][jss::Flags] = 1;
        hook[jss::Hook][jss::CreateCode] = strHex(AcceptHook);
        return hook;
    }

    void
    testMaxHookExecutions(FeatureBitset features)
    {
        testcase("max hook executions");

        using namespace test::jtx;
        using namespace std::literals;
        {
            // test::jtx::Env env{
            //     *this,
            //     network::makeNetworkConfig(21337, "10", "1000000", "200000"),
            //     nullptr,
            //     beast::severities::kTrace};

            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(100000), alice, bob);
            env.close();

            Json::Value tx1;
            tx1[jss::Account] = alice.human();
            tx1[jss::TransactionType] = "SetHook";
            tx1[jss::Hooks] = Json::arrayValue;
            for (int i = 0; i < 10; i++)
            {
                tx1[jss::Hooks][i] = getAcceptHook();
            }
            env(tx1, fee(XRP(10)), ter(tesSUCCESS));
            env.close();

            Json::Value tx2;
            tx2[jss::Account] = bob.human();
            tx2[jss::TransactionType] = "SetHook";
            tx2[jss::Hooks] = Json::arrayValue;
            for (int i = 0; i < 10; i++)
            {
                tx2[jss::Hooks][i] = getAcceptHook();
            }
            env(tx2, fee(XRP(10)), ter(tesSUCCESS));
            env.close();

            env(pay(alice, bob, XRP(10)), fee(XRP(100)));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            BEAST_EXPECT(executions.size() == 20);
        }
    }

    void
    testMaxHookExecutionsInvalid(FeatureBitset features)
    {
        testcase("max hook executions invalid");

        using namespace test::jtx;
        using namespace std::literals;
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(100000), alice, bob);
            env.close();

            Json::Value tx1;
            tx1[jss::Account] = alice.human();
            tx1[jss::TransactionType] = "SetHook";
            tx1[jss::Hooks] = Json::arrayValue;
            for (int i = 0; i < 10; i++)
            {
                tx1[jss::Hooks][i] = getAcceptHook();
            }
            env(tx1, fee(XRP(10)), ter(tesSUCCESS));

            Json::Value tx2;
            tx2[jss::Account] = bob.human();
            tx2[jss::TransactionType] = "SetHook";
            tx2[jss::Hooks] = Json::arrayValue;
            for (int i = 0; i < 10; i++)
            {
                tx2[jss::Hooks][i] = getAcceptHook();
            }
            env(tx2, fee(XRP(10)), ter(tesSUCCESS));

            env(pay(alice, bob, XRP(10)), fee(XRP(100)));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            BEAST_EXPECT(executions.size() == 10);
        }
    }

    // void
    // testMaxHookEmissions(FeatureBitset features)
    // {
    //     testcase("max hook emissions");

    //     using namespace test::jtx;
    //     using namespace std::literals;
    //     {
    //         test::jtx::Env env{
    //             *this,
    //             network::makeNetworkConfig(21337, "10", "1000000", "200000"),
    //             nullptr,
    //             beast::severities::kTrace};

    //         auto const alice = Account("alice");
    //         auto const bob = Account("bob");
    //         env.fund(XRP(100000), alice, bob);
    //         env.close();

    //         Json::Value tx1;
    //         tx1[jss::Account] = alice.human();
    //         tx1[jss::TransactionType] = "SetHook";
    //         tx1[jss::Hooks] = Json::arrayValue;
    //         for(int i = 0; i < 9; i++) {
    //             tx1[jss::Hooks][i] = getAcceptHook();
    //         }
    //         env(tx1, fee(XRP(10)));

    //         Json::Value tx2;
    //         tx2[jss::Account] = bob.human();
    //         tx2[jss::TransactionType] = "SetHook";
    //         tx2[jss::Hooks] = Json::arrayValue;
    //         for(int i = 0; i < 1; i++) {
    //             tx2[jss::Hooks][i] = getAcceptHook();
    //         }
    //         env(tx2, fee(XRP(10)));

    //         env(pay(alice, bob, XRP(10)), fee(XRP(2)));
    //         env.close();

    //         // verify tsh hook triggered
    //         Json::Value params;
    //         params[jss::transaction] =
    //             env.tx()->getJson(JsonOptions::none)[jss::hash];
    //         auto const jrr = env.rpc("json", "tx", to_string(params));
    //         std::cout << "RESULT: " << jrr << "\n";
    //     }
    // }

    void
    testWithFeats(FeatureBitset features)
    {
        // testAccountSetTSH(features);
        // testAccountDeleteTSH(features);
        // testCheckCancelTSH(features);
        // testCheckCashTSH(features);
        // testCheckCreateTSH(features);
        // testClaimRewardTSH(features);
        // testDepositPreauthTSH(features);
        // testEscrowCancelTSH(features);
        // testEscrowCreateTSH(features);
        // testEscrowFinishTSH(features);
        // testGenesisMintTSH(features);
        // testImportTSH(features);
        // testInvokeTSH(features);
        // testOfferCancelTSH(features);
        // testOfferCreateTSH(features);
        // testPaymentTSH(features);
        testPaymentChannelClaimTSH(features);
        // testPaymentChannelCreateTSH(features);
        // testPaymentChannelFundTSH(features);
        // testSetHookTSH(features);
        // testSetRegularKeyTSH(features);
        // testSignersListSetTSH(features);
        // testTicketCreateTSH(features);
        // testTrustSetTSH(features);
        // testURITokenBurnTSH(features);
        // testURITokenBuyTSH(features);
        // testURITokenCancelSellOfferTSH(features);
        // testURITokenCreateSellOfferTSH(features);
        // testURITokenMintTSH(features);
        // testMaxHookExecutions(features);
        // testMaxHookExecutionsInvalid(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeats(sa);
    }
};

BEAST_DEFINE_TESTSUITE(SetHookTSH, app, ripple);

}  // namespace test
}  // namespace ripple

/*
TODO:

URITokenBurn: otxn: owner, burnable: false, tsh: issuer == Strong (Should be
weak) URITokenBurn: otxn: issuer, burnable: true, tsh: owner == Strong (Should
be weak)

*/

/*
SetHookMax: 10
HookExecutions: unlimited (only so many chain executions can occur)
EmittedTxns: 256
EmitGeneratedTxns: 10 (emit chains. Ex: emit -> trigger -> emit -> trigger)
*/