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

#ifndef RIPPLE_TX_URITOKEN_H_INCLUDED
#define RIPPLE_TX_URITOKEN_H_INCLUDED

#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/tx/impl/Transactor.h>
#include <ripple/basics/Log.h>
#include <ripple/protocol/Indexes.h>

namespace ripple {

class URIToken : public Transactor
{
public:
    bool inline static validateUTF8(std::vector<uint8_t> const& u)
    {
        // this code is from
        // https://www.cl.cam.ac.uk/~mgk25/ucs/utf8_check.c
        uint8_t const* s = (uint8_t const*)u.data();
        uint8_t const* end = s + u.size();
        while (s < end)
        {
            if (*s < 0x80)
                /* 0xxxxxxx */
                s++;
            else if ((s[0] & 0xe0) == 0xc0)
            {
                /* 110XXXXx 10xxxxxx */
                if ((s[1] & 0xc0) != 0x80 ||
                    (s[0] & 0xfe) == 0xc0) /* overlong? */
                    return false;
                else
                    s += 2;
            }
            else if ((s[0] & 0xf0) == 0xe0)
            {
                /* 1110XXXX 10Xxxxxx 10xxxxxx */
                if ((s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 ||
                    (s[0] == 0xe0 && (s[1] & 0xe0) == 0x80) || /* overlong? */
                    (s[0] == 0xed && (s[1] & 0xe0) == 0xa0) || /* surrogate? */
                    (s[0] == 0xef && s[1] == 0xbf &&
                     (s[2] & 0xfe) == 0xbe)) /* U+FFFE or U+FFFF? */
                    return false;
                else
                    s += 3;
            }
            else if ((s[0] & 0xf8) == 0xf0)
            {
                /* 11110XXX 10XXxxxx 10xxxxxx 10xxxxxx */
                if ((s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 ||
                    (s[3] & 0xc0) != 0x80 ||
                    (s[0] == 0xf0 && (s[1] & 0xf0) == 0x80) || /* overlong? */
                    (s[0] == 0xf4 && s[1] > 0x8f) ||
                    s[0] > 0xf4) /* > U+10FFFF? */
                    return false;
                else
                    s += 4;
            }
            else
                return false;
        }
        return true;
    }

    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit URIToken(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

}  // namespace ripple

#endif
