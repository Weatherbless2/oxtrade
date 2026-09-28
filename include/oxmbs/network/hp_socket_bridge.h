#pragma once

#include "oxmbs/auth/auth_service.h"
#include "oxmbs/network/hp_socket_server.h"
#include "oxmbs/routing/workfunc_router.h"
#include "oxmbs/worker/reply_collector.h"

#include <cstdint>
#include <string>

namespace oxmbs::logging {
class RunLogger;
}

namespace oxmbs::network {

// 主进程桥接层只负责把网络帧和低频 IPC 连接起来；它不执行数据库查询，也不拥有队列或 Socket 生命周期。
class HpSocketBridge {
public:
    HpSocketBridge(HpSocketServer* server,
                   routing::WorkFuncRouter& router,
                   logging::RunLogger* logger = nullptr) noexcept;

    void attach_server(HpSocketServer& server) noexcept;
    void attach_auth_service(auth::AuthService& service) noexcept;

    static void on_frame(ConnectionId connection_id,
                         const PackMessage& message,
                         void* context);
    static void on_connection_closed(ConnectionId connection_id,
                                     std::uint64_t session_id,
                                     void* context);
    static void on_response(const ipc::IpcRespMsg& response, void* context);
    static void on_timeout(const ipc::ExpiredTask& task, void* context);

private:
    void handle_frame(ConnectionId connection_id, const PackMessage& message);
    void handle_auth_frame(ConnectionId connection_id, const PackMessage& message);
    void handle_response(const ipc::IpcRespMsg& response);
    void handle_timeout(const ipc::ExpiredTask& task);
    void send_error(ConnectionId connection_id,
                    const PackMessage& request,
                    std::uint64_t session_id,
                    PackMessageErrorCode code,
                    const std::string& message);
    void send_auth_response(ConnectionId connection_id,
                            const PackMessage& request,
                            std::uint64_t session_id,
                            PackMessageErrorCode code,
                            const std::string& data,
                            const std::string& message = {});
    static PackMessageErrorCode auth_error(auth::AuthStatus status) noexcept;
    static const char* required_permission(const PackMessage& request) noexcept;
    static PackMessageErrorCode route_error(routing::WorkFuncRouteStatus status) noexcept;
    static PackMessage make_response(const ipc::IpcRespMsg& response);
    static PackMessage make_timeout_response(const ipc::ExpiredTask& task);
    void log_warning(const std::string& message) const;
    void log_info(const std::string& message) const;

    HpSocketServer* server_{nullptr};
    routing::WorkFuncRouter* router_{nullptr};
    auth::AuthService* auth_service_{nullptr};
    logging::RunLogger* logger_{nullptr};
};

}  // namespace oxmbs::network
