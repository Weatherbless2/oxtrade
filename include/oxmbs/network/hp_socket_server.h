#pragma once

#include "oxmbs/network/pack_message.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace oxmbs::logging {
class RunLogger;
}

namespace oxmbs::network {

using ConnectionId = std::uint64_t;
using PackFrameSink = void (*)(ConnectionId connection_id,
                               const PackMessage& message,
                               void* context);
using ConnectionClosedSink = void (*)(ConnectionId connection_id,
                                      std::uint64_t session_id,
                                      void* context);

struct HpSocketServerOptions {
    std::string bind_host{"127.0.0.1"};
    std::uint16_t port{18080};
    std::uint32_t max_connections{64};
    std::uint32_t worker_thread_count{0};  // 0 表示使用 HP-Socket 默认线程策略。
    std::chrono::milliseconds keep_alive_time{30000};
    std::chrono::milliseconds keep_alive_interval{10000};
    std::chrono::milliseconds idle_timeout{60000};
    std::chrono::milliseconds idle_sweep_interval{1000};
    std::size_t max_frame_bytes{PackMessageWireLayout::kDefaultMaxFrameBytes};
    std::size_t max_send_queue_bytes{8U * 1024U * 1024U};
    PackFrameSink frame_sink{nullptr};
    void* frame_sink_context{nullptr};
    ConnectionClosedSink connection_closed_sink{nullptr};
    void* connection_closed_context{nullptr};
    logging::RunLogger* logger{nullptr};
};

struct HpSocketServerStats {
    std::size_t connection_count{0};
    std::size_t buffered_frame_bytes{0};
    std::size_t pending_send_bytes{0};
};

class HpSocketServer {
public:
    explicit HpSocketServer(HpSocketServerOptions options = {});
    ~HpSocketServer();

    HpSocketServer(const HpSocketServer&) = delete;
    HpSocketServer& operator=(const HpSocketServer&) = delete;

    bool start(std::string& error);
    bool stop(std::string& error) noexcept;
    bool is_running() const noexcept;

    bool send(ConnectionId connection_id,
              const PackMessage& message,
              std::string& error);
    bool send_frame(ConnectionId connection_id,
                    const PackPayload& frame,
                    std::string& error);
    bool send_to_session(std::uint64_t session_id,
                         const PackMessage& message,
                         std::string& error);

    bool bind_session(ConnectionId connection_id,
                      std::uint64_t session_id,
                      std::string& error);
    bool unbind_session(ConnectionId connection_id,
                        std::uint64_t session_id,
                        std::string& error);
    bool close_connection(ConnectionId connection_id, std::string& error);

    std::size_t connection_count() const noexcept;
    HpSocketServerStats stats() const noexcept;

    // 以下三个入口仅供 HP-Socket C 回调适配器调用，业务代码不应直接调用。
    int handle_accept(ConnectionId connection_id);
    int handle_receive(ConnectionId connection_id, const std::uint8_t* data, int length);
    int handle_close(ConnectionId connection_id, int operation, int error_code);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace oxmbs::network
