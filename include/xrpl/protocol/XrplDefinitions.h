//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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

#ifndef RIPPLE_PROTOCOL_XRPLDEFINITIONS_H_INCLUDED
#define RIPPLE_PROTOCOL_XRPLDEFINITIONS_H_INCLUDED

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

/** A compile-time reader for the other network's `server_definitions`.

    The JSON a rippled returns from `server_definitions` (the same shape as
    ripple-binary-codec's definitions.json) is checked into the tree as
    src/libxrpl/protocol/xrpl_server_definitions.json; CMake wraps it as a
    string literal in the build tree (xrpl_server_definitions.h.in beside
    it), there being no #embed before C++26. parseXrplDefinitions below
    reads the parts Export needs out of it entirely at compile time:

      TYPES              type name -> STI code, to resolve FIELDS[].type
      FIELDS             every serialized field: name, type, nth, flags
      TRANSACTION_TYPES  transaction name -> type code
      TRANSACTION_FORMATS per transaction: its own fields and optionality
                         (0 required, 1 optional, 2 default)
      TRANSACTION_FLAGS  per transaction: its tf* flags (and "universal")
      hash               the definitions hash the node reported

    Everything else in the file is skipped. The reader is deliberately only
    as general as that file: any shape it does not expect fails the build,
    which is the point of reading it at compile time.
*/
namespace ripple {
namespace xrpl_defs {

struct TypeDef
{
    std::string_view name;
    int value;
};

struct FieldDef
{
    std::string_view name;
    std::string_view typeName;  // as written; `type` is resolved from TYPES
    int type;
    int nth;
    bool vlEncoded;
    bool serialized;
    bool signing;
};

struct TxTypeDef
{
    std::string_view name;
    int code;
};

struct FormatField
{
    std::string_view name;
    int optionality;  // 0 required, 1 optional, 2 default
};

struct TxFormatDef
{
    std::string_view name;
    std::size_t first;  // into Definitions::formatFields
    std::size_t count;
};

struct TxFlagDef
{
    std::string_view tx;  // transaction name, or "universal"
    std::string_view name;
    std::uint32_t value;
};

struct Counts
{
    std::size_t types = 0;
    std::size_t fields = 0;
    std::size_t txTypes = 0;
    std::size_t formats = 0;
    std::size_t formatFields = 0;
    std::size_t flags = 0;
};

template <Counts N>
struct Definitions
{
    std::string_view hash;
    std::array<TypeDef, N.types> types;
    std::array<FieldDef, N.fields> fields;
    std::array<TxTypeDef, N.txTypes> txTypes;
    std::array<TxFormatDef, N.formats> formats;
    std::array<FormatField, N.formatFields> formatFields;
    std::array<TxFlagDef, N.flags> flags;

    constexpr TypeDef const*
    type(std::string_view name) const
    {
        for (auto const& t : types)
            if (t.name == name)
                return &t;
        return nullptr;
    }

    constexpr FieldDef const*
    field(std::string_view name) const
    {
        for (auto const& f : fields)
            if (f.name == name)
                return &f;
        return nullptr;
    }

    constexpr TxTypeDef const*
    txType(std::string_view name) const
    {
        for (auto const& t : txTypes)
            if (t.name == name)
                return &t;
        return nullptr;
    }

    constexpr TxFormatDef const*
    format(std::string_view name) const
    {
        for (auto const& f : formats)
            if (f.name == name)
                return &f;
        return nullptr;
    }

    /** The tf* flags of transaction `tx`, OR'd: its own only, not the
        "universal" ones every transaction there may carry */
    constexpr std::uint32_t
    flagsMask(std::string_view tx) const
    {
        std::uint32_t mask = 0;
        for (auto const& f : flags)
            if (f.tx == tx)
                mask |= f.value;
        return mask;
    }
};

namespace detail {

// Not constexpr: calling it during constant evaluation is an error, which
// turns a malformed definitions file into a build failure at the call site.
[[noreturn]] void
malformed(char const* what);

struct Cursor
{
    std::string_view s;
    std::size_t i = 0;

    constexpr bool
    eof() const
    {
        return i >= s.size();
    }

    constexpr char
    peek() const
    {
        return eof() ? '\0' : s[i];
    }

    constexpr void
    ws()
    {
        while (!eof() &&
               (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t'))
            ++i;
    }

    constexpr bool
    consume(char c)
    {
        ws();
        if (peek() != c)
            return false;
        ++i;
        return true;
    }

    constexpr void
    expect(char c)
    {
        if (!consume(c))
            malformed("unexpected character");
    }

    constexpr bool
    consumeWord(std::string_view w)
    {
        ws();
        if (s.substr(i, w.size()) != w)
            return false;
        i += w.size();
        return true;
    }

    constexpr std::string_view
    string()
    {
        expect('"');
        auto const begin = i;
        while (!eof() && s[i] != '"')
        {
            if (s[i] == '\\')
                malformed("escape sequences are not supported");
            ++i;
        }
        if (eof())
            malformed("unterminated string");
        return s.substr(begin, i++ - begin);
    }

    constexpr long long
    integer()
    {
        ws();
        bool const neg = consume('-');
        if (peek() < '0' || peek() > '9')
            malformed("expected a number");
        long long v = 0;
        while (!eof() && s[i] >= '0' && s[i] <= '9')
            v = v * 10 + (s[i++] - '0');
        if (!eof() && (s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
            malformed("expected an integer");
        return neg ? -v : v;
    }

    constexpr bool
    boolean()
    {
        if (consumeWord("true"))
            return true;
        if (consumeWord("false"))
            return false;
        malformed("expected a boolean");
    }

    constexpr void
    skipValue()
    {
        ws();
        switch (peek())
        {
            case '"':
                string();
                return;
            case '{':
                ++i;
                if (consume('}'))
                    return;
                do
                {
                    string();
                    expect(':');
                    skipValue();
                } while (consume(','));
                expect('}');
                return;
            case '[':
                ++i;
                if (consume(']'))
                    return;
                do
                    skipValue();
                while (consume(','));
                expect(']');
                return;
            default:
                if (consumeWord("true") || consumeWord("false") ||
                    consumeWord("null"))
                    return;
                integer();
                return;
        }
    }
};

// Reads `json` into `out` (null: count only). Objects are read key by key,
// in whatever order they come, so the file may be re-sorted freely.
template <class D>
constexpr Counts
read(std::string_view json, D* out)
{
    Counts n;
    auto const put = [&](auto& arr, std::size_t idx, auto value) {
        if (out)
        {
            if (idx >= arr.size())
                malformed("count mismatch");
            arr[idx] = value;
        }
    };

    Cursor c{json};
    c.expect('{');
    if (!c.consume('}'))
    {
        do
        {
            auto const key = c.string();
            c.expect(':');
            if (key == "hash")
            {
                auto const h = c.string();
                if (out)
                    out->hash = h;
            }
            else if (key == "TYPES")
            {
                c.expect('{');
                if (!c.consume('}'))
                {
                    do
                    {
                        auto const name = c.string();
                        c.expect(':');
                        auto const value = static_cast<int>(c.integer());
                        if (out)
                            put(out->types, n.types, TypeDef{name, value});
                        ++n.types;
                    } while (c.consume(','));
                    c.expect('}');
                }
            }
            else if (key == "FIELDS")
            {
                c.expect('[');
                if (!c.consume(']'))
                {
                    do
                    {
                        c.expect('[');
                        FieldDef f{};
                        f.name = c.string();
                        c.expect(',');
                        c.expect('{');
                        do
                        {
                            auto const k = c.string();
                            c.expect(':');
                            if (k == "nth")
                                f.nth = static_cast<int>(c.integer());
                            else if (k == "isVLEncoded")
                                f.vlEncoded = c.boolean();
                            else if (k == "isSerialized")
                                f.serialized = c.boolean();
                            else if (k == "isSigningField")
                                f.signing = c.boolean();
                            else if (k == "type")
                                f.typeName = c.string();
                            else
                                c.skipValue();
                        } while (c.consume(','));
                        c.expect('}');
                        c.expect(']');
                        if (out)
                            put(out->fields, n.fields, f);
                        ++n.fields;
                    } while (c.consume(','));
                    c.expect(']');
                }
            }
            else if (key == "TRANSACTION_TYPES")
            {
                c.expect('{');
                if (!c.consume('}'))
                {
                    do
                    {
                        auto const name = c.string();
                        c.expect(':');
                        auto const code = static_cast<int>(c.integer());
                        if (out)
                            put(out->txTypes, n.txTypes, TxTypeDef{name, code});
                        ++n.txTypes;
                    } while (c.consume(','));
                    c.expect('}');
                }
            }
            else if (key == "TRANSACTION_FORMATS")
            {
                c.expect('{');
                if (!c.consume('}'))
                {
                    do
                    {
                        TxFormatDef fmt{c.string(), n.formatFields, 0};
                        c.expect(':');
                        c.expect('[');
                        if (!c.consume(']'))
                        {
                            do
                            {
                                c.expect('{');
                                FormatField ff{};
                                do
                                {
                                    auto const k = c.string();
                                    c.expect(':');
                                    if (k == "name")
                                        ff.name = c.string();
                                    else if (k == "optionality")
                                        ff.optionality =
                                            static_cast<int>(c.integer());
                                    else
                                        c.skipValue();
                                } while (c.consume(','));
                                c.expect('}');
                                if (out)
                                    put(out->formatFields, n.formatFields, ff);
                                ++n.formatFields;
                                ++fmt.count;
                            } while (c.consume(','));
                            c.expect(']');
                        }
                        if (out)
                            put(out->formats, n.formats, fmt);
                        ++n.formats;
                    } while (c.consume(','));
                    c.expect('}');
                }
            }
            else if (key == "TRANSACTION_FLAGS")
            {
                c.expect('{');
                if (!c.consume('}'))
                {
                    do
                    {
                        auto const tx = c.string();
                        c.expect(':');
                        c.expect('{');
                        if (!c.consume('}'))
                        {
                            do
                            {
                                auto const name = c.string();
                                c.expect(':');
                                auto const v = c.integer();
                                if (v < 0 || v > 0xFFFFFFFFLL)
                                    malformed("flag out of range");
                                if (out)
                                    put(out->flags,
                                        n.flags,
                                        TxFlagDef{
                                            tx,
                                            name,
                                            static_cast<std::uint32_t>(v)});
                                ++n.flags;
                            } while (c.consume(','));
                            c.expect('}');
                        }
                    } while (c.consume(','));
                    c.expect('}');
                }
            }
            else
                c.skipValue();
        } while (c.consume(','));
        c.expect('}');
    }
    c.ws();
    if (!c.eof())
        malformed("trailing content");

    if (out)
    {
        // resolve each field's type name against TYPES
        for (auto& f : out->fields)
        {
            auto const t = out->type(f.typeName);
            if (!t)
                malformed("field of unknown type");
            f.type = t->value;
        }
    }
    return n;
}

}  // namespace detail

template <std::string_view const& Json>
consteval Counts
countXrplDefinitions()
{
    return detail::read<Definitions<Counts{}>>(Json, nullptr);
}

/** The definitions in `Json`, read at compile time; the template parameter
    must be a constexpr string_view with static storage, e.g.
    xrplServerDefinitionsJson. */
template <std::string_view const& Json>
consteval auto
parseXrplDefinitions()
{
    constexpr Counts n = countXrplDefinitions<Json>();
    Definitions<n> d{};
    if (detail::read(Json, &d).fields != n.fields)
        detail::malformed("count mismatch");
    return d;
}

}  // namespace xrpl_defs
}  // namespace ripple

#endif
