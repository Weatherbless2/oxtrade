#include "oxmbs/lbm/lbm_abi.h"

#include <cstring>
#include <string_view>

namespace {

int write_response(oxmbs::lbm::LbmResponseBuffer* response,
                   oxmbs::lbm::LbmResponseCode status,
                   std::string_view payload) {
    if (response == nullptr || response->payload == nullptr || response->payload_capacity < payload.size()) {  // 模拟模块先检查调用方给的输出合同，避免越界写内存。
        return 10;  // 模拟真实模块发现调用方未提供足够输出缓冲区，返回非零由 Loader 转为 Failed。
    }
    response->abi_version = oxmbs::lbm::kLbmAbiVersion;  // 返回双方都理解的布局版本。
    response->status = status;  // 让测试分别覆盖 Ok 与业务 Rejected。
    std::memcpy(response->payload, payload.data(), payload.size());  // 只写 Worker 提供的容量内存，不跨动态库分配/释放。
    response->payload_size = payload.size();  // 写入精确字节数，Loader 将按这个长度复制结果。
    return 0;  // 0 只代表模块调用过程成功，不等于业务状态一定是 Ok。
}

}  // namespace

extern "C" int oxmbs_lbm_test_execute(const oxmbs::lbm::LbmRequestView* request,
                                       oxmbs::lbm::LbmResponseBuffer* response) {
    if (request == nullptr || request->abi_version != oxmbs::lbm::kLbmAbiVersion ||
        (request->payload == nullptr && request->payload_size != 0)) {
        return 11;  // 输入 ABI 或指针不合法时主动失败，测试 Loader 的 InvocationFailed 映射。
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
