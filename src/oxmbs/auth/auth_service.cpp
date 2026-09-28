#include "oxmbs/auth/auth_service.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace oxmbs::auth {
namespace {

using Clock = std::chrono::system_clock;

bool constant_time_equal(const std::string& left, const std::string& right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char different = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        different |= static_cast<unsigned char>(left[index] ^ right[index]);
    }
    return different == 0;
}

std::string hex_encode(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return stream.str();
}

}  // namespace

AuthService::AuthService(AuthServiceOptions options)
    : options_(std::move(options)) {
    if (options_.challenge_ttl <= std::chrono::seconds::zero() ||
        options_.session_ttl <= std::chrono::seconds::zero() ||
        options_.max_sessions_per_user == 0) {
        throw std::invalid_argument("AuthService options must be positive");
    }
}

AuthChallengeResponse AuthService::issue_challenge(const AuthChallengeRequest& request) {
    AuthChallengeResponse response;
    if (!non_empty(request.username) || !non_empty(request.client_nonce)) {
        response.status = AuthStatus::InvalidArgument;
        response.error = "username and client_nonce are required";
        return response;
    }
    const auto now = Clock::now();
    Challenge challenge{
        request.username,
        request.client_nonce,
        random_token(16),
        now + options_.challenge_ttl,
    };
    response.status = AuthStatus::Ok;
    response.challenge_id = random_token(16);
    response.server_nonce = challenge.server_nonce;
    response.expires_at = challenge.expires_at;
    {
        std::lock_guard lock(mutex_);
        challenges_[response.challenge_id] = std::move(challenge);
    }
    return response;
}

AuthLoginResponse AuthService::login(const AuthLoginRequest& request) {
    AuthLoginResponse response;
    if (!non_empty(request.username) || !non_empty(request.challenge_id) ||
        !non_empty(request.client_nonce) || !non_empty(request.password_proof)) {
        response.status = AuthStatus::InvalidArgument;
        response.error = "username, challenge_id, client_nonce and password_proof are required";
        return response;
    }

    Challenge challenge;
    {
        std::lock_guard lock(mutex_);
        const auto iterator = challenges_.find(request.challenge_id);
        if (iterator == challenges_.end()) {
            response.status = AuthStatus::ChallengeNotFound;
            response.error = "challenge was not found";
            return response;
        }
        challenge = iterator->second;
        challenges_.erase(iterator);  // challenge 一次性消费，重放请求不能再次登录。
    }
    const auto now = Clock::now();
    if (challenge.expires_at <= now) {
        response.status = AuthStatus::ChallengeExpired;
        response.error = "challenge has expired";
        return response;
    }
    if (challenge.username != request.username || challenge.client_nonce != request.client_nonce) {
        response.status = AuthStatus::CredentialRejected;
        response.error = "credential verification failed";
        return response;
    }
    if (!options_.verify_credential ||
        !options_.verify_credential(request.username, request.challenge_id,
                                    request.client_nonce, challenge.server_nonce,
                                    request.password_proof)) {
        response.status = AuthStatus::CredentialRejected;
        response.error = "credential verification failed";
        return response;
    }

    const auto permissions = options_.resolve_permissions
                                ? options_.resolve_permissions(request.username)
                                : std::vector<std::string>{};
    const auto access_token = random_token(32);
    const auto session_id = random_session_id();
    Session session{
        SessionSnapshot{session_id, request.username, access_token, permissions,
                        now + options_.session_ttl},
        now,
    };
    {
        std::lock_guard lock(mutex_);
        auto& user_sessions = user_sessions_[request.username];
        if (user_sessions.size() >= options_.max_sessions_per_user) {
            response.status = AuthStatus::SessionLimitReached;
            response.error = "maximum active sessions reached";
            return response;
        }
        sessions_.emplace(session_id, session);
        user_sessions.insert(session_id);
    }
    response.status = AuthStatus::Ok;
    response.session_id = session_id;
    response.access_token = access_token;
    response.expires_at = session.snapshot.expires_at;
    response.permissions = permissions;
    return response;
}

AuthRefreshResponse AuthService::refresh(std::uint64_t session_id,
                                         const std::string& access_token) {
    AuthRefreshResponse response;
    std::lock_guard lock(mutex_);
    const auto iterator = sessions_.find(session_id);
    if (iterator == sessions_.end()) {
        response.status = AuthStatus::SessionNotFound;
        response.error = "session was not found";
        return response;
    }
    if (iterator->second.snapshot.expires_at <= Clock::now()) {
        response.status = AuthStatus::SessionExpired;
        response.error = "session has expired";
        return response;
    }
    if (!constant_time_equal(iterator->second.snapshot.access_token, access_token)) {
        response.status = AuthStatus::SessionNotFound;
        response.error = "session token is invalid";
        return response;
    }

    const auto now = Clock::now();
    iterator->second.snapshot.access_token = random_token(32);
    iterator->second.snapshot.expires_at = now + options_.session_ttl;
    response.status = AuthStatus::Ok;
    response.session_id = session_id;
    response.access_token = iterator->second.snapshot.access_token;
    response.expires_at = iterator->second.snapshot.expires_at;
    response.permissions = iterator->second.snapshot.permissions;
    return response;
}

AuthStatus AuthService::logout(std::uint64_t session_id,
                               const std::string& access_token,
                               std::string& error) {
    std::lock_guard lock(mutex_);
    const auto iterator = sessions_.find(session_id);
    if (iterator == sessions_.end()) {
        error = "session was not found";
        return AuthStatus::SessionNotFound;
    }
    if (!constant_time_equal(iterator->second.snapshot.access_token, access_token)) {
        error = "session token is invalid";
        return AuthStatus::SessionNotFound;
    }
    erase_session_locked(session_id);
    error.clear();
    return AuthStatus::Ok;
}

AuthStatus AuthService::disconnect(std::uint64_t session_id, std::string& error) {
    std::lock_guard lock(mutex_);
    if (sessions_.find(session_id) == sessions_.end()) {
        error = "session was not found";
        return AuthStatus::SessionNotFound;
    }
    erase_session_locked(session_id);
    error.clear();
    return AuthStatus::Ok;
}

AuthStatus AuthService::expire_sessions() {
    const auto now = Clock::now();
    std::lock_guard lock(mutex_);
    std::vector<std::uint64_t> expired;
    for (const auto& [session_id, session] : sessions_) {
        if (session.snapshot.expires_at <= now) {
            expired.push_back(session_id);
        }
    }
    for (const auto session_id : expired) {
        erase_session_locked(session_id);
    }
    return AuthStatus::Ok;
}

AuthStatus AuthService::authorize(std::uint64_t session_id,
                                  const std::string& access_token,
                                  const std::string& permission,
                                  std::string& error) const {
    if (!non_empty(permission)) {
        error = "permission is required";
        return AuthStatus::InvalidArgument;
    }
    std::lock_guard lock(mutex_);
    const Session* session = nullptr;
    const auto status = validate_session_locked(session_id, access_token, session, error);
    if (status != AuthStatus::Ok) {
        return status;
    }
    if (!contains_permission(session->snapshot.permissions, permission)) {
        error = "permission denied";
        return AuthStatus::PermissionDenied;
    }
    error.clear();
    return AuthStatus::Ok;
}

AuthStatus AuthService::authorize_bound_session(std::uint64_t session_id,
                                                const std::string& permission,
                                                std::string& error) const {
    if (!non_empty(permission)) {
        error = "permission is required";
        return AuthStatus::InvalidArgument;
    }
    std::lock_guard lock(mutex_);
    const auto iterator = sessions_.find(session_id);
    if (iterator == sessions_.end()) {
        error = "session was not found";
        return AuthStatus::SessionNotFound;
    }
    if (iterator->second.snapshot.expires_at <= Clock::now()) {
        error = "session has expired";
        return AuthStatus::SessionExpired;
    }
    if (!contains_permission(iterator->second.snapshot.permissions, permission)) {
        error = "permission denied";
        return AuthStatus::PermissionDenied;
    }
    error.clear();
    return AuthStatus::Ok;
}

bool AuthService::snapshot(std::uint64_t session_id,
                           SessionSnapshot& result,
                           std::string& error) const {
    std::lock_guard lock(mutex_);
    const auto iterator = sessions_.find(session_id);
    if (iterator == sessions_.end()) {
        error = "session was not found";
        return false;
    }
    if (iterator->second.snapshot.expires_at <= Clock::now()) {
        error = "session has expired";
        return false;
    }
    result = iterator->second.snapshot;
    error.clear();
    return true;
}

std::size_t AuthService::active_session_count() const noexcept {
    std::lock_guard lock(mutex_);
    return sessions_.size();
}

std::string AuthService::random_token(std::size_t bytes) {
    if (bytes == 0) {
        throw std::invalid_argument("random token size must be positive");
    }
    std::vector<std::uint8_t> random(bytes);
    std::ifstream source("/dev/urandom", std::ios::binary);
    if (!source.read(reinterpret_cast<char*>(random.data()), static_cast<std::streamsize>(random.size()))) {
        throw std::runtime_error("cannot read /dev/urandom for authentication token");
    }
    return hex_encode(random);
}

std::uint64_t AuthService::random_session_id() {
    const auto token = random_token(sizeof(std::uint64_t));
    std::uint64_t value = 0;
    const auto* first = token.data();
    const auto* last = token.data() + token.size();
    const auto result = std::from_chars(first, last, value, 16);
    if (result.ec != std::errc{} || value == 0) {
        throw std::runtime_error("cannot generate non-zero session id");
    }
    return value;
}

bool AuthService::non_empty(const std::string& value) noexcept {
    return !value.empty();
}

bool AuthService::contains_permission(const std::vector<std::string>& permissions,
                                      const std::string& permission) noexcept {
    return std::find(permissions.begin(), permissions.end(), permission) != permissions.end();
}

AuthStatus AuthService::validate_session_locked(std::uint64_t session_id,
                                                const std::string& access_token,
                                                const Session*& session,
                                                std::string& error) const {
    const auto iterator = sessions_.find(session_id);
    if (iterator == sessions_.end()) {
        error = "session was not found";
        return AuthStatus::SessionNotFound;
    }
    if (iterator->second.snapshot.expires_at <= Clock::now()) {
        error = "session has expired";
        return AuthStatus::SessionExpired;
    }
    if (!constant_time_equal(iterator->second.snapshot.access_token, access_token)) {
        error = "session token is invalid";
        return AuthStatus::SessionNotFound;
    }
    session = &iterator->second;
    return AuthStatus::Ok;
}

void AuthService::erase_session_locked(std::uint64_t session_id) {
    const auto iterator = sessions_.find(session_id);
    if (iterator == sessions_.end()) {
        return;
    }
    const auto username = iterator->second.snapshot.username;
    sessions_.erase(iterator);
    const auto user_iterator = user_sessions_.find(username);
    if (user_iterator != user_sessions_.end()) {
        user_iterator->second.erase(session_id);
        if (user_iterator->second.empty()) {
            user_sessions_.erase(user_iterator);
        }
    }
}

const char* auth_status_name(AuthStatus status) noexcept {
    switch (status) {
    case AuthStatus::Ok: return "Ok";
    case AuthStatus::InvalidArgument: return "InvalidArgument";
    case AuthStatus::ChallengeExpired: return "ChallengeExpired";
    case AuthStatus::ChallengeNotFound: return "ChallengeNotFound";
    case AuthStatus::CredentialRejected: return "CredentialRejected";
    case AuthStatus::SessionNotFound: return "SessionNotFound";
    case AuthStatus::SessionExpired: return "SessionExpired";
    case AuthStatus::PermissionDenied: return "PermissionDenied";
    case AuthStatus::SessionLimitReached: return "SessionLimitReached";
    }
    return "Unknown";
}

}  // namespace oxmbs::auth
