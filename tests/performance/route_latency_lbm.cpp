#include "oxmbs/lbm/lbm_abi.h"

#include <cstring>

extern "C" int oxmbs_route_latency_echo(const oxmbs::lbm::LbmRequestView* request,
                                         oxmbs::lbm::LbmResponseBuffer* response) {
    if (request == nullptr || response == nullptr ||
        request->abi_version != oxmbs::lbm::kLbmAbiVersion ||
        (request->payload == nullptr && request->payload_size != 0) ||
        response->payload == nullptr || response->payload_capacity < request->payload_size) {
        return 1;  // 基准模块也遵守真实 LBM ABI；输入或输出边界错误必须显式失败，不能缩短成不安全的 memcpy。
    }
    response->abi_version = oxmbs::lbm::kLbmAbiVersion;  // 保持调用方要求的 ABI 版本，Loader 会在复制结果前复核。
    response->status = oxmbs::lbm::LbmResponseCode::Ok;  // 两条路线都执行成功的原样应答，不把业务拒绝时间混入 IPC 比较。
    std::memcpy(response->payload, request->payload, request->payload_size);  // 低频 LBM 与高频直接处理都复制同样字节数，保持业务工作量一致。
    response->payload_size = request->payload_size;  // 返回精确正文长度，不包含 Task/Response 的 56 字节协议头。
    return 0;  // 返回 0 只代表动态库调用成功；业务状态仍由 response->status 表达。
}
