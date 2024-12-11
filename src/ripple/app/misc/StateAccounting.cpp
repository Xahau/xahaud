#include <ripple/app/misc/StateAccounting.h>

namespace ripple {

void
StateAccounting::mode(OperatingMode om)
{
    std::lock_guard lock(mutex_);
    auto now = std::chrono::steady_clock::now();

    ++counters_[static_cast<std::size_t>(om)].transitions;
    if (om == OperatingMode::FULL &&
        counters_[static_cast<std::size_t>(om)].transitions == 1)
    {
        initialSyncUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
                             now - processStart_)
                             .count();
    }
    counters_[static_cast<std::size_t>(mode_)].dur +=
        std::chrono::duration_cast<std::chrono::microseconds>(now - start_);

    mode_ = om;
    start_ = now;
}

void
StateAccounting::json(Json::Value& obj)
{
    auto [counters, mode, start, initialSync] = getCounterData();
    auto const current = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    counters[static_cast<std::size_t>(mode)].dur += current;

    obj[jss::state_accounting] = Json::objectValue;
    for (std::size_t i = static_cast<std::size_t>(OperatingMode::DISCONNECTED);
         i <= static_cast<std::size_t>(OperatingMode::FULL);
         ++i)
    {
        obj[jss::state_accounting][states_[i]] = Json::objectValue;
        auto& state = obj[jss::state_accounting][states_[i]];
        state[jss::transitions] = std::to_string(counters[i].transitions);
        state[jss::duration_us] = std::to_string(counters[i].dur.count());
    }
    obj[jss::server_state_duration_us] = std::to_string(current.count());
    if (initialSync)
        obj[jss::initial_sync_duration_us] = std::to_string(initialSync);
}

}  // namespace ripple
