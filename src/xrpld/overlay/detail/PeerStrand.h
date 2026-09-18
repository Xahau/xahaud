#ifndef RIPPLE_OVERLAY_PEERSTRAND_H_INCLUDED
#define RIPPLE_OVERLAY_PEERSTRAND_H_INCLUDED

#include <xrpld/core/Config.h>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/strand.hpp>
#include <utility>

namespace ripple {

class InlineExecutor
{
    boost::asio::execution_context* context_;

public:
    explicit InlineExecutor(boost::asio::execution_context& ctx) noexcept
        : context_(&ctx)
    {
    }

    [[nodiscard]] boost::asio::execution_context&
    context() const noexcept
    {
        return *context_;
    }

    void
    on_work_started() const noexcept
    {
    }

    void
    on_work_finished() const noexcept
    {
    }

    template <class F, class A>
    void
    dispatch(F&& f, A const&) const
    {
        std::forward<F>(f)();
    }

    template <class F, class A>
    void
    post(F&& f, A const&) const
    {
        std::forward<F>(f)();
    }

    template <class F, class A>
    void
    defer(F&& f, A const&) const
    {
        std::forward<F>(f)();
    }

    friend bool
    operator==(InlineExecutor const& a, InlineExecutor const& b) noexcept
    {
        return a.context_ == b.context_;
    }

    friend bool
    operator!=(InlineExecutor const& a, InlineExecutor const& b) noexcept
    {
        return a.context_ != b.context_;
    }
};

using PeerStrand = boost::asio::strand<boost::asio::executor>;

[[nodiscard]] inline PeerStrand
makePeerStrand(
    Config const& config,
    boost::asio::any_io_executor const& transportExecutor)
{
    if (config.inlineStrands)
        return boost::asio::make_strand(
            boost::asio::executor(InlineExecutor{transportExecutor.context()}));
    return boost::asio::make_strand(transportExecutor);
}

}  // namespace ripple

#endif
