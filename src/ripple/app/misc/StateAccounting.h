#ifndef RIPPLE_APP_MAIN_STATEACCOUNTING_H_INCLUDED
#define RIPPLE_APP_MAIN_STATEACCOUNTING_H_INCLUDED

#include <ripple/basics/chrono.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/json/json_value.h>
#include <ripple/protocol/jss.h>
#include <array>
#include <mutex>

namespace ripple {

// This is the primary interface into the "client" portion of the program.
// Code that wants to do normal operations on the network such as
// creating and monitoring accounts, creating transactions, and so on
// should use this interface. The RPC code will primarily be a light wrapper
// over this code.
//
// Eventually, it will check the node's operating mode (synched, unsynched,
// etectera) and defer to the correct means of processing. The current
// code assumes this node is synched (and will continue to do so until
// there's a functional network.
//

/** Specifies the mode under which the server believes it's operating.

    This has implications about how the server processes transactions and
    how it responds to requests (e.g. account balance request).

    @note Other code relies on the numerical values of these constants; do
          not change them without verifying each use and ensuring that it is
          not a breaking change.
*/
enum class OperatingMode {
    DISCONNECTED = 0,  //!< not ready to process requests
    CONNECTED = 1,     //!< convinced we are talking to the network
    SYNCING = 2,       //!< fallen slightly behind
    TRACKING = 3,      //!< convinced we agree with the network
    FULL = 4           //!< we have the ledger and can even validate
};

class StateAccounting
{
public:
    constexpr static std::array<Json::StaticString const, 5> const states_ = {
        {Json::StaticString("disconnected"),
         Json::StaticString("connected"),
         Json::StaticString("syncing"),
         Json::StaticString("tracking"),
         Json::StaticString("full")}};

    struct Counters
    {
        explicit Counters() = default;

        std::uint64_t transitions = 0;
        std::chrono::microseconds dur = std::chrono::microseconds(0);
    };

private:
    OperatingMode mode_ = OperatingMode::DISCONNECTED;
    std::array<Counters, 5> counters_;
    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point start_ =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point const processStart_ = start_;
    std::uint64_t initialSyncUs_{0};

public:
    explicit StateAccounting()
    {
        counters_[static_cast<std::size_t>(OperatingMode::DISCONNECTED)]
            .transitions = 1;
    }

    //! Record state transition. Update duration spent in previous state.
    void
    mode(OperatingMode om);

    //! Output state counters in JSON format.
    void
    json(Json::Value& obj);

    using CounterData = std::tuple<
        decltype(counters_),
        decltype(mode_),
        decltype(start_),
        decltype(initialSyncUs_)>;

    CounterData
    getCounterData()
    {
        return {counters_, mode_, start_, initialSyncUs_};
    }
};

}  // namespace ripple

#endif
