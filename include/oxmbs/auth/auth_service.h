#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oxmbs::auth {

enum class AuthStatus {
    Ok,
    InvalidArgument,
    ChallengeExpired,
    ChallengeNotFound,
    CredentialRejected,
    SessionNotFound,
    SessionExpired,
    PermissionDenied,
    SessionLimitReached,
};

struct AuthChallengeRequest {
    std::string username;
    std::string client_nonce;
};

struct AuthChallengeResponse {
    AuthStatus status{AuthStatus::InvalidArgument};
    std::string challenge_id;
    std::string server_nonce;
    std::chrono::system_clock::time_point expires_at{};
    std::string error;

    explicit operator bool() const noexcept { return status == AuthStatus::Ok; }
};

struct AuthLoginRequest {
    std::string username;
    std::string challenge_id;
    std::string client_nonce;
    std::string password_proof;
};

struct AuthLoginResponse {
    AuthStatus status{AuthStatus::InvalidArgument};
    std::uint64_t session_id{0};
    std::string access_token;
    std::chrono::system_clock::time_point expires_at{};
    std::vector<std::string> permissions;
    std::string error;

    explicit operator bool() const noexcept { return status == AuthStatus::Ok; }
};

using AuthRefreshResponse = AuthLoginResponse;

struct SessionSnapshot {
    std::uint64_t session_id{0};
    std::string username;
    std::string access_token;
    std::vector<std::string> permissions;
    std::chrono::system_clock::time_point expires_at{};
};

struct AuthServiceOptions {
    std::chrono::seconds challenge_ttl{30};
    std::chrono::seconds session_ttl{8 * 60 * 60};
    std::size_t max_sessions_per_user{1};
    // 凭据和权限由后续 PostgreSQL/目录适配器注入；AuthService 不直接访问数据库。
    std::function<bool(const std::string& username,
                       const std::string& challenge_id,
                       const std::string& client_nonce,
                       const std::string& server_nonce,
                       const std::string& password_proof)> verify_credential;
    std::function<std::vector<std::string>(const std::string& username)> resolve_permissions;
};

class AuthService {
public:
    explicit AuthService(AuthServiceOptions options = {});

    AuthChallengeResponse issue_challenge(const AuthChallengeRequest& request);
    AuthLoginResponse login(const AuthLoginRequest& request);
    AuthRefreshResponse refresh(std::uint64_t session_id,
                                const std::string& access_token);

    AuthStatus logout(std::uint64_t session_id, const std::string& access_token, std::string& error);
    AuthStatus disconnect(std::uint64_t session_id, std::string& error);
    AuthStatus expire_sessions();

    AuthStatus authorize(std::uint64_t session_id,
                         const std::string& access_token,
                         const std::string& permission,
                         std::string& error) const;
    // 网络层在确认 connection_id -> session_id 后使用；Token 只在登录、刷新和注销阶段出现。
    AuthStatus authorize_bound_session(std::uint64_t session_id,
                                       const std::string& permission,
                                       std::string& error) const;
    bool snapshot(std::uint64_t session_id, SessionSnapshot& result, std::string& error) const;
    std::size_t active_session_count() const noexcept;

private:
    struct Challenge {
        std::string username;
        std::string client_nonce;
        std::string server_nonce;
        std::chrono::system_clock::time_point expires_at;
    };

    struct Session {
        SessionSnapshot snapshot;
        std::chrono::system_clock::time_point created_at;
    };

    AuthServiceOptions options_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Challenge> challenges_;
    std::unordered_map<std::uint64_t, Session> sessions_;
    std::unordered_map<std::string, std::unordered_set<std::uint64_t>> user_sessions_;

    static std::string random_token(std::size_t bytes);
    static std::uint64_t random_session_id();
    static bool non_empty(const std::string& value) noexcept;
    static bool contains_permission(const std::vector<std::string>& permissions,
                                    const std::string& permission) noexcept;
    AuthStatus validate_session_locked(std::uint64_t session_id,
                                       const std::string& access_token,
                                       const Session*& session,
                                       std::string& error) const;
    void erase_session_locked(std::uint64_t session_id);
};

const char* auth_status_name(AuthStatus status) noexcept;

}  // namespace oxmbs::auth
