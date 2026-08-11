#ifndef XRPLD_APP_HOOK_HOOKGUESTMEMORY_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKGUESTMEMORY_H_INCLUDED

#include <xrpl/basics/Expected.h>
#include <xrpl/hook/Enum.h>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hook {

/** Non-owning, engine-neutral view of one Hook guest's linear memory.

    WasmEdge and Wasmtime adapters construct this view from the memory owned by
    the current invocation.  All pointer arithmetic is widened before bounds
    checks; the class never truncates or grows guest buffers.
 */
class HookGuestMemory
{
public:
    using Error = hook_api::hook_return_code;

    HookGuestMemory(std::uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size)
    {
    }

    [[nodiscard]] std::size_t
    size() const noexcept
    {
        return size_;
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

private:
    std::uint8_t* data_;
    std::size_t size_;
};

}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKGUESTMEMORY_H_INCLUDED
