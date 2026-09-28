// Compile-only check: shipped SDK declarations must match the runtime ABI,
// not just the prototype comments used by generate_extern.sh.
#include <cstdint>
#include <type_traits>

// Use the dependency-free guard-checker view of the host enum definitions.
#define GUARD_CHECKER_BUILD 1
#include <xrpl/hook/Enum.h>

constexpr auto hostAnyStrongVeto = hook_api::ENTROPY_ALLOW_ANY_STRONG_VETO;
constexpr auto hostSameAccountStrongVeto =
    hook_api::ENTROPY_ALLOW_SAME_ACCOUNT_STRONG_VETO;
constexpr auto hostLaterStrongHook =
    static_cast<int64_t>(hook_api::hook_return_code::LATER_STRONG_HOOK);

#include "../hookapi.h"

static_assert(ENTROPY_ALLOW_ANY_STRONG_VETO == hostAnyStrongVeto);
static_assert(
    ENTROPY_ALLOW_SAME_ACCOUNT_STRONG_VETO == hostSameAccountStrongVeto);
static_assert(LATER_STRONG_HOOK == hostLaterStrongHook);

#define HOOK_API_DEFINITION(                                                   \
    RETURN_TYPE, FUNCTION_NAME, PARAMS_TUPLE, AMENDMENT)                       \
    static_assert(                                                             \
        std::is_same_v<decltype(&FUNCTION_NAME), RETURN_TYPE(*) PARAMS_TUPLE>, \
        "Hook SDK/runtime ABI mismatch: " #FUNCTION_NAME);
#include <xrpl/hook/hook_api.macro>
#undef HOOK_API_DEFINITION
