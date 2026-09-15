#pragma once

#include "oxmbs/service/qmt_service_router.h"

#include <cstddef>
#include <cstdint>

namespace oxmbs::lbm {

constexpr std::uint32_t kQmtLbmHostAbiVersion{1};  // 宿主端口与 LBM 请求 ABI 分开版本化，避免模块猜测 Repository 指针布局。

// 同一产品构建内的只读宿主端口：模块只借用 Repository，不拥有连接池、Socket 或 IPC 队列。
struct QmtLbmHostContext {
    std::uint32_t abi_version{kQmtLbmHostAbiVersion};
    service::IQmtRepository* repository{nullptr};
    std::size_t max_page_size{50};
    std::size_t max_instrument_count{200};
    std::size_t max_response_payload_bytes{
        ipc::TaskProtocolLimits{}.max_frame_bytes - ipc::IpcWireLayout::kHeaderBytes};
    bool require_authenticated_session{true};
};

}  // namespace oxmbs::lbm
