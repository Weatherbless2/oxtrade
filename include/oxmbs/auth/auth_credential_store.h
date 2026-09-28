#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace oxmbs::auth {

// 运行时凭据记录只保存密码摘要，不保存明文密码；记录来自 OXMBS_AUTH_USERS 环境变量。
struct AuthCredentialRecord {
    std::string password_digest;
    std::vector<std::string> permissions;
};

class EnvironmentCredentialStore {
public:
    // 格式：user|sha256(password)|permission.a,permission.b;another|sha256|permission.c
    bool load_from_environment(const char* variable_name, std::string& error);

    bool verify_credential(const std::string& username,
                           const std::string& challenge_id,
                           const std::string& client_nonce,
                           const std::string& server_nonce,
                           const std::string& password_proof) const noexcept;

    std::vector<std::string> resolve_permissions(const std::string& username) const;
    bool empty() const noexcept { return records_.empty(); }

private:
    std::unordered_map<std::string, AuthCredentialRecord> records_;
};

// 客户端和服务端必须对同一组字段计算相同的 challenge proof。
std::string make_password_proof(std::string_view username,
                                 std::string_view password,
                                 std::string_view challenge_id,
                                 std::string_view client_nonce,
                                 std::string_view server_nonce);

}  // namespace oxmbs::auth
