#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/detail/QuickJSProviderProfile.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSProviderSession.h>
#include <xrpl/basics/scope.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <wasmtime.h>

namespace hook {
namespace {

std::optional<std::string>
decodeModuleValidationResult(
    std::int32_t word,
    QuickJSModuleValidation& validation)
{
    validation = {};
    if (word <= 0)
        return "QuickJS provider returned an invalid module-validation "
               "success word";

    auto const value = static_cast<std::uint32_t>(word);
    if ((value & artifact::generated::moduleValidationReservedMask) != 0)
        return "QuickJS provider module-validation result has reserved bits";

    auto const version =
        (value & artifact::generated::moduleValidationVersionMask) >>
        artifact::generated::moduleValidationVersionShift;
    if (version != artifact::generated::moduleValidationLayoutVersion)
        return "QuickJS provider module-validation result has an unsupported "
               "layout version";

    auto const entries = value & artifact::generated::moduleValidationEntryMask;
    if ((entries & artifact::generated::moduleValidationMainBit) == 0)
        return "QuickJS provider module-validation result has no callable "
               "main entry";
    auto const mainOnly = artifact::generated::moduleValidationMainBit;
    auto const mainAndCallback =
        mainOnly | artifact::generated::moduleValidationCallbackBit;
    if (entries != mainOnly && entries != mainAndCallback)
        return "QuickJS provider module-validation result has invalid entry "
               "bits";

    auto const profileCode =
        (value & artifact::generated::moduleValidationProfileMask) >>
        artifact::generated::moduleValidationProfileShift;
    if (profileCode == artifact::generated::xflArithmeticProfileNone)
    {
        validation.xflArithmeticProfile = artifact::XFLArithmeticProfile::none;
    }
    else if (
        profileCode == artifact::generated::xflArithmeticProfileXahauFloatV1)
    {
        validation.xflArithmeticProfile =
            artifact::XFLArithmeticProfile::xahauFloatV1;
    }
    else if (
        profileCode == artifact::generated::xflArithmeticProfileNearestEvenV1)
    {
        validation.xflArithmeticProfile =
            artifact::XFLArithmeticProfile::nearestEvenV1;
    }
    else
        return "QuickJS provider module-validation result has an unknown "
               "XFL arithmetic profile";
    validation.hasCallback =
        (entries & artifact::generated::moduleValidationCallbackBit) != 0;
    return std::nullopt;
}

std::optional<std::string>
validate(
    QuickJSRuntimeHandle const& runtime,
    std::span<std::uint8_t const> bytecode,
    QuickJSModuleValidation& validation,
    std::uint64_t* invocationFuelConsumed)
{
    validation = {};
    if (!runtime)
        return "QuickJS runtime profile is not registered";
    if (bytecode.empty() ||
        bytecode.size() >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        return "QuickJS Hook bytecode is empty or too large";

    quickjs::ProviderStage stage = quickjs::ProviderStage::none;
    std::string detail;
    auto const failure = [&]() {
        return detail.empty() ? std::string{quickjs::providerStageName(stage)}
                              : detail;
    };
    auto session =
        quickjs::ProviderSession::create(*runtime, nullptr, stage, detail);
    if (!session)
        return failure();
    if (!session->initialize(stage, detail) ||
        !session->resetInvocationFuel(stage, detail))
        return failure();

    ripple::scope_exit recordFuel{[&] {
        if (invocationFuelConsumed)
            if (auto const consumed = session->invocationFuelConsumed())
                *invocationFuelConsumed = *consumed;
    }};

    auto const pointer = session->allocateAndCopy(bytecode, stage, detail);
    if (!pointer)
        return failure();

    wasmtime_val_t arguments[2] = {
        {.kind = WASMTIME_I32,
         .of = {.i32 = static_cast<std::int32_t>(*pointer)}},
        {.kind = WASMTIME_I32,
         .of = {.i32 = static_cast<std::int32_t>(bytecode.size())}}};
    wasmtime_val_t result[1];
    if (!session->callExport(
            "qjs_validate_hook_module", arguments, 2, result, 1, detail))
        return detail;
    if (result[0].kind != WASMTIME_I32)
        return "QuickJS provider export returned a non-i32";

    auto const word = result[0].of.i32;
    if (word == artifact::generated::moduleValidationFailureSentinel)
    {
        detail = session->readDiagnostic();
        return detail.empty() ? "QuickJS Hook bytecode validation failed"
                              : detail;
    }
    return decodeModuleValidationResult(word, validation);
}

}  // namespace

std::optional<std::string>
validateQuickJSBytecode(
    QuickJSRuntimeHandle const& runtime,
    std::span<std::uint8_t const> bytecode,
    QuickJSModuleValidation& validation)
{
    return validate(runtime, bytecode, validation, nullptr);
}

#ifdef ENABLE_TESTS
QuickJSValidationForTests
validateQuickJSBytecodeForTests(
    QuickJSRuntimeHandle const& runtime,
    std::span<std::uint8_t const> bytecode)
{
    QuickJSValidationForTests result;
    result.error =
        validate(runtime, bytecode, result, &result.invocationFuelConsumed);
    return result;
}

std::optional<std::string>
decodeQuickJSModuleValidationForTests(
    std::int32_t word,
    QuickJSModuleValidation& validation)
{
    return decodeModuleValidationResult(word, validation);
}

QuickJSSessionCostForTests
measureQuickJSSessionCostForTests(
    QuickJSRuntimeHandle const& runtime,
    std::span<std::uint8_t const> bytecode,
    std::size_t iterations)
{
    using clock = std::chrono::steady_clock;
    auto const nanosSince = [](clock::time_point const& start) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock::now() - start)
                .count());
    };
    auto const keepMin = [](std::uint64_t& slot, std::uint64_t sample) {
        slot = slot == 0 ? sample : std::min(slot, sample);
    };

    QuickJSSessionCostForTests cost;
    if (!runtime)
    {
        cost.error = "QuickJS runtime profile is not registered";
        return cost;
    }
    for (std::size_t iteration = 0; iteration < iterations; ++iteration)
    {
        quickjs::ProviderStage stage = quickjs::ProviderStage::none;
        std::string detail;
        auto const failure = [&]() {
            return detail.empty()
                ? std::string{quickjs::providerStageName(stage)}
                : detail;
        };

        auto const createStart = clock::now();
        auto session =
            quickjs::ProviderSession::create(*runtime, nullptr, stage, detail);
        auto const createNanos = nanosSince(createStart);
        if (!session)
        {
            cost.error = failure();
            return cost;
        }

        auto const initializeStart = clock::now();
        if (!session->initialize(stage, detail))
        {
            cost.error = failure();
            return cost;
        }
        auto const initializeNanos = nanosSince(initializeStart);
        if (auto const consumed = session->initializationFuelConsumed())
            cost.initializationFuelConsumed = *consumed;
        if (!session->resetInvocationFuel(stage, detail))
        {
            cost.error = failure();
            return cost;
        }

        auto const validateStart = clock::now();
        auto const pointer = session->allocateAndCopy(bytecode, stage, detail);
        if (!pointer)
        {
            cost.error = failure();
            return cost;
        }
        wasmtime_val_t arguments[2] = {
            {.kind = WASMTIME_I32,
             .of = {.i32 = static_cast<std::int32_t>(*pointer)}},
            {.kind = WASMTIME_I32,
             .of = {.i32 = static_cast<std::int32_t>(bytecode.size())}}};
        wasmtime_val_t result[1];
        if (!session->callExport(
                "qjs_validate_hook_module", arguments, 2, result, 1, detail))
        {
            cost.error = detail;
            return cost;
        }
        if (result[0].kind != WASMTIME_I32)
        {
            cost.error = "QuickJS provider export returned a non-i32";
            return cost;
        }
        auto const validateNanos = nanosSince(validateStart);
        auto const word = result[0].of.i32;
        if (word == artifact::generated::moduleValidationFailureSentinel)
        {
            // A cost sample from a session that rejected the bytecode is not
            // the cost being measured.
            detail = session->readDiagnostic();
            cost.error = detail.empty()
                ? "QuickJS Hook bytecode validation failed during measurement"
                : detail;
            return cost;
        }
        QuickJSModuleValidation validation;
        if (auto const decodeError =
                decodeModuleValidationResult(word, validation))
        {
            cost.error = *decodeError;
            return cost;
        }
        if (auto const consumed = session->invocationFuelConsumed())
            cost.invocationFuelConsumed = *consumed;

        ++cost.iterations;
        cost.createNanosTotal += createNanos;
        keepMin(cost.createNanosMin, createNanos);
        cost.initializeNanosTotal += initializeNanos;
        keepMin(cost.initializeNanosMin, initializeNanos);
        cost.validateNanosTotal += validateNanos;
        keepMin(cost.validateNanosMin, validateNanos);
    }
    return cost;
}
#endif

}  // namespace hook
