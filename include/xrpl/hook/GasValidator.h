//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 XRPL-Labs

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

#ifndef RIPPLE_APP_HOOK_GASVALIDATOR_H_INCLUDED
#define RIPPLE_APP_HOOK_GASVALIDATOR_H_INCLUDED

#include <xrpl/basics/Expected.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/Rules.h>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

// Forward declaration
using GuardLog =
    std::optional<std::reference_wrapper<std::basic_ostream<char>>>;

namespace hook {

/**
 * @brief Validate WASM host functions for Gas-type hooks
 *
 * Validates that a WASM binary only imports allowed host functions
 * and does not import the _g (guard) function, which is only for
 * Guard-type hooks.
 *
 * @param wasm The WASM binary to validate
 * @param guardLog Logging function for validation errors
 * @param guardLogAccStr Account string for logging
 * @return bool if validation succeeds,
 * @return true if contains cbak function
 * @return false if otherwise
 * @return error message if validation fails
 */
ripple::Expected<bool, std::string>
validateWasmHostFunctionsForGas(
    std::vector<uint8_t> const& wasm,
    ripple::Rules const& rules,
    beast::Journal const& j);

}  // namespace hook

#endif  // RIPPLE_APP_HOOK_GASVALIDATOR_H_INCLUDED
