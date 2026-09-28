#include "oxmbs/auth/auth_credential_store.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace oxmbs::auth {
namespace {

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::vector<std::string> split(std::string_view value, char separator) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(separator, begin);
        parts.push_back(trim(value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin)));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return parts;
}

bool hex_digest(std::string_view value) noexcept {
    if (value.size() != 64) {
        return false;
    }
    for (const char character : value) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        const bool upper = character >= 'A' && character <= 'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

std::string sha256_hex(std::string_view input) {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        return {};
    }
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digest_size = 0;
    const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                    EVP_DigestUpdate(context, input.data(), input.size()) == 1 &&
                    EVP_DigestFinal_ex(context, digest, &digest_size) == 1;
    EVP_MD_CTX_free(context);
    if (!ok) {
        return {};
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digest_size; ++index) {
        output << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return output.str();
}

bool constant_time_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

}  // namespace

bool EnvironmentCredentialStore::load_from_environment(const char* variable_name, std::string& error) {
    records_.clear();
    if (variable_name == nullptr || *variable_name == '\0') {
        error = "credential environment variable name is empty";
        return false;
    }
    const char* raw = std::getenv(variable_name);
    if (raw == nullptr || *raw == '\0') {
        return true;  // 未配置时保持空存储，服务仍可启动，但所有登录都会被拒绝。
    }
    for (const auto& record_text : split(raw, ';')) {
        if (record_text.empty()) {
            continue;
        }
        const auto fields = split(record_text, '|');
        if (fields.size() != 3 || fields[0].empty() || !hex_digest(fields[1])) {
            error = "OXMBS_AUTH_USERS contains an invalid credential record";
            records_.clear();
            return false;
        }
        if (records_.find(fields[0]) != records_.end()) {
            error = "OXMBS_AUTH_USERS contains duplicate usernames";
            records_.clear();
            return false;
        }
        AuthCredentialRecord record;
        record.password_digest = fields[1];
        for (const auto& permission : split(fields[2], ',')) {
            if (!permission.empty()) {
                record.permissions.push_back(permission);
            }
        }
        records_.emplace(fields[0], std::move(record));
    }
    return true;
}

bool EnvironmentCredentialStore::verify_credential(const std::string& username,
                                                   const std::string& challenge_id,
                                                   const std::string& client_nonce,
                                                   const std::string& server_nonce,
                                                   const std::string& password_proof) const noexcept {
    const auto iterator = records_.find(username);
    if (iterator == records_.end()) {
        return false;
    }
    const auto password_digest = iterator->second.password_digest;
    std::string input;
    input.reserve(username.size() + password_digest.size() + challenge_id.size() +
                  client_nonce.size() + server_nonce.size() + 4);
    input.append(username).append(":").append(password_digest).append(":").append(challenge_id)
        .append(":").append(client_nonce).append(":").append(server_nonce);
    const auto expected = sha256_hex(input);
    return !expected.empty() && constant_time_equal(expected, password_proof);
}

std::vector<std::string> EnvironmentCredentialStore::resolve_permissions(const std::string& username) const {
    const auto iterator = records_.find(username);
    return iterator == records_.end() ? std::vector<std::string>{} : iterator->second.permissions;
}

std::string make_password_proof(std::string_view username,
                                std::string_view password,
                                std::string_view challenge_id,
                                std::string_view client_nonce,
                                std::string_view server_nonce) {
    const auto password_digest = sha256_hex(password);
    std::string input;
    input.reserve(username.size() + password_digest.size() + challenge_id.size() +
                  client_nonce.size() + server_nonce.size() + 4);
    input.append(username).append(":").append(password_digest).append(":").append(challenge_id)
        .append(":").append(client_nonce).append(":").append(server_nonce);
    return sha256_hex(input);
}

}  // namespace oxmbs::auth
