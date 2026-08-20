#include "oxmbs/routing/message_route_policy.h"

#include <utility>

namespace oxmbs::routing {

MessageRoutePolicy::MessageRoutePolicy(std::unordered_set<std::uint32_t> query_or_manage_message_types)
    : query_or_manage_message_types_(std::move(query_or_manage_message_types)) {
}

bool MessageRoutePolicy::is_query_or_manage_msg(std::uint32_t msg_type) const noexcept {
    // 0 是协议层非法类型，且不允许因为空集合或意外默认值落入低频 IPC。
    return msg_type != 0 && query_or_manage_message_types_.find(msg_type) != query_or_manage_message_types_.end();
}

}  // namespace oxmbs::routing
