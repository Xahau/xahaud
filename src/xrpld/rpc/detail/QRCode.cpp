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

#include <xrpld/rpc/detail/QRCode.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

namespace ripple {
namespace qr {

namespace {

// ISO/IEC 18004 table 9 by [level][version], levels L, M, Q, H
constexpr std::uint8_t eccLen[4][41] = {
    {0,  7,  10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26,
     30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30,
     30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {0,  10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22,
     24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28,
     28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},
    {0,  13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24,
     20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30,
     30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {0,  17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22,
     24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30,
     30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30}};
constexpr std::uint8_t eccBlocks[4][41] = {
    {0,  1,  1,  1,  1,  1,  2,  2,  2,  2,  4,  4,  4,  4,
     4,  6,  6,  6,  6,  7,  8,  8,  9,  9,  10, 12, 12, 12,
     13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},
    {0,  1,  1,  1,  2,  2,  4,  4,  4,  5,  5,  5,  8,  9,
     9,  10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25,
     26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
    {0,  1,  1,  2,  2,  4,  4,  6,  6,  8,  8,  8,  10, 12,
     16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34,
     35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},
    {0,  1,  1,  2,  4,  4,  4,  5,  6,  8,  8,  11, 11, 16,
     16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40,
     42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81}};
constexpr int formatLevel[4] = {1, 0, 3, 2};

constexpr std::string_view alnum =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

// modules left for codewords in version v
int
rawModules(int v)
{
    int r = (16 * v + 128) * v + 64;
    if (v >= 2)
    {
        int const k = v / 7 + 2;
        r -= (25 * k - 10) * k - 55;
        if (v >= 7)
            r -= 36;
    }
    return r;
}

int
dataCodewords(int v, int l)
{
    return rawModules(v) / 8 - eccLen[l][v] * eccBlocks[l][v];
}

// GF(2^8) mod x^8 + x^4 + x^3 + x^2 + 1
std::uint8_t
mul(int x, int y)
{
    int z = 0;
    for (int i = 7; i >= 0; --i)
    {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return z;
}

// the n Reed-Solomon codewords for `data`
std::vector<std::uint8_t>
reedSolomon(std::vector<std::uint8_t> const& data, int n)
{
    std::vector<std::uint8_t> g(n), r(n);
    g[n - 1] = 1;
    for (int i = 0, root = 1; i < n; ++i, root = mul(root, 2))
        for (int j = 0; j < n; ++j)
            g[j] = mul(g[j], root) ^ (j + 1 < n ? g[j + 1] : 0);
    for (auto const b : data)
    {
        int const f = b ^ r[0];
        std::rotate(r.begin(), r.begin() + 1, r.end());
        r[n - 1] = 0;
        for (int j = 0; j < n; ++j)
            r[j] ^= mul(g[j], f);
    }
    return r;
}

struct Symbol
{
    int n;
    std::vector<char> dark, fixed;

    explicit Symbol(int v) : n(4 * v + 17), dark(n * n), fixed(n * n)
    {
    }

    bool
    at(int x, int y) const
    {
        return dark[y * n + x];
    }

    void
    set(int x, int y, bool d)
    {
        dark[y * n + x] = d;
        fixed[y * n + x] = 1;
    }

    void
    format(int level, int mask)
    {
        int const data = formatLevel[level] << 3 | mask;
        int rem = data;
        for (int i = 0; i < 10; ++i)
            rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        int const bits = (data << 10 | rem) ^ 0x5412;
        auto const bit = [&](int i) { return (bits >> i) & 1; };
        for (int i = 0; i < 6; ++i)
            set(8, i, bit(i));
        set(8, 7, bit(6));
        set(8, 8, bit(7));
        set(7, 8, bit(8));
        for (int i = 9; i < 15; ++i)
            set(14 - i, 8, bit(i));
        for (int i = 0; i < 8; ++i)
            set(n - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; ++i)
            set(8, n - 15 + i, bit(i));
        set(8, n - 8, true);
    }

    // timing, finders and separators, alignment, version, reserved format
    void
    patterns(int v)
    {
        for (int i = 0; i < n; ++i)
        {
            set(6, i, i % 2 == 0);
            set(i, 6, i % 2 == 0);
        }

        auto const square = [&](int cx, int cy, int r, auto&& dark) {
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx)
                    if (int const x = cx + dx, y = cy + dy;
                        x >= 0 && x < n && y >= 0 && y < n)
                        set(x, y, dark(std::max(std::abs(dx), std::abs(dy))));
        };
        for (auto const& [x, y] : std::array<std::pair<int, int>, 3>{
                 {{3, 3}, {n - 4, 3}, {3, n - 4}}})
            square(x, y, 4, [](int d) { return d != 2 && d != 4; });

        if (v > 1)
        {
            int const k = v / 7 + 2;
            int const step = (v * 8 + k * 3 + 5) / (k * 4 - 4) * 2;
            std::vector<int> pos{6};
            for (int i = 1; i < k; ++i)
                pos.push_back(n - 7 - (k - 1 - i) * step);
            for (int i = 0; i < k; ++i)
                for (int j = 0; j < k; ++j)
                    if ((i || j) && (i || j != k - 1) && (i != k - 1 || j))
                        square(pos[i], pos[j], 2, [](int d) { return d != 1; });
        }

        format(0, 0);

        if (v >= 7)
        {
            int rem = v;
            for (int i = 0; i < 12; ++i)
                rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
            long const bits = long(v) << 12 | rem;
            for (int i = 0; i < 18; ++i)
            {
                bool const b = (bits >> i) & 1;
                set(n - 11 + i % 3, i / 3, b);
                set(i / 3, n - 11 + i % 3, b);
            }
        }
    }

    // the codewords, in the zigzag, around the function patterns
    void
    place(std::vector<std::uint8_t> const& cw)
    {
        std::size_t i = 0;
        for (int right = n - 1; right >= 1; right -= 2)
        {
            if (right == 6)
                right = 5;
            for (int vert = 0; vert < n; ++vert)
                for (int j = 0; j < 2; ++j)
                {
                    int const x = right - j;
                    int const y = ((right + 1) & 2) ? vert : n - 1 - vert;
                    if (!fixed[y * n + x] && i < cw.size() * 8)
                    {
                        dark[y * n + x] = (cw[i >> 3] >> (7 - (i & 7))) & 1;
                        ++i;
                    }
                }
        }
    }

    void
    applyMask(int m)
    {
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                bool const flip = [&] {
                    switch (m)
                    {
                        case 0:
                            return (x + y) % 2 == 0;
                        case 1:
                            return y % 2 == 0;
                        case 2:
                            return x % 3 == 0;
                        case 3:
                            return (x + y) % 3 == 0;
                        case 4:
                            return (x / 3 + y / 2) % 2 == 0;
                        case 5:
                            return x * y % 2 + x * y % 3 == 0;
                        case 6:
                            return (x * y % 2 + x * y % 3) % 2 == 0;
                        default:
                            return ((x + y) % 2 + x * y % 3) % 2 == 0;
                    }
                }();
                if (flip && !fixed[y * n + x])
                    dark[y * n + x] ^= 1;
            }
    }

    // ISO/IEC 18004 7.8.3: runs, 2x2 blocks, finder-like runs, dark balance
    long
    penalty() const
    {
        long p = 0;
        for (int pass = 0; pass < 2; ++pass)
            for (int a = 0; a < n; ++a)
            {
                auto const m = [&](int b) {
                    return pass ? at(a, b) : at(b, a);
                };
                auto const light = [&](int from, int to) {
                    for (int c = std::max(from, 0); c < std::min(to, n); ++c)
                        if (m(c))
                            return false;
                    return true;
                };
                for (int b = 0, run = 0; b < n; ++b)
                {
                    run = b && m(b) == m(b - 1) ? run + 1 : 1;
                    p += run == 5 ? 3 : run > 5;
                    if (b + 6 < n && m(b) && !m(b + 1) && m(b + 2) &&
                        m(b + 3) && m(b + 4) && !m(b + 5) && m(b + 6) &&
                        (light(b - 4, b) || light(b + 7, b + 11)))
                        p += 40;
                }
            }
        for (int y = 0; y + 1 < n; ++y)
            for (int x = 0; x + 1 < n; ++x)
                p += 3 *
                    (at(x, y) == at(x + 1, y) && at(x, y) == at(x, y + 1) &&
                     at(x, y) == at(x + 1, y + 1));
        long const total = n * n;
        long const dk = std::count(dark.begin(), dark.end(), 1);
        return p + std::abs(dk * 20 - total * 10) / total * 10;
    }
};

}  // namespace

std::vector<std::vector<bool>>
encode(std::string_view text, int mask)
{
    // (alphanumeric, text) segments: one for all of it, or a byte mode head
    // and the longest alphanumeric tail, whichever is shorter
    auto const isAN = [](char c) {
        return alnum.find(c) != std::string_view::npos;
    };
    using Segments = std::vector<std::pair<bool, std::string_view>>;
    auto const bitsAt = [](Segments const& segs, int v) {
        int const r = v < 10 ? 0 : v < 27 ? 1 : 2;
        std::size_t n = 0;
        for (auto const& [an, t] : segs)
            n += 4 +
                (an ? std::array{9, 11, 13}[r] : std::array{8, 16, 16}[r]) +
                (an ? t.size() / 2 * 11 + t.size() % 2 * 6 : t.size() * 8);
        return n;
    };
    std::size_t tail = text.size();
    while (tail && isAN(text[tail - 1]))
        --tail;
    Segments segs{{false, text}};
    if (tail < text.size())
    {
        Segments split;
        if (tail)
            split.emplace_back(false, text.substr(0, tail));
        split.emplace_back(true, text.substr(tail));
        if (bitsAt(split, 40) <= bitsAt(segs, 40))
            segs = std::move(split);
    }

    // the smallest version, then the strongest level it allows
    auto const fits = [&](int v, int l) {
        return bitsAt(segs, v) <= std::size_t(dataCodewords(v, l)) * 8;
    };
    int v = 1, l = 0;
    while (v <= 40 && !fits(v, 0))
        ++v;
    if (v > 40)
        return {};
    while (l < 3 && fits(v, l + 1))
        ++l;

    std::vector<bool> bits;
    auto const put = [&](std::uint32_t x, int n) {
        for (int i = n - 1; i >= 0; --i)
            bits.push_back((x >> i) & 1);
    };
    int const r = v < 10 ? 0 : v < 27 ? 1 : 2;
    for (auto const& [an, t] : segs)
    {
        put(an ? 2 : 4, 4);
        put(t.size(), an ? std::array{9, 11, 13}[r] : std::array{8, 16, 16}[r]);
        if (!an)
            for (unsigned char c : t)
                put(c, 8);
        else
        {
            auto const idx = [](char c) { return alnum.find(c); };
            for (std::size_t i = 0; i + 1 < t.size(); i += 2)
                put(45 * idx(t[i]) + idx(t[i + 1]), 11);
            if (t.size() % 2)
                put(idx(t.back()), 6);
        }
    }

    std::size_t const cap = dataCodewords(v, l) * 8;
    put(0, std::min<std::size_t>(4, cap - bits.size()));
    put(0, (8 - bits.size() % 8) % 8);

    std::vector<std::uint8_t> data;
    for (std::size_t i = 0; i < bits.size(); i += 8)
    {
        std::uint8_t b = 0;
        for (int j = 0; j < 8; ++j)
            b = b << 1 | bits[i + j];
        data.push_back(b);
    }
    for (std::uint8_t pad = 0xEC; data.size() < cap / 8; pad ^= 0xEC ^ 0x11)
        data.push_back(pad);

    // split into blocks, the short ones first, each with its ECC, then
    // interleave them column by column (short blocks skip the last data byte)
    int const nb = eccBlocks[l][v], el = eccLen[l][v];
    int const raw = rawModules(v) / 8, nShort = nb - raw % nb;
    int const shortLen = raw / nb;
    std::vector<std::vector<std::uint8_t>> blocks;
    for (int i = 0, k = 0; i < nb; ++i)
    {
        int const n = shortLen - el + (i >= nShort);
        std::vector<std::uint8_t> b(data.begin() + k, data.begin() + k + n);
        k += n;
        auto const e = reedSolomon(b, el);
        if (i < nShort)
            b.push_back(0);
        b.insert(b.end(), e.begin(), e.end());
        blocks.push_back(std::move(b));
    }
    std::vector<std::uint8_t> cw;
    for (int i = 0; i <= shortLen; ++i)
        for (int j = 0; j < nb; ++j)
            if (i != shortLen - el || j >= nShort)
                cw.push_back(blocks[j][i]);

    Symbol s(v);
    s.patterns(v);
    s.place(cw);

    if (mask < 0 || mask > 7)
    {
        long best = -1;
        for (int m = 0; m < 8; ++m)
        {
            Symbol t = s;
            t.format(l, m);
            t.applyMask(m);
            if (auto const p = t.penalty(); best < 0 || p < best)
                best = p, mask = m;
        }
    }
    s.format(l, mask);
    s.applyMask(mask);

    std::vector<std::vector<bool>> out(s.n, std::vector<bool>(s.n));
    for (int y = 0; y < s.n; ++y)
        for (int x = 0; x < s.n; ++x)
            out[y][x] = s.at(x, y);
    return out;
}

std::vector<std::string>
render(std::vector<std::vector<bool>> const& code, bool invert, int quiet)
{
    int const n = code.size(), w = n + 2 * quiet;
    auto const drawn = [&](int x, int y) {
        x -= quiet, y -= quiet;
        bool const dark = x >= 0 && y >= 0 && x < n && y < n && code[y][x];
        return y + quiet < w && dark == invert;
    };
    // nothing, lower half, upper half, full block
    static char const* const glyph[] = {
        " ", "\xE2\x96\x84", "\xE2\x96\x80", "\xE2\x96\x88"};
    std::vector<std::string> out;
    for (int y = 0; y < w; y += 2)
    {
        std::string line;
        for (int x = 0; x < w; ++x)
            line += glyph[drawn(x, y) * 2 + drawn(x, y + 1)];
        out.push_back(std::move(line));
    }
    return out;
}

}  // namespace qr
}  // namespace ripple
