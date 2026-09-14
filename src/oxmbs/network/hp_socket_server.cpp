#include "oxmbs/network/hp_socket_server.h"

#include "oxmbs/logging/run_logger.h"

#include <HPSocket4C.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oxmbs::network {
namespace {

using Clock = std::chrono::steady_clock;

struct ConnectionContext {
    explicit ConnectionContext(ConnectionId value, PackMessageLimits limits)
        : connection_id(value), decoder(limits), last_activity(Clock::now()) {
    }

    ConnectionId connection_id;
    std::uint64_t session_id{0};
    PackMessageFrameDecoder decoder;
    std::deque<PackPayload> send_queue;
    std::size_t pending_send_bytes{0};
    bool send_in_progress{false};
    bool closing{false};
    Clock::time_point last_activity;
    mutable std::mutex mutex;
};

std::mutex g_server_registry_mutex;
std::unordered_map<HP_Server, HpSocketServer*> g_server_registry;

HpSocketServer* find_server(HP_Server sender) noexcept {
    std::lock_guard lock(g_server_registry_mutex);
    const auto iterator = g_server_registry.find(sender);
    return iterator == g_server_registry.end() ? nullptr : iterator->second;
}

const char* hp_error_desc(HP_Server server) noexcept {
    const auto* description = HP_Server_GetLastErrorDesc(server);
    return description == nullptr ? "unknown HP-Socket error" : description;
}

std::string connection_text(ConnectionId connection_id) {
    return "connection_id=" + std::to_string(connection_id);
}

}  // namespace

struct HpSocketServer::Impl {
    explicit Impl(HpSocketServerOptions value)
        : options(std::move(value)) {
    }

    HpSocketServerOptions options;
    HP_TcpServerListener listener{nullptr};
    HP_TcpServer server{nullptr};
    mutable std::mutex mutex;
    std::unordered_map<ConnectionId, std::shared_ptr<ConnectionContext>> connections;
    std::unordered_map<std::uint64_t, ConnectionId> sessions;
    std::condition_variable idle_condition;
    std::mutex idle_mutex;
    std::mutex lifecycle_mutex;  // Stop/Send/Disconnect 共享句柄生命周期锁，防止并发关闭时调用失效句柄。
    std::mutex api_mutex;  // 串行化 HP-Socket 原生 Stop/Send/Disconnect，避免第三方句柄并发操作未定义。
    std::thread idle_thread;
    std::atomic<bool> stop_requested{false};
    bool running{false};

    void log_debug(const std::string& message) const {
        if (options.logger != nullptr) {
            options.logger->debug(message);
        }
    }

    void log_info(const std::string& message) const {
        if (options.logger != nullptr) {
            options.logger->info(message);
        }
    }

    void log_warning(const std::string& message) const {
        if (options.logger != nullptr) {
            options.logger->warn(message);
        }
    }

    std::shared_ptr<ConnectionContext> find_connection(ConnectionId connection_id) const {
        std::lock_guard lock(mutex);
        const auto iterator = connections.find(connection_id);
        return iterator == connections.end() ? nullptr : iterator->second;
    }

    void remove_connection(ConnectionId connection_id) {
        std::lock_guard lock(mutex);
        const auto iterator = connections.find(connection_id);
        if (iterator == connections.end()) {
            return;
        }
        const auto context = iterator->second;
        std::lock_guard context_lock(context->mutex);
        context->closing = true;
        if (context->session_id != 0) {
            sessions.erase(context->session_id);
        }
        connections.erase(iterator);
    }

    void idle_loop() {
        std::unique_lock wait_lock(idle_mutex);
        while (!stop_requested.load(std::memory_order_acquire)) {
            idle_condition.wait_for(wait_lock, options.idle_sweep_interval, [this] {
                return stop_requested.load(std::memory_order_acquire);
            });
            if (stop_requested.load(std::memory_order_acquire)) {
                break;
            }

            std::vector<ConnectionId> expired;
            {
                std::lock_guard lock(mutex);
                const auto now = Clock::now();
                for (const auto& [connection_id, context] : connections) {
                    std::lock_guard context_lock(context->mutex);
                    if (!context->closing && now - context->last_activity >= options.idle_timeout) {
                        expired.push_back(connection_id);
                    }
                }
            }
            for (const auto connection_id : expired) {
                log_info("disconnect idle connection " + connection_text(connection_id));
                std::lock_guard api_lock(api_mutex);
                if (!HP_Server_Disconnect(server, static_cast<HP_CONNID>(connection_id), TRUE)) {
                    log_warning("idle disconnect failed " + connection_text(connection_id) +
                                " error=" + hp_error_desc(server));
                }
            }
        }
    }

    bool options_are_valid(std::string& error) const {
        if (options.bind_host.empty() || options.port == 0 || options.max_connections == 0 ||
            options.keep_alive_time <= std::chrono::milliseconds::zero() ||
            options.keep_alive_interval <= std::chrono::milliseconds::zero() ||
            options.idle_timeout <= std::chrono::milliseconds::zero() ||
            options.idle_sweep_interval <= std::chrono::milliseconds::zero() ||
            options.max_send_queue_bytes == 0) {
            error = "HP-Socket server options contain an empty or non-positive value";
            return false;
        }
        if (options.max_frame_bytes < PackMessageWireLayout::kHeaderBytes ||
            options.max_frame_bytes > PackMessageWireLayout::kDefaultMaxFrameBytes) {
            error = "HP-Socket max_frame_bytes must be between 64 and 4 MiB";
            return false;
        }
        return true;
    }
};

namespace {

En_HP_HandleResult __HP_CALL on_prepare_listen(HP_Server sender, UINT_PTR) {
    auto* server = find_server(sender);
    if (server == nullptr) {
        return HR_ERROR;
    }
    return HR_OK;
}

En_HP_HandleResult __HP_CALL on_accept(HP_Server sender, HP_CONNID connection_id, UINT_PTR) {
    auto* server = find_server(sender);
    if (server == nullptr) {
        return HR_ERROR;
    }
    return server->handle_accept(static_cast<ConnectionId>(connection_id)) == HR_OK ? HR_OK : HR_ERROR;
}

En_HP_HandleResult __HP_CALL on_receive(HP_Server sender,
                                        HP_CONNID connection_id,
                                        const BYTE* data,
                                        int length) {
    auto* server = find_server(sender);
    if (server == nullptr) {
        return HR_ERROR;
    }
    return server->handle_receive(static_cast<ConnectionId>(connection_id), data, length) == HR_OK ? HR_OK : HR_ERROR;
}

En_HP_HandleResult __HP_CALL on_close(HP_Server sender,
                                      HP_CONNID connection_id,
                                      En_HP_SocketOperation operation,
                                      int error_code) {
    auto* server = find_server(sender);
    if (server == nullptr) {
        return HR_ERROR;
    }
    return server->handle_close(static_cast<ConnectionId>(connection_id), static_cast<int>(operation), error_code) == HR_OK
               ? HR_OK
               : HR_ERROR;
}

En_HP_HandleResult __HP_CALL on_shutdown(HP_Server sender) {
    auto* server = find_server(sender);
    if (server == nullptr) {
        return HR_ERROR;
    }
    return HR_OK;
}

}  // namespace

HpSocketServer::HpSocketServer(HpSocketServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {
}

HpSocketServer::~HpSocketServer() {
    std::string ignored;
    stop(ignored);
}

bool HpSocketServer::start(std::string& error) {
    std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
    if (!impl_->options_are_valid(error)) {
        return false;
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->running) {
            error = "HP-Socket server is already running";
            return false;
        }
    }

    auto listener = Create_HP_TcpServerListener();
    if (listener == nullptr) {
        error = "Create_HP_TcpServerListener returned null";
        return false;
    }
    HP_Set_FN_Server_OnPrepareListen(listener, on_prepare_listen);
    HP_Set_FN_Server_OnAccept(listener, on_accept);
    HP_Set_FN_Server_OnReceive(listener, on_receive);
    HP_Set_FN_Server_OnClose(listener, on_close);
    HP_Set_FN_Server_OnShutdown(listener, on_shutdown);

    auto server = Create_HP_TcpServer(listener);
    if (server == nullptr) {
        Destroy_HP_TcpServerListener(listener);
        error = "Create_HP_TcpServer returned null";
        return false;
    }
    HP_Server_SetMaxConnectionCount(server, impl_->options.max_connections);
    if (impl_->options.worker_thread_count != 0) {
        HP_Server_SetWorkerThreadCount(server, impl_->options.worker_thread_count);
    }
    HP_Server_SetMarkSilence(server, TRUE);
    HP_Server_SetOnSendSyncPolicy(server, OSSP_RECEIVE);  // 同一连接的 OnReceive/OnClose 不并发，便于稳定维护 decoder 生命周期。
    HP_TcpServer_SetKeepAliveTime(server, static_cast<DWORD>(impl_->options.keep_alive_time.count()));
    HP_TcpServer_SetKeepAliveInterval(server, static_cast<DWORD>(impl_->options.keep_alive_interval.count()));

    {
        std::lock_guard registry_lock(g_server_registry_mutex);
        g_server_registry.emplace(server, this);  // HP 回调只有不透明 server 句柄，注册表把它还原为 C++ 实例。
    }
    {
        std::lock_guard lock(impl_->mutex);
        impl_->listener = listener;
        impl_->server = server;
        impl_->stop_requested.store(false, std::memory_order_release);
        impl_->running = true;  // 先发布状态，再启动监听，避免首个连接回调看到“未运行”。
    }
    if (!HP_Server_Start(server, impl_->options.bind_host.c_str(), impl_->options.port)) {
        {
            std::lock_guard registry_lock(g_server_registry_mutex);
            g_server_registry.erase(server);
        }
        {
            std::lock_guard lock(impl_->mutex);
            impl_->running = false;
            impl_->server = nullptr;
            impl_->listener = nullptr;
        }
        error = "HP_Server_Start failed: " + std::string(hp_error_desc(server));
        Destroy_HP_TcpServer(server);
        Destroy_HP_TcpServerListener(listener);
        return false;
    }
    impl_->idle_thread = std::thread([this] { impl_->idle_loop(); });
    impl_->log_info("HP-Socket server started at " + impl_->options.bind_host + ":" +
                    std::to_string(impl_->options.port));
    return true;
}

bool HpSocketServer::stop(std::string& error) noexcept {
    std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
    HP_TcpServer server = nullptr;
    HP_TcpServerListener listener = nullptr;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->running) {
            return true;
        }
        server = impl_->server;
        listener = impl_->listener;
        impl_->stop_requested.store(true, std::memory_order_release);
    }
    impl_->idle_condition.notify_all();

    bool stopped = false;
    {
        std::lock_guard api_lock(impl_->api_mutex);
        stopped = HP_Server_Stop(server) != FALSE;
    }
    if (!stopped) {
        error = "HP_Server_Stop failed: " + std::string(hp_error_desc(server));
    }
    if (impl_->idle_thread.joinable()) {
        impl_->idle_thread.join();
    }
    {
        std::lock_guard lock(impl_->mutex);
        for (auto& [connection_id, context] : impl_->connections) {
            (void)connection_id;
            std::lock_guard context_lock(context->mutex);
            context->closing = true;
            context->send_queue.clear();
            context->pending_send_bytes = 0;
        }
        impl_->connections.clear();
        impl_->sessions.clear();
        impl_->running = false;
        impl_->server = nullptr;
        impl_->listener = nullptr;
    }
    {
        std::lock_guard registry_lock(g_server_registry_mutex);
        g_server_registry.erase(server);
    }
    Destroy_HP_TcpServer(server);
    Destroy_HP_TcpServerListener(listener);
    return stopped;
}

bool HpSocketServer::is_running() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->running;
}

bool HpSocketServer::send(ConnectionId connection_id,
                          const PackMessage& message,
                          std::string& error) {
    PackPayload frame;
    PackMessageStatus status = PackMessageStatus::InvalidArgument;
    try {
        status = encode_pack_message(message, frame, error,
                                     PackMessageLimits{impl_->options.max_frame_bytes});
    } catch (const std::bad_alloc&) {
        error = "cannot allocate encoded PackMessage frame";
    }
    if (status != PackMessageStatus::Ok) {
        return false;
    }
    return send_frame(connection_id, frame, error);
}

bool HpSocketServer::send_frame(ConnectionId connection_id,
                                const PackPayload& frame,
                                std::string& error) {
    std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
    if (frame.empty() || frame.size() > impl_->options.max_frame_bytes) {
        error = "send frame is empty or exceeds max_frame_bytes";
        return false;
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->running || impl_->server == nullptr) {
            error = "HP-Socket server is not running";
            return false;
        }
    }
    const auto context = impl_->find_connection(connection_id);
    if (context == nullptr) {
        error = "connection is not registered: " + connection_text(connection_id);
        return false;
    }

    try {
        {
            std::lock_guard lock(context->mutex);
            if (context->closing) {
                error = "connection is closing: " + connection_text(connection_id);
                return false;
            }
            if (frame.size() > impl_->options.max_send_queue_bytes -
                                  std::min(context->pending_send_bytes, impl_->options.max_send_queue_bytes)) {
                error = "connection send queue limit exceeded: " + connection_text(connection_id);
                return false;
            }
            context->send_queue.push_back(frame);  // 先受应用层上限保护，再让 HP-Socket 接管一份稳定的字节副本。
            context->pending_send_bytes += frame.size();
            if (context->send_in_progress) {
                return true;
            }
            context->send_in_progress = true;
        }

        while (true) {
            PackPayload current;
            {
                std::lock_guard lock(context->mutex);
                if (context->send_queue.empty()) {
                    context->send_in_progress = false;
                    return true;
                }
                current = context->send_queue.front();
            }
            bool sent = false;
            {
                std::lock_guard api_lock(impl_->api_mutex);
                sent = HP_Server_Send(impl_->server,
                                      static_cast<HP_CONNID>(connection_id),
                                      current.data(),
                                      static_cast<int>(current.size())) != FALSE;
            }
            if (!sent) {
                std::lock_guard lock(context->mutex);
                context->send_queue.clear();
                context->pending_send_bytes = 0;
                context->send_in_progress = false;
                error = "HP_Server_Send failed: " + std::string(hp_error_desc(impl_->server));
                return false;
            }
            {
                std::lock_guard lock(context->mutex);
                if (!context->send_queue.empty()) {
                    context->pending_send_bytes -= context->send_queue.front().size();
                    context->send_queue.pop_front();
                }
            }
        }
    } catch (const std::bad_alloc&) {
        error = "cannot allocate connection send queue entry";
        return false;
    }
}

bool HpSocketServer::send_to_session(std::uint64_t session_id,
                                     const PackMessage& message,
                                     std::string& error) {
    ConnectionId connection_id = 0;
    {
        std::lock_guard lock(impl_->mutex);
        const auto iterator = impl_->sessions.find(session_id);
        if (iterator == impl_->sessions.end()) {
            error = "session is not bound to a live connection";
            return false;
        }
        connection_id = iterator->second;
    }
    return send(connection_id, message, error);
}

bool HpSocketServer::bind_session(ConnectionId connection_id,
                                   std::uint64_t session_id,
                                   std::string& error) {
    if (session_id == 0) {
        error = "session_id must be non-zero";
        return false;
    }
    const auto context = impl_->find_connection(connection_id);
    if (context == nullptr) {
        error = "connection is not registered: " + connection_text(connection_id);
        return false;
    }
    std::lock_guard lock(impl_->mutex);
    const auto existing = impl_->sessions.find(session_id);
    if (existing != impl_->sessions.end() && existing->second != connection_id) {
        error = "session_id is already bound to another connection";
        return false;
    }
    std::lock_guard context_lock(context->mutex);
    if (context->closing) {
        error = "connection is closing: " + connection_text(connection_id);
        return false;
    }
    if (context->session_id != 0 && context->session_id != session_id) {
        impl_->sessions.erase(context->session_id);
    }
    context->session_id = session_id;
    impl_->sessions[session_id] = connection_id;
    return true;
}

bool HpSocketServer::unbind_session(ConnectionId connection_id,
                                     std::uint64_t session_id,
                                     std::string& error) {
    std::lock_guard lock(impl_->mutex);
    const auto iterator = impl_->sessions.find(session_id);
    if (iterator == impl_->sessions.end() || iterator->second != connection_id) {
        error = "session is not bound to this connection";
        return false;
    }
    const auto context_iterator = impl_->connections.find(connection_id);
    if (context_iterator != impl_->connections.end()) {
        std::lock_guard context_lock(context_iterator->second->mutex);
        context_iterator->second->session_id = 0;
    }
    impl_->sessions.erase(iterator);
    return true;
}

bool HpSocketServer::close_connection(ConnectionId connection_id, std::string& error) {
    std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->running || impl_->server == nullptr) {
            error = "HP-Socket server is not running";
            return false;
        }
    }
    std::lock_guard api_lock(impl_->api_mutex);
    if (!HP_Server_Disconnect(impl_->server, static_cast<HP_CONNID>(connection_id), TRUE)) {
        error = "HP_Server_Disconnect failed: " + std::string(hp_error_desc(impl_->server));
        return false;
    }
    return true;
}

std::size_t HpSocketServer::connection_count() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->connections.size();
}

HpSocketServerStats HpSocketServer::stats() const noexcept {
    HpSocketServerStats result;
    std::lock_guard lock(impl_->mutex);
    result.connection_count = impl_->connections.size();
    for (const auto& [connection_id, context] : impl_->connections) {
        (void)connection_id;
        std::lock_guard context_lock(context->mutex);
        result.buffered_frame_bytes += context->decoder.buffered_bytes();
        result.pending_send_bytes += context->pending_send_bytes;
    }
    return result;
}

int HpSocketServer::handle_accept(ConnectionId connection_id) {
    std::shared_ptr<ConnectionContext> context;
    try {
        context = std::make_shared<ConnectionContext>(connection_id,
                                                       PackMessageLimits{impl_->options.max_frame_bytes});
    } catch (const std::bad_alloc&) {
        impl_->log_warning("cannot allocate connection context " + connection_text(connection_id));
        return HR_ERROR;
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->running || impl_->connections.size() >= impl_->options.max_connections) {
            impl_->log_warning("reject connection at capacity " + connection_text(connection_id));
            return HR_ERROR;
        }
        impl_->connections.emplace(connection_id, context);
    }
    impl_->log_info("accepted " + connection_text(connection_id));
    return HR_OK;
}

int HpSocketServer::handle_receive(ConnectionId connection_id,
                                   const std::uint8_t* data,
                                   int length) {
    if (data == nullptr || length <= 0) {
        impl_->log_warning("invalid receive buffer " + connection_text(connection_id));
        return HR_ERROR;
    }
    const auto context = impl_->find_connection(connection_id);
    if (context == nullptr) {
        return HR_ERROR;
    }

    std::vector<PackMessage> messages;
    std::string error;
    PackMessageStatus status;
    try {
        std::lock_guard lock(context->mutex);
        context->last_activity = Clock::now();  // 任意有效 TCP 数据都刷新空闲计时，心跳也因此可以维持连接。
        status = context->decoder.append(data, static_cast<std::size_t>(length), messages, error);
    } catch (const std::bad_alloc&) {
        status = PackMessageStatus::FrameTooLarge;
        error = "cannot allocate connection receive buffer";
    } catch (const std::exception& exception) {
        status = PackMessageStatus::InvalidPayload;
        error = exception.what();
    }
    if (status != PackMessageStatus::Ok && status != PackMessageStatus::NeedMoreData) {
        impl_->log_warning("drop invalid PackMessage " + connection_text(connection_id) +
                           " status=" + std::string(pack_message_status_name(status)) +
                           " error=" + error);
        std::string ignored;
        close_connection(connection_id, ignored);  // 协议边界错误不进入业务/IPC，直接隔离该连接。
        return HR_ERROR;
    }

    for (const auto& message : messages) {
        if (message.kind == PackMessageKind::Heartbeat) {
            PackMessage heartbeat;
            heartbeat.kind = PackMessageKind::Heartbeat;
            heartbeat.request_id = message.request_id;
            heartbeat.session_id = message.session_id;
            heartbeat.sequence = message.sequence;
            heartbeat.timestamp_ms = message.timestamp_ms;
            heartbeat.payload = message.payload;
            std::string send_error;
            if (!send(connection_id, heartbeat, send_error)) {
                impl_->log_warning("heartbeat response failed " + connection_text(connection_id) +
                                   " error=" + send_error);
                return HR_ERROR;
            }
            continue;
        }
        if (impl_->options.frame_sink != nullptr) {
            try {
                impl_->options.frame_sink(connection_id, message, impl_->options.frame_sink_context);
            } catch (const std::exception& exception) {
                impl_->log_warning("PackMessage sink threw for " + connection_text(connection_id) +
                                   " error=" + exception.what());
                std::string ignored;
                close_connection(connection_id, ignored);
                return HR_ERROR;
            } catch (...) {
                impl_->log_warning("PackMessage sink threw unknown exception for " + connection_text(connection_id));
                std::string ignored;
                close_connection(connection_id, ignored);
                return HR_ERROR;
            }
        }
    }
    return HR_OK;
}

int HpSocketServer::handle_close(ConnectionId connection_id,
                                 int operation,
                                 int error_code) {
    impl_->remove_connection(connection_id);
    impl_->log_info("closed " + connection_text(connection_id) +
                    " operation=" + std::to_string(operation) +
                    " error_code=" + std::to_string(error_code));
    return HR_OK;
}

}  // namespace oxmbs::network
