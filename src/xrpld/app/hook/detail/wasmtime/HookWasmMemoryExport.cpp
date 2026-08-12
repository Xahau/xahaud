#include <xrpld/app/hook/detail/wasmtime/HookWasmMemoryExport.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace hook::wasmtime {
namespace {

class Reader
{
public:
    Reader(
        std::span<std::uint8_t const> bytes,
        std::size_t begin,
        std::size_t end)
        : bytes_(bytes), position_(begin), end_(end)
    {
    }

    std::optional<std::uint8_t>
    byte() noexcept
    {
        if (position_ >= end_)
            return std::nullopt;
        return bytes_[position_++];
    }

    std::optional<std::uint32_t>
    u32() noexcept
    {
        std::uint32_t value = 0;
        for (std::uint32_t shift = 0; shift < 35; shift += 7)
        {
            auto const next = byte();
            if (!next)
                return std::nullopt;
            if (shift == 28 && (*next & 0xF0U) != 0)
                return std::nullopt;
            value |= static_cast<std::uint32_t>(*next & 0x7FU) << shift;
            if ((*next & 0x80U) == 0)
                return value;
        }
        return std::nullopt;
    }

    std::optional<std::uint64_t>
    u64() noexcept
    {
        std::uint64_t value = 0;
        for (std::uint32_t shift = 0; shift < 70; shift += 7)
        {
            auto const next = byte();
            if (!next)
                return std::nullopt;
            if (shift == 63 && (*next & 0xFEU) != 0)
                return std::nullopt;
            value |= static_cast<std::uint64_t>(*next & 0x7FU) << shift;
            if ((*next & 0x80U) == 0)
                return value;
        }
        return std::nullopt;
    }

    std::optional<std::span<std::uint8_t const>>
    name() noexcept
    {
        auto const length = u32();
        if (!length || *length > end_ - position_)
            return std::nullopt;
        auto result = bytes_.subspan(position_, *length);
        position_ += *length;
        return result;
    }

    bool
    skipLimits() noexcept
    {
        auto const flags = u32();
        if (!flags || (*flags & ~0x0FU) != 0)
            return false;
        auto const memory64 = (*flags & 0x04U) != 0;
        if (memory64 ? !u64() : !u32())
            return false;
        if ((*flags & 0x01U) != 0 && (memory64 ? !u64() : !u32()))
            return false;
        if ((*flags & 0x08U) != 0 && !u32())
            return false;
        return true;
    }

    std::size_t
    position() const noexcept
    {
        return position_;
    }

    bool
    done() const noexcept
    {
        return position_ == end_;
    }

private:
    std::span<std::uint8_t const> bytes_;
    std::size_t position_;
    std::size_t end_;
};

void
appendU32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    do
    {
        auto byte = static_cast<std::uint8_t>(value & 0x7FU);
        value >>= 7;
        if (value != 0)
            byte |= 0x80U;
        output.push_back(byte);
    } while (value != 0);
}

bool
sameName(std::span<std::uint8_t const> name, char const* expected) noexcept
{
    auto const length = std::strlen(expected);
    return name.size() == length &&
        std::memcmp(name.data(), expected, length) == 0;
}

bool
sectionFollowsExports(std::uint8_t id) noexcept
{
    return id == 8 || id == 9 || id == 10 || id == 11 || id == 12 || id == 13;
}

bool
readImports(
    std::span<std::uint8_t const> wasm,
    std::size_t begin,
    std::size_t end,
    std::uint32_t& memoryCount) noexcept
{
    Reader reader{wasm, begin, end};
    auto const count = reader.u32();
    if (!count)
        return false;
    for (std::uint32_t index = 0; index < *count; ++index)
    {
        if (!reader.name() || !reader.name())
            return false;
        auto const kind = reader.byte();
        if (!kind)
            return false;
        switch (*kind)
        {
            case 0:  // function type index
                if (!reader.u32())
                    return false;
                break;
            case 1:  // table type
                if (!reader.byte() || !reader.skipLimits())
                    return false;
                break;
            case 2:  // memory type
                if (!reader.skipLimits())
                    return false;
                ++memoryCount;
                break;
            case 3:  // global type
                if (!reader.byte() || !reader.byte())
                    return false;
                break;
            case 4:  // tag attribute + function type index
                if (!reader.byte() || !reader.u32())
                    return false;
                break;
            default:
                return false;
        }
    }
    return reader.done();
}

}  // namespace

MemoryExportResult
ensureHookMemoryExport(std::span<std::uint8_t const> wasm)
{
    static constexpr std::array<std::uint8_t, 8> header{
        0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U};
    if (wasm.size() < header.size() ||
        !std::equal(header.begin(), header.end(), wasm.begin()))
        return {{}, "invalid WebAssembly header"};

    std::uint32_t memoryCount = 0;
    std::optional<std::size_t> exportSectionStart;
    std::size_t exportPayloadEnd = 0;
    std::size_t exportEntriesStart = 0;
    std::uint32_t exportCount = 0;
    std::size_t insertionOffset = wasm.size();
    bool hasMemoryExport = false;

    std::size_t position = header.size();
    while (position < wasm.size())
    {
        auto const sectionStart = position;
        auto const id = wasm[position++];
        Reader lengthReader{wasm, position, wasm.size()};
        auto const payloadLength = lengthReader.u32();
        if (!payloadLength)
            return {{}, "invalid WebAssembly section length"};
        auto const payloadStart = lengthReader.position();
        if (*payloadLength > wasm.size() - payloadStart)
            return {{}, "truncated WebAssembly section"};
        auto const payloadEnd = payloadStart + *payloadLength;

        if (id != 0 && sectionFollowsExports(id) &&
            insertionOffset == wasm.size())
            insertionOffset = sectionStart;

        if (id == 2)
        {
            if (!readImports(wasm, payloadStart, payloadEnd, memoryCount))
                return {
                    {}, "unsupported or invalid WebAssembly import section"};
        }
        else if (id == 5)
        {
            Reader reader{wasm, payloadStart, payloadEnd};
            auto const count = reader.u32();
            if (!count)
                return {{}, "invalid WebAssembly memory section"};
            if (*count >
                std::numeric_limits<std::uint32_t>::max() - memoryCount)
                return {{}, "WebAssembly memory count overflow"};
            memoryCount += *count;
        }
        else if (id == 7)
        {
            if (exportSectionStart)
                return {{}, "duplicate WebAssembly export section"};
            exportSectionStart = sectionStart;
            exportPayloadEnd = payloadEnd;
            Reader reader{wasm, payloadStart, payloadEnd};
            auto const count = reader.u32();
            if (!count)
                return {{}, "invalid WebAssembly export section"};
            exportCount = *count;
            exportEntriesStart = reader.position();
            for (std::uint32_t index = 0; index < exportCount; ++index)
            {
                auto const name = reader.name();
                auto const kind = reader.byte();
                auto const itemIndex = reader.u32();
                if (!name || !kind || !itemIndex)
                    return {{}, "invalid WebAssembly export entry"};
                if (sameName(*name, "memory"))
                {
                    if (*kind != 2 || *itemIndex != 0)
                        return {
                            {},
                            "the `memory` export does not name memory index 0"};
                    hasMemoryExport = true;
                }
            }
            if (!reader.done())
                return {{}, "trailing bytes in WebAssembly export section"};
        }
        position = payloadEnd;
    }

    if (memoryCount == 0 || hasMemoryExport)
        return {{wasm.begin(), wasm.end()}, {}};
    if (memoryCount != 1)
        return {{}, "C Hook Wasmtime execution requires exactly one memory"};

    std::vector<std::uint8_t> entry;
    appendU32(entry, 6);
    entry.insert(entry.end(), {'m', 'e', 'm', 'o', 'r', 'y'});
    entry.push_back(2);
    appendU32(entry, 0);

    if (!exportSectionStart)
    {
        std::vector<std::uint8_t> section;
        section.push_back(7);
        std::vector<std::uint8_t> payload;
        appendU32(payload, 1);
        payload.insert(payload.end(), entry.begin(), entry.end());
        appendU32(section, static_cast<std::uint32_t>(payload.size()));
        section.insert(section.end(), payload.begin(), payload.end());

        std::vector<std::uint8_t> result;
        result.reserve(wasm.size() + section.size());
        result.insert(
            result.end(), wasm.begin(), wasm.begin() + insertionOffset);
        result.insert(result.end(), section.begin(), section.end());
        result.insert(result.end(), wasm.begin() + insertionOffset, wasm.end());
        return {std::move(result), {}};
    }

    if (exportCount == std::numeric_limits<std::uint32_t>::max())
        return {{}, "WebAssembly export count overflow"};
    std::vector<std::uint8_t> payload;
    appendU32(payload, exportCount + 1);
    payload.insert(
        payload.end(),
        wasm.begin() + exportEntriesStart,
        wasm.begin() + exportPayloadEnd);
    payload.insert(payload.end(), entry.begin(), entry.end());
    if (payload.size() > std::numeric_limits<std::uint32_t>::max())
        return {{}, "WebAssembly export section is too large"};

    std::vector<std::uint8_t> result;
    result.reserve(wasm.size() + entry.size() + 5);
    result.insert(
        result.end(), wasm.begin(), wasm.begin() + *exportSectionStart);
    result.push_back(7);
    appendU32(result, static_cast<std::uint32_t>(payload.size()));
    result.insert(result.end(), payload.begin(), payload.end());
    result.insert(result.end(), wasm.begin() + exportPayloadEnd, wasm.end());
    return {std::move(result), {}};
}

}  // namespace hook::wasmtime
