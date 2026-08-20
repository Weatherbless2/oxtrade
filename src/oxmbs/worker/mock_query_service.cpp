#include "oxmbs/worker/mock_query_service.h"

#include <string>

namespace oxmbs::worker {
namespace {

ipc::IpcPayload payload_from_text(const char* text) {
    const std::string value{text};
    return ipc::IpcPayload(value.begin(), value.end());  // Mock Response 仍按协议二进制 payload 传输，不要求调用方把它当 C 字符串。
}

}  // namespace

MockQueryService::MockQueryService(MockQueryServiceOptions options)
    : options_(options) {
}

MockQueryExecutionResult MockQueryService::handle(const ipc::IpcTaskMsg& task, void* context) {
    if (context == nullptr) {
        MockQueryExecutionResult result;
        result.status = ipc::IpcResponseStatus::Failed;
        result.payload = payload_from_text("{\"ok\":false,\"error\":\"mock-service-context-missing\"}");
        return result;  // 上下文错误变成可关联的 Failed Reply，不让 Worker 因测试/配置错误直接崩溃。
    }
    return static_cast<MockQueryService*>(context)->execute(task);
}

MockQueryExecutionResult MockQueryService::execute(const ipc::IpcTaskMsg& task) const {
    MockQueryExecutionResult result;
    if (task.msg_type == options_.login_message_type && options_.login_message_type != 0) {
        result.status = ipc::IpcResponseStatus::Ok;
        result.payload = payload_from_text(
            "{\"ok\":true,\"user\":{\"id\":\"mock-user-1001\",\"name\":\"Mock Alice\",\"role\":\"operator\"}}");
        return result;  // 只模拟认证成功后的用户资料；不读取或验证请求中的用户名、密码或资金密码。
    }
    if (task.msg_type == options_.user_query_message_type && options_.user_query_message_type != 0) {
        result.status = ipc::IpcResponseStatus::Ok;
        result.payload = payload_from_text(
            "{\"id\":\"mock-user-1001\",\"name\":\"Mock Alice\",\"status\":\"active\",\"roles\":[\"operator\"]}");
        return result;  // 模拟已登录用户的查询结果，后续 #12 再改为权限与数据库查询。
    }

    result.status = ipc::IpcResponseStatus::Rejected;
    result.payload = payload_from_text("{\"ok\":false,\"error\":\"mock-unsupported-message-type\"}");
    return result;  // 未映射类型显式拒绝，不把未知消息伪装成成功的 Mock 查询。
}

}  // namespace oxmbs::worker
