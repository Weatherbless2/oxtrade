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
    std::uint32_t abi_version{kLbmAbiVersion};
    std::uint64_t task_seq{0};
    std::uint64_t session_id{0};
    std::uint64_t deadline_unix_ms{0};
    std::uint32_t msg_type{0};
    std::uint32_t func_id{0};
    const std::uint8_t* payload{nullptr};
    std::size_t payload_size{0};  // 仅借用 Worker 的请求内存，LBM 不得保存该指针。
};

struct LbmResponseBuffer {
    std::uint32_t abi_version{kLbmAbiVersion};
    LbmResponseCode status{LbmResponseCode::Failed};
    std::uint8_t* payload{nullptr};
    std::size_t payload_capacity{0};
    std::size_t payload_size{0};  // LBM 写入实际长度，超出 capacity 会被适配层拒绝。
};

using LbmEntryPoint = int (*)(const LbmRequestView* request,
                               LbmResponseBuffer* response);  // 0 表示模块执行成功，非 0 表示模块调用失败。

bool is_valid_lbm_response_code(LbmResponseCode code) noexcept;

}  // namespace oxmbs::lbm
