#pragma once
//------------------------------------------------------------------------------
// Transport — abstracts PeerImp's socket I/O
// (read/write/close/executor/shutdown
// + the handshake shared-value) behind a runtime virtual interface, so the REAL
// PeerImp can run over the production ssl_stream OR an in-process bus (the
// Stage 1 test harness). See spec §5.1: this is a thin, devirtualizable shim,
// NOT the deleted ~40-site PeerImpT<Transport> template weld. PeerImp is
// constructed at exactly two sites (OverlayImpl.cpp, ConnectAttempt.cpp), both
// of which wrap their ssl_stream in an SslTransport.
//------------------------------------------------------------------------------
#include <xrpld/overlay/detail/Handshake.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/utility/Journal.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ripple {

class Transport
{
public:
    using socket_type = boost::asio::ip::tcp::socket;
    using middle_type = boost::beast::tcp_stream;
    using stream_type = boost::beast::ssl_stream<middle_type>;
    using executor_type = boost::asio::any_io_executor;
    using error_code = boost::system::error_code;
    using ReadHandler = std::function<void(error_code, std::size_t)>;
    using WriteHandler = std::function<void(error_code, std::size_t)>;
    using ShutdownHandler = std::function<void(error_code)>;

    virtual ~Transport() = default;

    // The underlying executor PeerImp builds its strand from.
    [[nodiscard]] virtual executor_type
    get_executor() = 0;

    [[nodiscard]] virtual bool
    is_open() const = 0;

    // Completions are invoked on `strand` (the impl binds it), matching prod's
    // bind_executor(strand_, handler). Buffers are passed as vectors so the
    // interface can stay non-templated; the impl copies the descriptors into
    // the composed op (the underlying bytes are owned by the caller for the
    // op's life).
    virtual void
    async_read_some(
        std::vector<boost::asio::mutable_buffer> buffers,
        executor_type strand,
        ReadHandler handler) = 0;

    virtual void
    async_write(
        std::vector<boost::asio::const_buffer> buffers,
        executor_type strand,
        WriteHandler handler) = 0;

    virtual void
    async_shutdown(executor_type strand, ShutdownHandler handler) = 0;

    // Hard, immediate socket close (prod's `socket_.close(ec)`; the error is
    // discarded, matching the call site's
    // NOLINT(bugprone-unused-return-value)). Distinct from async_shutdown's
    // graceful TLS teardown.
    virtual void
    close() = 0;

    // The one TLS-internals touch (handshake, cold/once-per-connection). Prod
    // hashes the live OpenSSL session; the sim impl returns a deterministic
    // value.
    [[nodiscard]] virtual std::optional<uint256>
    makeSharedValue(beast::Journal journal) = 0;
};

// Production transport: a thin shim over the real ssl_stream. Behaviour is
// bit-identical to the pre-seam direct stream use.
class SslTransport : public Transport
{
    std::unique_ptr<stream_type> stream_;
    socket_type& socket_;

public:
    explicit SslTransport(std::unique_ptr<stream_type> stream)
        : stream_(std::move(stream)), socket_(stream_->next_layer().socket())
    {
    }

    [[nodiscard]] executor_type
    get_executor() override
    {
        return socket_.get_executor();
    }

    [[nodiscard]] bool
    is_open() const override
    {
        return socket_.is_open();
    }

    void
    async_read_some(
        std::vector<boost::asio::mutable_buffer> buffers,
        executor_type strand,
        ReadHandler handler) override
    {
        stream_->async_read_some(
            buffers, boost::asio::bind_executor(strand, std::move(handler)));
    }

    void
    async_write(
        std::vector<boost::asio::const_buffer> buffers,
        executor_type strand,
        WriteHandler handler) override
    {
        // The composed async_write transfers ALL bytes (prod's transfer_all()).
        boost::asio::async_write(
            *stream_,
            buffers,
            boost::asio::bind_executor(strand, std::move(handler)));
    }

    void
    async_shutdown(executor_type strand, ShutdownHandler handler) override
    {
        stream_->async_shutdown(
            boost::asio::bind_executor(strand, std::move(handler)));
    }

    void
    close() override
    {
        error_code ec;
        socket_.close(ec);  // NOLINT(bugprone-unused-return-value)
    }

    [[nodiscard]] std::optional<uint256>
    makeSharedValue(beast::Journal journal) override
    {
        return ripple::makeSharedValue(*stream_, journal);
    }
};

// Helper: flatten any asio buffer sequence into the vector the Transport takes.
template <class BufferSequence>
std::vector<boost::asio::const_buffer>
toConstBuffers(BufferSequence const& seq)
{
    return std::vector<boost::asio::const_buffer>(
        boost::asio::buffer_sequence_begin(seq),
        boost::asio::buffer_sequence_end(seq));
}

template <class BufferSequence>
std::vector<boost::asio::mutable_buffer>
toMutableBuffers(BufferSequence const& seq)
{
    return std::vector<boost::asio::mutable_buffer>(
        boost::asio::buffer_sequence_begin(seq),
        boost::asio::buffer_sequence_end(seq));
}

}  // namespace ripple
