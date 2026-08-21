#include "oxmbs/lbm/lbm_abi.h"

#include <cstring>
#include <string_view>

namespace {

int write_response(oxmbs::lbm::LbmResponseBuffer* response,
                   oxmbs::lbm::LbmResponseCode status,
                   std::string_view payload) {
    if (response == nullptr || response->payload == nullptr || response->payload_capacity < payload.size()) {
        return 10;  // 模拟真实模块发现调用方未提供足够输出缓冲区，返回非零由 Loader 转为 Failed。
    }
    response->abi_version = oxmbs::lbm::kLbmAbiVersion;
    response->status = status;
    std::memcpy(response->payload, payload.data(), payload.size());  // 输出字节写入 Worker 提供的缓冲区，不跨动态库分配内存。
    response->payload_size = payload.size();
    return 0;
}

}  // namespace

extern "C" int oxmbs_lbm_test_execute(const oxmbs::lbm::LbmRequestView* request,
                                       oxmbs::lbm::LbmResponseBuffer* response) {
    if (request == nullptr || request->abi_version != oxmbs::lbm::kLbmAbiVersion ||
        (request->payload == nullptr && request->payload_size != 0)) {
        return 11;
    }
    return write_response(response, oxmbs::lbm::LbmResponseCode::Ok, "mock-lbm-ok");
}

extern "C" int oxmbs_lbm_test_reject(const oxmbs::lbm::LbmRequestView*,
                                      oxmbs::lbm::LbmResponseBuffer* response) {
    return write_response(response, oxmbs::lbm::LbmResponseCode::Rejected, "mock-lbm-rejected");
}

extern "C" int oxmbs_lbm_test_invalid_response(const oxmbs::lbm::LbmRequestView*,
                                                oxmbs::lbm::LbmResponseBuffer* response) {
    if (response == nullptr) {
        return 12;
    }
    response->abi_version = oxmbs::lbm::kLbmAbiVersion + 1;  // 故意伪造 ABI 版本，验证 Loader 绝不读取不兼容模块结果。
    response->status = oxmbs::lbm::LbmResponseCode::Ok;
    response->payload_size = 0;
    return 0;
}

extern "C" int oxmbs_lbm_test_return_failure(const oxmbs::lbm::LbmRequestView*,
                                              oxmbs::lbm::LbmResponseBuffer*) {
    return 13;  // 模拟 LBM 内部失败，非零返回值必须映射为 InvocationFailed。
}
