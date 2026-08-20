#pragma once

#include <cstdint>
#include <unordered_set>

namespace oxmbs::routing {

// 低频白名单由部署配置或真实服务枚举注入，避免在基础设施层猜测登录、查询和管理消息的正式数值。
class MessageRoutePolicy {
public:
    MessageRoutePolicy() = default;
    explicit MessageRoutePolicy(std::unordered_set<std::uint32_t> query_or_manage_message_types);

    bool is_query_or_manage_msg(std::uint32_t msg_type) const noexcept;  // 对应 Mentor 所称 IsQueryOrManageMsg；只有显式白名单才可进入 IPC。

private:
    std::unordered_set<std::uint32_t> query_or_manage_message_types_;  // 空集合默认全部直通，形成高频保护的保守默认行为。
};

}  // namespace oxmbs::routing
