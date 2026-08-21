#pragma once

#include <cstddef>
#include <cstdint>

namespace oxmbs::lbm {

constexpr std::uint32_t kLbmAbiVersion{1};  // ABI 字段变化必须升级版本，旧模块不能被猜测性调用。

enum class LbmResponseCode : std::uint32_t {
    Ok = 0,
    Rejected = 1,
    Failed = 2,
    TimedOut = 3,
};

struct LbmRequestView {
    std::uint32_t abi_version{kLbmAbiVersion};  // 模块先据此确认双方字段布局一致，版本不匹配就拒绝读取。
    std::uint64_t task_seq{0};  // 跨 Request/Reply 的任务编号，模块不应修改它，只能原样用于业务日志。
    std::uint64_t session_id{0};  // 上游会话关联号，帮助最终回包找到原请求。
    std::uint64_t deadline_unix_ms{0};  // 绝对毫秒截止时间；Worker 已先拦截过期任务，模块仍可用于自检。
    std::uint32_t msg_type{0};  // XML 路由使用的业务消息类别，不是 System V 的 long mtype。
    std::uint32_t func_id{0};  // 与 msg_type 组合定位导出函数，避免只按单字段误路由。
    const std::uint8_t* payload{nullptr};  // 指向 Worker 当前任务正文的借用指针，模块只能读。
    std::size_t payload_size{0};  // 正文精确字节数，不包含头部和 C 字符串结束符。
};

struct LbmResponseBuffer {
    std::uint32_t abi_version{kLbmAbiVersion};  // 返回时必须保持当前 ABI 版本，否则 Loader 拒绝整份响应。
    LbmResponseCode status{LbmResponseCode::Failed};  // 先以失败为默认值，模块未明确写成功时不会被误报成功。
    std::uint8_t* payload{nullptr};  // Worker 预分配的输出地址；模块不能替换为自己 malloc 的内存。
    std::size_t payload_capacity{0};  // 可写上限，模块写入前必须以它做边界检查。
    std::size_t payload_size{0};  // 模块写入的实际字节数，Loader 会验证不超过 capacity。
};

using LbmEntryPoint = int (*)(const LbmRequestView* request,
                               LbmResponseBuffer* response);  // C ABI 只传稳定指针/整数；0 表示模块执行成功，非 0 转为 InvocationFailed。

bool is_valid_lbm_response_code(LbmResponseCode code) noexcept;

}  // namespace oxmbs::lbm
