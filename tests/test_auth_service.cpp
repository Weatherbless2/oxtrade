#include "oxmbs/auth/auth_service.h"

#include <cassert>
#include <chrono>
#include <string>

int main() {
    oxmbs::auth::AuthServiceOptions options;
    options.verify_credential = [](const std::string& username,
                                   const std::string& challenge_id,
                                   const std::string& client_nonce,
                                   const std::string& server_nonce,
                                   const std::string& proof) {
        return username == "operator" && !challenge_id.empty() && client_nonce == "client-nonce" &&
               !server_nonce.empty() && proof == "proof";
    };
    options.resolve_permissions = [](const std::string& username) {
        return username == "operator" ? std::vector<std::string>{"quote.read", "system.read"}
                                       : std::vector<std::string>{};
    };
    oxmbs::auth::AuthService service(options);

    const auto challenge = service.issue_challenge({"operator", "client-nonce"});
    assert(challenge);
    const auto login = service.login({"operator", challenge.challenge_id, "client-nonce", "proof"});
    assert(login);
    assert(login.session_id != 0 && !login.access_token.empty());
    std::string error;
    assert(service.authorize(login.session_id, login.access_token, "quote.read", error) ==
           oxmbs::auth::AuthStatus::Ok);
    assert(service.authorize(login.session_id, login.access_token, "fund.write", error) ==
           oxmbs::auth::AuthStatus::PermissionDenied);
    assert(service.logout(login.session_id, login.access_token, error) == oxmbs::auth::AuthStatus::Ok);
    assert(service.active_session_count() == 0);
    return 0;
}
