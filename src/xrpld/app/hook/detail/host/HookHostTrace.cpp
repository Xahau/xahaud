#include <xrpld/app/hook/HookHostOperations/Trace.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/host/HookHostText.h>
#include <algorithm>
#include <array>
#include <string_view>

namespace hook::raw {
using enum hook_api::hook_return_code;

Result
trace(
    HookContext& hookCtx,
    HookGuestMemory memory,
    std::uint32_t messagePtr,
    std::uint32_t messageLength,
    std::uint32_t dataPtr,
    std::uint32_t dataLength,
    std::uint32_t asHex,
    beast::Journal const& journal)
{
    auto message = memory.read(messagePtr, messageLength);
    auto data = memory.read(dataPtr, dataLength);
    if (!message || !data)
        return OUT_OF_BOUNDS;
    if (!journal.trace())
        return std::uint64_t{0};

    message = message->first(std::min<std::size_t>(message->size(), 128));
    data = data->first(std::min<std::size_t>(data->size(), 1023));

    std::array<std::uint8_t, 2200> output{};
    std::size_t length = 0;
    if (!message->empty())
    {
        std::copy(message->begin(), message->end(), output.begin());
        length = message->size();
        if (output[length - 1] == 0)
            --length;
        output[length++] = ':';
        output[length++] = ' ';
    }

    if (!data->empty())
    {
        if (asHex)
        {
            static constexpr char digits[] = "0123456789ABCDEF";
            for (auto byte : *data)
            {
                output[length++] = digits[byte >> 4];
                output[length++] = digits[byte & 0x0F];
            }
        }
        else if (detail::isUTF16LE(*data))
        {
            for (std::size_t index = 0; index < data->size(); index += 2)
                output[length++] = (*data)[index];
        }
        else
        {
            std::copy(data->begin(), data->end(), output.begin() + length);
            length += data->size();
        }
    }

    if (length != 0)
        journal.trace() << "HookTrace[" << hookCtx.result.account << '-'
                        << hookCtx.result.otxnAccount << "]: "
                        << std::string_view{
                               reinterpret_cast<char const*>(output.data()),
                               length};
    return std::uint64_t{0};
}

}  // namespace hook::raw
