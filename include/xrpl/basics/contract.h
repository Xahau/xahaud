//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2014 Ripple Labs Inc.

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

#ifndef RIPPLE_BASICS_CONTRACT_H_INCLUDED
#define RIPPLE_BASICS_CONTRACT_H_INCLUDED

#include <xrpl/beast/type_name.h>
#include <exception>
#include <string_view>
#include <utility>

namespace ripple {

namespace detail {

/** Throws an exception, logging its type and message before doing so. */
void
LogThrow(std::string_view type, std::string_view what);

}  // namespace detail
/** Throws an exception, logging its type and message before doing so.

    @tparam E    The exception type. Must derive from std::exception.
    @tparam Args Constructor argument types for E.

    @param args  Arguments forwarded to the constructor of E.
 */
template <class E, class... Args>
[[noreturn]] void
Throw(Args&&... args)
{
    static_assert(
        std::derived_from<E, std::exception>,
        "Exception must derive from std::exception.");

    E e{std::forward<Args>(args)...};
    detail::LogThrow(beast::type_name<E>().c_str(), e.what());
    throw e;
}

/** Logs a fatal message and terminates the process unconditionally.

    This should be called when code detects a broken invariant
    or a condition from which recovery is not possible.

    @param msg A description of the error.
 */
[[noreturn]] void
LogicError(std::string_view msg) noexcept;

}  // namespace ripple

#endif
