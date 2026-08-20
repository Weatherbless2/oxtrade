#pragma once

#include "oxmbs/worker/query_worker_runtime.h"

#include <cstdint>

namespace oxmbs::worker {

struct MockQueryServiceOptions {
    std::uint32_t login_message_type{0};  // 由测试或临时启动配置注入；不能把 Mock 数字误当成正式 HP-Socket 枚举。
    std::uint32_t user_query_message_type{0};  // 同样由外部注入，后续真实协议只替换该映射来源。
};

class MockQueryService {
public:
    explicit MockQueryService(MockQueryServiceOptions options);

    static MockQueryExecutionResult handle(const ipc::IpcTaskMsg& task,
                                           void* context);  // 适配 QueryTaskHandler；context 必须指向仍存活的 MockQueryService。

private:
    MockQueryExecutionResult execute(const ipc::IpcTaskMsg& task) const;  // 不解析密码或访问数据库，只依据注入类型返回稳定的示例业务结果。

    MockQueryServiceOptions options_;
};

}  // namespace oxmbs::worker
