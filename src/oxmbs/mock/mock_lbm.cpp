#include "oxmbs/lbm/lbm_abi.h"
#include "oxmbs/mock/mock_backend_protocol.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Form = std::map<std::string, std::string>;
using Record = std::map<std::string, std::string>;
using Records = std::vector<Record>;

struct MockStore {
    std::map<std::string, Records> resources;
};

struct MockResult {
    oxmbs::lbm::LbmResponseCode response_code{oxmbs::lbm::LbmResponseCode::Ok};
    std::string app_status{"succeeded"};
    std::string error_code{"none"};
    std::string error_message;
    std::string payload_json{"{}"};
};

constexpr std::size_t kMaxMockPageSize{10};  // Mock LBM 的单页限制保证 JSON 结果和 4 KiB IPC 帧同时可控，完整数据通过分页读取。

std::string url_encode(std::string_view source) {
    std::ostringstream output;
    output << std::uppercase << std::hex;
    for (const unsigned char character : source) {
        if (std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            output << static_cast<char>(character);
        } else {
            output << '%' << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(character);
        }
    }
    return output.str();
}

int hex_value(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

std::string url_decode(std::string_view source) {
    std::string output;
    output.reserve(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        if (source[index] == '%' && index + 2 < source.size()) {
            const int high = hex_value(source[index + 1]);
            const int low = hex_value(source[index + 2]);
            if (high >= 0 && low >= 0) {
                output.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        output.push_back(source[index] == '+' ? ' ' : source[index]);
    }
    return output;
}

Form parse_form(std::string_view source) {
    Form form;
    std::size_t start = 0;
    while (start <= source.size()) {
        const std::size_t end = source.find('&', start);
        const std::string_view pair = source.substr(start, end == std::string_view::npos ? source.size() - start : end - start);
        if (!pair.empty()) {
            const std::size_t separator = pair.find('=');
            const std::string key = url_decode(pair.substr(0, separator));
            const std::string value = separator == std::string_view::npos ? std::string{} : url_decode(pair.substr(separator + 1));
            if (!key.empty()) {
                form[key] = value;  // Mock 协议只允许每个键一个最终值，重复键以后一次为准，避免含糊的 CRUD 语义。
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return form;
}

std::string form_value(const Form& form, std::string_view key) {
    const auto found = form.find(std::string(key));
    return found == form.end() ? std::string{} : found->second;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

int parse_positive_int(const std::string& text, int fallback) {
    try {
        const int value = std::stoi(text);
        return value > 0 ? value : fallback;
    } catch (...) {
        return fallback;  // Mock 输入不合法时回到稳定默认分页，避免异常穿过动态库 C ABI。
    }
}

std::string json_escape(std::string_view source) {
    std::ostringstream output;
    for (const unsigned char character : source) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20U) {
                output << "\\u" << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned int>(character) << std::dec;
            } else {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    return output.str();
}

std::string json_record(const Record& record) {
    std::ostringstream output;
    output << '{';
    bool first = true;
    for (const auto& [key, value] : record) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << '"' << json_escape(key) << "\":";
        if ((key == "enabled" || key == "handled" || key == "multi_login") && (value == "true" || value == "false")) {
            output << value;  // 布尔列按 JSON bool 返回，Qt 仍可渲染，后续真实协议无需做字符串猜测。
        } else {
            output << '"' << json_escape(value) << '"';
        }
    }
    output << '}';
    return output.str();
}

std::string json_items(const Records& rows, std::size_t start, std::size_t end) {
    std::ostringstream output;
    output << '[';
    for (std::size_t index = start; index < end; ++index) {
        if (index != start) {
            output << ',';
        }
        output << json_record(rows[index]);
    }
    output << ']';
    return output.str();
}

std::string resource_for_service(const std::string& service) {
    if (service == "system") {
        return "menus";
    }
    if (service == "funds") {
        return "products";
    }
    if (service == "risk") {
        return "risk_rules";
    }
    if (service == "dictionaries" || service == "dictionary_details" || service == "system_logs" ||
        service == "role_rights" || service == "users" || service == "roles" || service == "sites" ||
        service == "projects" || service == "accounts" || service == "fund_project_links" ||
        service == "risk_parameters" || service == "risk_events") {
        return service;
    }
    return {};
}

std::pair<std::string, std::string> identity_for_resource(const std::string& resource) {
    if (resource == "menus") return {"menu_id", "menu"};
    if (resource == "dictionaries") return {"dict_id", "dict"};
    if (resource == "dictionary_details") return {"detail_id", "detail"};
    if (resource == "system_logs") return {"log_id", "log"};
    if (resource == "role_rights") return {"right_id", "right"};
    if (resource == "users") return {"user_id", "user"};
    if (resource == "roles") return {"role_id", "role"};
    if (resource == "sites") return {"site_id", "site"};
    if (resource == "products") return {"fund_id", "fund"};
    if (resource == "projects") return {"project_id", "project"};
    if (resource == "accounts") return {"account_id", "account"};
    if (resource == "fund_project_links") return {"link_id", "link"};
    if (resource == "risk_rules") return {"rule_id", "rule"};
    if (resource == "risk_parameters") return {"parameter_id", "parameter"};
    if (resource == "risk_events") return {"event_id", "event"};
    return {};
}

std::string padded(int value, int width = 3) {
    std::ostringstream output;
    output << std::setw(width) << std::setfill('0') << value;
    return output.str();
}

MockStore make_store() {
    MockStore store;
    for (int index = 1; index <= 40; ++index) {
        const std::string status = index % 11 == 0 ? "pending" : (index % 7 == 0 ? "frozen" : "active");  // Mock 状态覆盖待审核与冻结，前端筛选可验证真实 IPC/LBM 回包而不是仅在 Python 容器中演示。
        store.resources["users"].push_back({
            {"user_id", "user-" + padded(index)},
            {"user_name", index == 1 ? "yangjh" : "quant.user" + padded(index, 2)},
            {"display_name", index == 1 ? "Yang JH" : "Quant User " + padded(index, 2)},
            {"status", status},
            {"role_ids", "role-" + padded((index - 1) % 8 + 1)},
            {"site_ids", "site-" + padded((index - 1) % 6 + 1)},
        });
    }
    for (int index = 1; index <= 8; ++index) {
        store.resources["roles"].push_back({
            {"role_id", "role-" + padded(index)}, {"role_name", "Mock Role " + padded(index, 2)},
            {"status", index == 8 ? "frozen" : "active"}, {"permission_count", std::to_string(6 + index)},
        });
    }
    for (int index = 1; index <= 6; ++index) {
        store.resources["sites"].push_back({
            {"site_id", "site-" + padded(index)}, {"site_name", "Mock Site " + padded(index, 2)},
            {"ip_policy", index % 2 == 0 ? "allowlist" : "internal"}, {"multi_login", index % 2 == 0 ? "false" : "true"},
        });
    }
    for (int index = 1; index <= 20; ++index) {
        store.resources["menus"].push_back({
            {"menu_id", "menu-" + padded(index)}, {"menu_name", "Mock Menu " + padded(index, 2)},
            {"parent_id", index <= 4 ? "root" : "menu-" + padded((index - 1) / 4)},
            {"enabled", index % 9 == 0 ? "false" : "true"},
        });
    }
    for (int index = 1; index <= 8; ++index) {
        store.resources["dictionaries"].push_back({
            {"dict_id", "dict-" + padded(index)}, {"dict_name", "Mock Dictionary " + padded(index, 2)},
            {"category", index % 2 == 0 ? "system" : "risk"}, {"enabled", "true"}, {"detail_count", "4"},
        });
        for (int detail = 1; detail <= 4; ++detail) {
            const int detail_index = (index - 1) * 4 + detail;
            store.resources["dictionary_details"].push_back({
                {"detail_id", "detail-" + padded(detail_index)}, {"dict_id", "dict-" + padded(index)},
                {"value", "V" + padded(detail)}, {"label", "Mock Value " + padded(detail)},
                {"sort", std::to_string(detail)}, {"enabled", "true"},
            });
        }
    }
    for (int index = 1; index <= 32; ++index) {
        store.resources["role_rights"].push_back({
            {"right_id", "right-" + padded(index)}, {"role_id", "role-" + padded((index - 1) % 8 + 1)},
            {"menu_id", "menu-" + padded((index - 1) % 20 + 1)}, {"permission", index % 3 == 0 ? "write" : "read"},
            {"enabled", index % 11 == 0 ? "false" : "true"},
        });
    }
    for (int index = 1; index <= 12; ++index) {
        store.resources["products"].push_back({
            {"fund_id", "fund-" + padded(index)}, {"fund_name", "Mock Fund " + padded(index, 2)},
            {"status", index % 6 == 0 ? "paused" : "active"}, {"project_ids", "project-" + padded((index - 1) * 2 + 1)},
        });
    }
    for (int index = 1; index <= 24; ++index) {
        store.resources["projects"].push_back({
            {"project_id", "project-" + padded(index)}, {"project_name", "Mock Project " + padded(index, 2)},
            {"fund_id", "fund-" + padded((index - 1) / 2 + 1)}, {"status", index % 9 == 0 ? "paused" : "active"},
        });
        store.resources["fund_project_links"].push_back({
            {"link_id", "link-" + padded(index)}, {"fund_id", "fund-" + padded((index - 1) / 2 + 1)},
            {"project_id", "project-" + padded(index)}, {"allocation", std::to_string(4 + index % 8) + "%"}, {"status", "active"},
        });
    }
    for (int index = 1; index <= 18; ++index) {
        store.resources["accounts"].push_back({
            {"account_id", "account-" + padded(index)}, {"account_name", "Mock Account " + padded(index, 2)},
            {"fund_id", "fund-" + padded((index - 1) % 12 + 1)}, {"available", std::to_string(1000000 + index * 25000)}, {"currency", "CNY"},
        });
    }
    for (int index = 1; index <= 15; ++index) {
        store.resources["risk_parameters"].push_back({
            {"parameter_id", "parameter-" + padded(index)}, {"parameter_name", "Mock Parameter " + padded(index, 2)},
            {"value", std::to_string(index * 10)}, {"unit", "%"}, {"scope", index % 2 == 0 ? "fund" : "account"}, {"updated_by", "yangjh"},
        });
        store.resources["risk_rules"].push_back({
            {"rule_id", "rule-" + padded(index)}, {"rule_name", "Mock Risk Rule " + padded(index, 2)},
            {"enabled", index % 5 == 0 ? "false" : "true"}, {"limit", std::to_string(100 + index * 5)}, {"unit", "CNY"}, {"scope", "account"},
        });
    }
    for (int index = 1; index <= 40; ++index) {
        const char* level = index % 11 == 0 ? "ERROR" : (index % 4 == 0 ? "WARN" : "INFO");
        store.resources["risk_events"].push_back({
            {"event_id", "event-" + padded(index)}, {"event_time", "2026-08-24 10:" + padded(index % 60, 2)},
            {"rule_id", "rule-" + padded((index - 1) % 15 + 1)}, {"level", level},
            {"message", "Mock risk event " + padded(index)}, {"handled", index % 3 == 0 ? "true" : "false"},
        });
        store.resources["system_logs"].push_back({
            {"log_id", "log-" + padded(index)}, {"event_time", "2026-08-24 09:" + padded(index % 60, 2)},
            {"operator", index % 5 == 0 ? "system" : "yangjh"}, {"level", level},
            {"event", "Mock audit event " + padded(index)}, {"trace_id", "mock-trace-" + padded(index)},
        });
    }
    return store;
}

MockStore& store() {
    static MockStore instance = make_store();  // MockBackend 固定一个 Worker，因此 LBM 的进程内可变数据能保持 CRUD 前后的一致视图。
    return instance;
}

void append_audit(const std::string& event) {
    Records& logs = store().resources["system_logs"];
    logs.insert(logs.begin(), {
        {"log_id", "log-" + padded(static_cast<int>(logs.size() + 1))}, {"event_time", "2026-08-24 10:30:00"},
        {"operator", "yangjh"}, {"level", "INFO"}, {"event", event}, {"trace_id", "mock-mutation"},
    });
}

MockResult failure(std::string code, std::string message, bool unauthorized = false) {
    MockResult result;
    result.response_code = unauthorized ? oxmbs::lbm::LbmResponseCode::Rejected : oxmbs::lbm::LbmResponseCode::Failed;
    result.app_status = unauthorized ? "unauthorized" : "failed";
    result.error_code = std::move(code);
    result.error_message = std::move(message);
    return result;
}

bool matches_filter(const Record& row, const std::string& status, const std::string& query) {
    if (!status.empty()) {
        const auto status_value = row.find("status");
        const auto level_value = row.find("level");
        const auto enabled_value = row.find("enabled");
        if ((status_value == row.end() || status_value->second != status) &&
            (level_value == row.end() || level_value->second != status) &&
            (enabled_value == row.end() || enabled_value->second != (status == "enabled" ? "true" : "false"))) {
            return false;
        }
    }
    if (!query.empty()) {
        std::string haystack;
        for (const auto& [key, value] : row) {
            haystack += key + "=" + value + ";";
        }
        if (lower_copy(haystack).find(lower_copy(query)) == std::string::npos) {
            return false;
        }
    }
    return true;
}

MockResult handle_page(const std::string& resource, const Form& form) {
    const auto found = store().resources.find(resource);
    if (found == store().resources.end()) {
        return failure("service_error", "mock resource is not configured");
    }
    Records filtered;
    const std::string status = form_value(form, "status");
    const std::string query = form_value(form, "query");
    for (const Record& row : found->second) {
        if (matches_filter(row, status, query)) {
            filtered.push_back(row);
        }
    }
    const int page = parse_positive_int(form_value(form, "page"), 1);
    const int page_size = std::min(kMaxMockPageSize, static_cast<std::size_t>(parse_positive_int(form_value(form, "page_size"), 10)));
    const std::size_t start = std::min(filtered.size(), static_cast<std::size_t>(page - 1) * static_cast<std::size_t>(page_size));
    const std::size_t end = std::min(filtered.size(), start + static_cast<std::size_t>(page_size));
    MockResult result;
    result.payload_json = "{\"items\":" + json_items(filtered, start, end) + ",\"page\":" + std::to_string(page) +
                          ",\"page_size\":" + std::to_string(page_size) + ",\"total\":" + std::to_string(filtered.size()) +
                          ",\"source\":\"mock-lbm\"}";
    return result;
}

MockResult handle_login(const Form& form) {
    const std::string user_name = form_value(form, "user_name").empty() ? "yangjh" : form_value(form, "user_name");
    const std::string password = form_value(form, "password");
    if (!password.empty() && password != "mock-password") {
        return failure("unauthorized", "mock password is invalid", true);
    }
    for (const Record& user : store().resources["users"]) {
        if (user.at("user_name") == user_name) {
            if (user.at("status") != "active") {
                return failure("unauthorized", "mock user is not active", true);
            }
            append_audit("Mock login succeeded for " + user_name);
            MockResult result;
            result.payload_json = "{\"session_id\":\"mock-session-" + json_escape(user.at("user_id")) +
                                  "\",\"user\":" + json_record(user) + ",\"source\":\"mock-lbm\"}";
            return result;
        }
    }
    return failure("unauthorized", "mock user was not found", true);
}

MockResult handle_create(const std::string& resource, const Form& form) {
    const auto identity = identity_for_resource(resource);
    if (identity.first.empty()) {
        return failure("service_error", "mock resource has no CRUD identity");
    }
    Record created;
    for (const auto& [key, value] : form) {
        constexpr std::string_view prefix{"record."};
        if (key.compare(0, prefix.size(), prefix) == 0) {
            created[key.substr(prefix.size())] = value;
        }
    }
    if (created.empty()) {
        return failure("service_error", "create requires record fields");
    }
    Records& rows = store().resources[resource];
    created[identity.first] = identity.second + "-" + padded(static_cast<int>(rows.size() + 1));  // 主键由 Mock LBM 生成，客户端只能提交可编辑字段。
    if (resource == "risk_rules" || resource == "menus" || resource == "dictionaries" || resource == "dictionary_details" || resource == "role_rights") {
        created.emplace("enabled", "true");
    } else if (resource == "risk_events") {
        created.emplace("handled", "false");
    } else {
        created.emplace("status", "active");
    }
    rows.push_back(created);
    append_audit("Mock create " + resource + " " + created[identity.first]);
    MockResult result;
    result.payload_json = "{\"item\":" + json_record(created) + ",\"message\":\"mock record created\",\"source\":\"mock-lbm\"}";
    return result;
}

MockResult handle_update(const std::string& resource, const Form& form) {
    const auto identity = identity_for_resource(resource);
    const std::string record_id = form_value(form, "record_id");
    if (identity.first.empty() || record_id.empty()) {
        return failure("service_error", "update requires a configured record_id");
    }
    for (Record& row : store().resources[resource]) {
        if (row[identity.first] != record_id) {
            continue;
        }
        for (const auto& [key, value] : form) {
            constexpr std::string_view prefix{"changes."};
            if (key.compare(0, prefix.size(), prefix) == 0 && key.substr(prefix.size()) != identity.first) {
                row[key.substr(prefix.size())] = value;  // Mock 层拒绝主键变更，保持与未来真实 CRUD 的标识不可变约束一致。
            }
        }
        append_audit("Mock update " + resource + " " + record_id);
        MockResult result;
        result.payload_json = "{\"item\":" + json_record(row) + ",\"message\":\"mock record updated\",\"source\":\"mock-lbm\"}";
        return result;
    }
    return failure("service_error", "mock record was not found");
}

MockResult handle_transition(const std::string& resource, const Form& form) {
    const auto identity = identity_for_resource(resource);
    const std::string record_id = form_value(form, "record_id");
    const std::string state = form_value(form, "state");
    if (identity.first.empty() || record_id.empty() || state.empty()) {
        return failure("service_error", "transition requires record_id and state");
    }
    for (Record& row : store().resources[resource]) {
        if (row[identity.first] != record_id) {
            continue;
        }
        if (row.find("enabled") != row.end()) {
            row["enabled"] = state == "enabled" ? "true" : "false";
        } else if (row.find("handled") != row.end()) {
            row["handled"] = state == "handled" ? "true" : "false";
        } else {
            row["status"] = state;
        }
        append_audit("Mock transition " + resource + " " + record_id + " -> " + state);
        MockResult result;
        result.payload_json = "{\"item\":" + json_record(row) + ",\"message\":\"mock state transitioned\",\"source\":\"mock-lbm\"}";
        return result;
    }
    return failure("service_error", "mock record was not found");
}

MockResult dispatch(const Form& form) {
    if (form_value(form, "force_unauthorized") == "true") {
        return failure("unauthorized", "mock request was denied", true);
    }
    if (form_value(form, "force_error") == "true") {
        return failure("service_error", "mock request was asked to fail");
    }
    const std::string service = form_value(form, "__mock_service");
    const std::string operation = form_value(form, "__mock_operation");
    const std::string resource = resource_for_service(service);
    if (resource.empty()) {
        return failure("service_error", "unknown mock service");
    }
    if (operation == "page") {
        return handle_page(resource, form);
    }
    if (operation == "heartbeat" && service == "system") {
        MockResult result;
        result.payload_json = "{\"status\":\"ok\",\"server_time\":\"2026-08-24T09:00:00+08:00\",\"source\":\"mock-lbm\"}";
        return result;
    }
    if (operation == "login" && service == "users") {
        return handle_login(form);
    }
    if (operation == "create") {
        return handle_create(resource, form);
    }
    if (operation == "update") {
        return handle_update(resource, form);
    }
    if (operation == "transition") {
        return handle_transition(resource, form);
    }
    return failure("service_error", "unsupported mock operation");
}

std::string encode_envelope(const Form& request, const MockResult& result) {
    // LBM 自身把客户端关联字段装入结果正文；ReplyCollector 无需依赖有竞态的 task_seq -> request_id 临时表。
    return "request_id=" + url_encode(form_value(request, "__mock_request_id")) +
           "&session_id=" + url_encode(form_value(request, "__mock_session_id")) +
           "&service=" + url_encode(form_value(request, "__mock_service")) +
           "&operation=" + url_encode(form_value(request, "__mock_operation")) +
           "&app_status=" + url_encode(result.app_status) +
           "&error_code=" + url_encode(result.error_code) +
           "&error_message=" + url_encode(result.error_message) +
           "&payload_json=" + url_encode(result.payload_json);
}

int write_response(oxmbs::lbm::LbmResponseBuffer* response,
                   oxmbs::lbm::LbmResponseCode status,
                   std::string_view payload) {
    if (response == nullptr || response->payload == nullptr || response->payload_capacity < payload.size()) {
        return 10;  // 输出缓冲不足时绝不截断 JSON；返回非零让 #8 Loader 映射成明确失败。
    }
    response->abi_version = oxmbs::lbm::kLbmAbiVersion;
    response->status = status;
    std::memcpy(response->payload, payload.data(), payload.size());
    response->payload_size = payload.size();  // 所有权仍属于 Worker 提供的 buffer，LBM 不分配跨库内存。
    return 0;
}

}  // namespace

extern "C" int oxmbs_lbm_mock_execute(const oxmbs::lbm::LbmRequestView* request,
                                        oxmbs::lbm::LbmResponseBuffer* response) {
    if (request == nullptr || request->abi_version != oxmbs::lbm::kLbmAbiVersion ||
        (request->payload == nullptr && request->payload_size != 0)) {
        return 11;  // C ABI 和借用 payload 均验证后才读取，避免动态库面对不可信结构体布局继续执行。
    }
    const Form form = parse_form(std::string_view(reinterpret_cast<const char*>(request->payload), request->payload_size));
    const auto expected_route = oxmbs::mock::resolve_mock_route(form_value(form, "__mock_service"), form_value(form, "__mock_operation"));
    MockResult result;
    if (!expected_route || expected_route.message_type != request->msg_type || expected_route.function_id != request->func_id) {
        result = failure("protocol_error", "mock route does not match the encoded service and operation");
    } else {
        result = dispatch(form);
    }
    const std::string envelope = encode_envelope(form, result);
    return write_response(response, result.response_code, envelope);  // LBM 业务状态映射到 #7 IPC Response，客户端状态仍保留在 envelope 内。
}
