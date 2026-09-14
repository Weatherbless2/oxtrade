#pragma once

#include "oxmbs/ipc/task_protocol.h"

#include <cstddef>
#include <cstdint>

namespace oxmbs::routing {

// 临时适配模型：真实 HP-Socket PackMessage 尚未提供时，用它表达 WorkFunc 已解析出的借用消息视图。
// 后续只需在适配层把 PackMessage 转成同样的字段，路由、协议和 IPC 代码不应依赖真实结构体的内存布局。
struct MockPackMessage {
    ipc::SessionId session_id{0};  // 模拟客户端连接标识；路由只透传给 #5 关联表，不持有任何网络对象。
    std::uint32_t msg_type{0};  // 模拟业务消息枚举；具体数值仅在测试策略注入，不能视为正式协议号。
    std::uint32_t func_id{0};  // 预留给后续 funclist/LBM 的函数标识，路由层原样写入 IpcTaskMsg。
    const std::uint8_t* data{nullptr};  // 上游缓冲区借用指针，route() 返回后可能被无锁队列复用，绝不能跨进程保存。
    std::size_t data_len{0};  // 原始二进制正文长度；不包含字符串终止符，也不假定正文是 JSON 或 UTF-8。
    ipc::TaskSequence request_id{0};  // 正式网络请求标识；旧 Mock 聚合初始化不提供时保持 0。
};

}  // namespace oxmbs::routing
