#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpl/beast/unit_test/suite.h>
#include <array>
#include <cstdint>

namespace ripple::test {

class HookGuestMemory_test : public beast::unit_test::suite
{
public:
    void
    run() override
    {
        testcase("Checked guest spans");
        std::array<std::uint8_t, 8> bytes{};
        hook::HookGuestMemory memory{bytes.data(), bytes.size()};

        BEAST_EXPECT(memory.valid());
        BEAST_EXPECT(memory.data() == bytes.data());
        BEAST_EXPECT(memory.contains(0, 0));
        BEAST_EXPECT(memory.contains(7, 1));
        BEAST_EXPECT(!memory.contains(8, 0));
        BEAST_EXPECT(!memory.contains(7, 2));
        BEAST_EXPECT(!memory.contains(UINT32_MAX, 2));

        auto writable = memory.write(2, 3);
        BEAST_EXPECT(!!writable);
        if (writable)
        {
            (*writable)[0] = 1;
            (*writable)[1] = 2;
            (*writable)[2] = 3;
        }
        auto readable = memory.read(2, 3);
        BEAST_EXPECT(!!readable);
        if (readable)
            BEAST_EXPECT((*readable)[0] == 1 && (*readable)[2] == 3);

        auto const before = bytes;
        auto rejected = memory.write(7, 2);
        BEAST_EXPECT(!rejected);
        BEAST_EXPECT(bytes == before);

        testcase("Legacy write compatibility");
        std::array<std::uint8_t, 2> source{9, 8};
        BEAST_EXPECT(memory.legacyWrite(
            6, std::span<std::uint8_t const>{source.data(), source.size()}));
        BEAST_EXPECT(bytes[6] == 9 && bytes[7] == 8);
        BEAST_EXPECT(memory.legacyWrite(
            8, std::span<std::uint8_t const>{source.data(), 0}));
        BEAST_EXPECT(memory.legacyWrite(8, std::span<std::uint8_t const>{}));
        BEAST_EXPECT(!memory.legacyWrite(
            7, std::span<std::uint8_t const>{source.data(), source.size()}));

        hook::HookGuestMemory writerFailure{
            bytes.data(),
            bytes.size(),
            nullptr,
            +[](void*, std::uint32_t, std::span<std::uint8_t const>) noexcept {
                return false;
            }};
        BEAST_EXPECT(!writerFailure.legacyWrite(
            bytes.size(), std::span<std::uint8_t const>{}));

        hook::HookGuestMemory empty{nullptr, 0};
        BEAST_EXPECT(!empty.valid());
        BEAST_EXPECT(!empty.legacyWrite(
            0, std::span<std::uint8_t const>{source.data(), 0}));
    }
};

BEAST_DEFINE_TESTSUITE(HookGuestMemory, app, ripple);

}  // namespace ripple::test
