#pragma once

#include "oxmbs/ipc/ipc_queue_manager.h"
#include "oxmbs/ipc/task_correlation.h"
#include "oxmbs/ipc/task_protocol.h"
#include "oxmbs/routing/message_route_policy.h"
#include "oxmbs/routing/mock_pack_message.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace oxmbs::logging {
class RunLogger;
}

namespace oxmbs::routing {

enum class WorkFuncRouteStatus {
    HighFrequencyPassthrough,  // 不属于低频白名单，调用方继续走既有交易、行情或托管实时链路。
    ForwardedToWorker,         // 已完成深拷贝、登记、编码并非阻塞写入 Request Queue。
    InvalidMessage,            // 低频消息的 session、payload 指针、长度或 msg_type 不合法，未登记也未投递。
    InvalidRouterOptions,      // timeout、mtype 或协议长度限制错误，拒绝路由以避免产生不可回收任务。
    PayloadTooLarge,           // payload 加协议固定头超过 #5 的最大帧限制，未登记也未投递。
    PayloadCopyFailed,         // 深拷贝上游借用缓冲区时内存分配失败，未登记也未投递。
    RegistryRejected,          // #5 无法分配 task_seq 或 deadline，未投递。
    EncodingRejected,          // 已登记但协议编码失败；路由器会立即回滚 pending_ 记录。
    QueueSendFailed,           // 非阻塞入队失败；路由器会立即回滚 pending_ 记录且不等待队列空间。
};

const char* workfunc_route_status_name(WorkFuncRouteStatus status) noexcept;

struct WorkFuncRouterOptions {
    ipc::TaskProtocolLimits protocol_limits{};  // 临时默认 4 KiB；后续应与运行配置和内核 MSGMAX 一起统一管理。
    std::chrono::milliseconds query_task_timeout{1000};  // Mock 阶段的显式暂定值；真实部署应由 mbsconfig.ini 注入。
    long request_message_type{1};  // System V 传输层 mtype，不等同于业务 msg_type，必须保持正数。
};

struct WorkFuncRouteResult {
    WorkFuncRouteStatus status{WorkFuncRouteStatus::InvalidMessage};
    ipc::TaskSequence task_seq{0};  // 仅登记成功后非零；便于日志和后续 Reply 追踪。
    ipc::TaskCorrelationStatus correlation_status{ipc::TaskCorrelationStatus::InvalidArgument};
    ipc::TaskCorrelationStatus rollback_status{ipc::TaskCorrelationStatus::InvalidArgument};  // 只在编码或入队失败后记录撤销结果。
    ipc::IpcResult ipc_result{ipc::IpcResult::Ok};  // 仅队列操作后反映内核语义，例如 WouldBlock、QueueRemoved。
    std::string error;

    bool forwarded() const noexcept { return status == WorkFuncRouteStatus::ForwardedToWorker; }
};

class WorkFuncRouter {
public:
    WorkFuncRouter(ipc::IpcQueueManager& queue_manager,
                   ipc::TaskCorrelationRegistry& correlation_registry,
                   MessageRoutePolicy route_policy,
                   WorkFuncRouterOptions options = {},
                   logging::RunLogger* logger = nullptr);

    bool is_query_or_manage_msg(const MockPackMessage& message) const noexcept;  // 仅判断白名单，不读取或保存上游 payload。
    WorkFuncRouteResult route(const MockPackMessage& message);  // WorkFunc 调用入口：高频返回直通标识，低频才进入 ForwardToWorkerProcessPool。
    WorkFuncRouteResult forward_to_worker_process_pool(const MockPackMessage& message);  // 对应 Mentor 的 ForwardToWorkerProcessPool，保证不做慢 IO 或等待队列空间。

private:
    bool options_are_valid() const noexcept;
    void log_debug(const std::string& message) const;
    void log_warning(const std::string& message) const;

    ipc::IpcQueueManager& queue_manager_;  // 只借用主进程拥有的双队列；本模块没有 IPC_RMID 删除责任。
    ipc::TaskCorrelationRegistry& correlation_registry_;  // 只借用主进程关联表；Worker 进程不得持有或调用它。
    MessageRoutePolicy route_policy_;  // 值持有不可变白名单，Mock 数值替换为真实枚举时无需改动路由流程。
    WorkFuncRouterOptions options_;
    logging::RunLogger* logger_{nullptr};  // 可选运行日志；空指针用于纯单元测试，不影响路由结果。
};

}  // namespace oxmbs::routing
