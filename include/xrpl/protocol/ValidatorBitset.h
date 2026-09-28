//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright 2026 Xahau

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
    IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_PROTOCOL_VALIDATORBITSET_H_INCLUDED
#define RIPPLE_PROTOCOL_VALIDATORBITSET_H_INCLUDED

#include <xrpl/basics/Blob.h>
#include <xrpl/basics/Slice.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace ripple {

class ValidatedValidatorBitset
{
    Blob bitset_;
    std::size_t memberCount_ = 0;
    std::size_t selected_ = 0;

    ValidatedValidatorBitset(
        Slice bitset,
        std::size_t memberCount,
        std::size_t selected)
        : bitset_(bitset.begin(), bitset.end())
        , memberCount_(memberCount)
        , selected_(selected)
    {
    }

public:
    ValidatedValidatorBitset(ValidatedValidatorBitset const&) = default;
    ValidatedValidatorBitset&
    operator=(ValidatedValidatorBitset const&) = default;

    ValidatedValidatorBitset(ValidatedValidatorBitset&& other) noexcept
        : bitset_(std::move(other.bitset_))
        , memberCount_(other.memberCount_)
        , selected_(other.selected_)
    {
        other.bitset_.clear();
        other.memberCount_ = 0;
        other.selected_ = 0;
    }

    ValidatedValidatorBitset&
    operator=(ValidatedValidatorBitset&& other) noexcept
    {
        if (this != &other)
        {
            bitset_ = std::move(other.bitset_);
            memberCount_ = other.memberCount_;
            selected_ = other.selected_;

            other.bitset_.clear();
            other.memberCount_ = 0;
            other.selected_ = 0;
        }
        return *this;
    }

    /** Construct an owning, validated validator bitset. */
    static std::optional<ValidatedValidatorBitset>
    make(Slice bitset, std::size_t memberCount);

    std::size_t
    selected() const
    {
        return selected_;
    }

    bool
    contains(std::size_t position) const
    {
        if (position >= memberCount_)
            return false;

        auto const byte = position / 8;
        return (bitset_[byte] &
                static_cast<std::uint8_t>(1u << (position % 8))) != 0;
    }
};

constexpr std::size_t
validatorBitsetBytes(std::size_t memberCount)
{
    return memberCount / 8 + (memberCount % 8 != 0);
}

/** Validate an LSB-first bitset over a fixed canonical validator universe.

    A valid bitset has exactly enough bytes for the universe and leaves every
    unused high bit in the final byte clear. The returned population can be
    used by a caller's profile-specific admission and quorum rules.
*/
inline std::optional<ValidatedValidatorBitset>
validateValidatorBitset(Slice bitset, std::size_t memberCount)
{
    return ValidatedValidatorBitset::make(bitset, memberCount);
}

inline std::optional<ValidatedValidatorBitset>
ValidatedValidatorBitset::make(Slice bitset, std::size_t memberCount)
{
    if (bitset.size() != validatorBitsetBytes(memberCount))
        return std::nullopt;

    auto const remainder = memberCount % 8;
    if (remainder != 0)
    {
        auto const allowed = static_cast<std::uint8_t>((1u << remainder) - 1u);
        if ((bitset[bitset.size() - 1] & static_cast<std::uint8_t>(~allowed)) !=
            0)
            return std::nullopt;
    }

    std::size_t selected = 0;
    for (auto const byte : bitset)
        selected += std::popcount(byte);

    return ValidatedValidatorBitset{bitset, memberCount, selected};
}

template <class Predicate>
Blob
makeValidatorBitset(std::size_t memberCount, Predicate&& selected)
{
    Blob bitset(validatorBitsetBytes(memberCount), 0);
    for (std::size_t i = 0; i < memberCount; ++i)
    {
        if (selected(i))
            bitset[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
    }
    return bitset;
}

}  // namespace ripple

#endif
