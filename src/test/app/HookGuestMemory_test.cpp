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
        BEAST_EXPECT(!memory.contains(9, 0));
        BEAST_EXPECT(!memory.contains(7, 2));
        BEAST_EXPECT(!memory.contains(UINT32_MAX, 2));
        BEAST_EXPECT(!!memory.read(7, 1));
        BEAST_EXPECT(!memory.read(8, 0));
        BEAST_EXPECT(!!memory.write(7, 1));
        BEAST_EXPECT(!memory.write(8, 0));

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

        testcase("Compatibility copy");
        std::array<std::uint8_t, 2> source{9, 8};
        BEAST_EXPECT(memory.compatibilityCopy(
            6, std::span<std::uint8_t const>{source.data(), source.size()}));
        BEAST_EXPECT(bytes[6] == 9 && bytes[7] == 8);
        BEAST_EXPECT(memory.compatibilityCopy(
            8, std::span<std::uint8_t const>{source.data(), 0}));
        BEAST_EXPECT(
            memory.compatibilityCopy(8, std::span<std::uint8_t const>{}));
        BEAST_EXPECT(!memory.compatibilityCopy(
            9, std::span<std::uint8_t const>{source.data(), 0}));
        auto const copied = bytes;
        BEAST_EXPECT(!memory.compatibilityCopy(
            7, std::span<std::uint8_t const>{source.data(), source.size()}));
        BEAST_EXPECT(!memory.compatibilityCopy(
            UINT32_MAX,
            std::span<std::uint8_t const>{source.data(), source.size()}));
        BEAST_EXPECT(bytes == copied);

        hook::HookGuestMemory empty{nullptr, 0};
        BEAST_EXPECT(!empty.valid());
        BEAST_EXPECT(!empty.compatibilityCopy(
            0, std::span<std::uint8_t const>{source.data(), 0}));

        testcase("Compatibility writer");
        struct WriterState
        {
            bool called = false;
            bool succeed = false;
        } writerState;
        auto const writer = [](void* context,
                               std::uint32_t,
                               std::span<std::uint8_t const>) noexcept {
            auto& state = *static_cast<WriterState*>(context);
            state.called = true;
            return state.succeed;
        };
        hook::HookGuestMemory writerMemory{
            bytes.data(), bytes.size(), &writerState, writer};
        BEAST_EXPECT(!writerMemory.compatibilityCopy(
            0, std::span<std::uint8_t const>{source.data(), source.size()}));
        BEAST_EXPECT(writerState.called);
        writerState.called = false;
        writerState.succeed = true;
        BEAST_EXPECT(
            writerMemory.compatibilityCopy(8, std::span<std::uint8_t const>{}));
        BEAST_EXPECT(writerState.called);

        testcase("Deferred memory resolution");
        struct ResolverState
        {
            std::uint8_t* data;
            std::size_t size;
            std::size_t calls = 0;
        } resolverState{bytes.data(), bytes.size()};
        auto const resolver =
            [](void* context,
               std::uint8_t*& data,
               std::size_t& size,
               void*&,
               hook::HookGuestMemory::CompatibilityWriter&) noexcept {
                auto& state = *static_cast<ResolverState*>(context);
                ++state.calls;
                data = state.data;
                size = state.size;
            };
        hook::HookGuestMemory deferred{
            &resolverState,
            resolver,
            hook::HookGuestMemory::WriteContract::legacyActualSize};
        BEAST_EXPECT(resolverState.calls == 0);
        BEAST_EXPECT(
            deferred.writeContract() ==
            hook::HookGuestMemory::WriteContract::legacyActualSize);
        BEAST_EXPECT(resolverState.calls == 0);
        deferred.resolve();
        BEAST_EXPECT(resolverState.calls == 1);
        BEAST_EXPECT(deferred.valid());
        BEAST_EXPECT(deferred.data() == bytes.data());
        BEAST_EXPECT(deferred.size() == bytes.size());
        BEAST_EXPECT(resolverState.calls == 1);
    }
};

BEAST_DEFINE_TESTSUITE(HookGuestMemory, app, ripple);

}  // namespace ripple::test
