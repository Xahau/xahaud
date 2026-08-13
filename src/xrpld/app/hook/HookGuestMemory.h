#ifndef XRPLD_APP_HOOK_HOOKGUESTMEMORY_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKGUESTMEMORY_H_INCLUDED

#include <xrpl/basics/Expected.h>
#include <xrpl/hook/Enum.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace hook {

/** Non-owning view of one Hook invocation's linear memory.

    Engine adapters construct this view from the current callback's memory.
    All pointer arithmetic is widened before bounds checks; the class never
    truncates or grows guest buffers.
 */
class HookGuestMemory
{
public:
    using Error = hook_api::hook_return_code;
    enum class WriteContract : std::uint8_t { fixedBuffer, legacyActualSize };
    using CompatibilityWriter = bool (*)(
        void* context,
        std::uint32_t offset,
        std::span<std::uint8_t const> bytes) noexcept;
    using Resolver = void (*)(
        void* context,
        std::uint8_t*& data,
        std::size_t& size,
        void*& writerContext,
        CompatibilityWriter& writer) noexcept;

    HookGuestMemory(
        std::uint8_t* data,
        std::size_t size,
        void* writerContext = nullptr,
        CompatibilityWriter writer = nullptr,
        WriteContract writeContract = WriteContract::fixedBuffer) noexcept
        : data_(data)
        , size_(size)
        , writerContext_(writerContext)
        , writer_(writer)
        , writeContract_(writeContract)
    {
    }

    HookGuestMemory(
        void* resolverContext,
        Resolver resolver,
        WriteContract writeContract) noexcept
        : resolverContext_(resolverContext)
        , resolver_(resolver)
        , resolved_(false)
        , writeContract_(writeContract)
    {
    }

    void
    resolve() const noexcept
    {
        if (resolved_)
            return;
        resolved_ = true;
        if (resolver_)
            resolver_(resolverContext_, data_, size_, writerContext_, writer_);
    }

    [[nodiscard]] std::uint8_t*
    data() const noexcept
    {
        resolve();
        return data_;
    }

    [[nodiscard]] std::size_t
    size() const noexcept
    {
        resolve();
        return size_;
    }

    [[nodiscard]] bool
    valid() const noexcept
    {
        resolve();
        return data_ != nullptr && size_ != 0;
    }

    [[nodiscard]] WriteContract
    writeContract() const noexcept
    {
        return writeContract_;
    }

    [[nodiscard]] bool
    contains(std::uint32_t offset, std::uint32_t length) const noexcept
    {
        resolve();
        auto const start = static_cast<std::uint64_t>(offset);
        auto const count = static_cast<std::uint64_t>(length);
        auto const extent = static_cast<std::uint64_t>(size_);
        return data_ != nullptr && start < extent && count <= extent - start;
    }

    [[nodiscard]] ripple::Expected<std::span<std::uint8_t const>, Error>
    read(std::uint32_t offset, std::uint32_t length) const noexcept
    {
        if (!contains(offset, length))
            return ripple::Unexpected(Error::OUT_OF_BOUNDS);
        return std::span<std::uint8_t const>{data_ + offset, length};
    }

    [[nodiscard]] ripple::Expected<std::span<std::uint8_t>, Error>
    write(std::uint32_t offset, std::uint32_t length) const noexcept
    {
        if (!contains(offset, length))
            return ripple::Unexpected(Error::OUT_OF_BOUNDS);
        return std::span<std::uint8_t>{data_ + offset, length};
    }

    /** Copy bytes with the raw-operation compatibility boundary.

        Unlike contains(), a zero-byte write at exactly the end of memory is
        permitted.
     */
    [[nodiscard]] bool
    canCompatibilityCopy(std::uint32_t offset, std::size_t count) const noexcept
    {
        resolve();
        auto const start = static_cast<std::uint64_t>(offset);
        auto const length = static_cast<std::uint64_t>(count);
        auto const extent = static_cast<std::uint64_t>(size_);
        return data_ != nullptr && start <= extent && length <= extent - start;
    }

    [[nodiscard]] bool
    compatibilityCopy(std::uint32_t offset, std::span<std::uint8_t const> bytes)
        const noexcept
    {
        if (!canCompatibilityCopy(offset, bytes.size()))
            return false;

        if (writer_)
            return writer_(writerContext_, offset, bytes);

        if (bytes.empty())
            return true;

        std::memcpy(data_ + offset, bytes.data(), bytes.size());
        return true;
    }

private:
    mutable std::uint8_t* data_ = nullptr;
    mutable std::size_t size_ = 0;
    mutable void* writerContext_ = nullptr;
    mutable CompatibilityWriter writer_ = nullptr;
    void* resolverContext_ = nullptr;
    Resolver resolver_ = nullptr;
    mutable bool resolved_ = true;
    WriteContract writeContract_;
};

}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKGUESTMEMORY_H_INCLUDED
