#ifndef XRPLD_APP_HOOK_HOOKGUESTMEMORY_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKGUESTMEMORY_H_INCLUDED

#include <xrpl/basics/Expected.h>
#include <xrpl/hook/Enum.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace hook {

/** Non-owning view of one QuickJS provider invocation's linear memory.

    The Wasmtime adapter constructs this view from the current callback's
    memory. All pointer arithmetic is widened before bounds checks; the class
    never truncates or grows guest buffers.
 */
class HookGuestMemory
{
public:
    using Error = hook_api::hook_return_code;

    HookGuestMemory(std::uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size)
    {
    }

    [[nodiscard]] std::uint8_t*
    data() const noexcept
    {
        return data_;
    }

    [[nodiscard]] std::size_t
    size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] bool
    valid() const noexcept
    {
        return data_ != nullptr && size_ != 0;
    }

    [[nodiscard]] bool
    contains(std::uint32_t offset, std::uint32_t length) const noexcept
    {
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
    compatibilityCopy(std::uint32_t offset, std::span<std::uint8_t const> bytes)
        const noexcept
    {
        auto const start = static_cast<std::uint64_t>(offset);
        auto const count = static_cast<std::uint64_t>(bytes.size());
        auto const extent = static_cast<std::uint64_t>(size_);
        if (data_ == nullptr || start > extent || count > extent - start)
            return false;

        if (bytes.empty())
            return true;

        std::memcpy(data_ + offset, bytes.data(), bytes.size());
        return true;
    }

private:
    std::uint8_t* data_;
    std::size_t size_;
};

}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKGUESTMEMORY_H_INCLUDED
