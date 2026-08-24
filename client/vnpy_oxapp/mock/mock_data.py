"""Mock-first 阶段的关联业务数据集。

这份数据用于页面、Mock LBM 和端到端演示，不代表任何真实账户、资金或风控执行结果。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


RESOURCE_IDENTITIES: dict[str, tuple[str, str]] = {
    "users": ("user_id", "u"),
    "roles": ("role_id", "r"),
    "sites": ("site_id", "site"),
    "menus": ("menu_id", "m"),
    "dictionaries": ("dict_id", "dict"),
    "dictionary_details": ("detail_id", "detail"),
    "role_rights": ("right_id", "rr"),
    "products": ("fund_id", "fund"),
    "projects": ("project_id", "project"),
    "accounts": ("account_id", "acct"),
    "fund_project_links": ("link_id", "link"),
    "risk_rules": ("rule_id", "risk"),
    "risk_parameters": ("parameter_id", "param"),
    "risk_events": ("event_id", "event"),
}


def _seed_roles() -> list[dict[str, Any]]:
    names = ("系统管理员", "交易操作员", "风控专员", "产品经理", "投资经理", "运营专员", "审计只读", "外部访客")
    return [
        {
            "role_id": f"r-{index:03}", "role_name": name,
            "status": "active" if index < 8 else "disabled", "permission_count": max(2, 24 - index * 3),
            "description": f"Mock {name}角色",
        }
        for index, name in enumerate(names, start=1)
    ]


def _seed_sites() -> list[dict[str, Any]]:
    cities = (("sh", "上海"), ("sz", "深圳"), ("bj", "北京"), ("hz", "杭州"), ("cd", "成都"), ("hk", "香港"))
    return [
        {
            "site_id": f"site-{code}", "site_name": f"{name}办公点", "ip_policy": f"10.{10 + index}.0.0/16",
            "multi_login": index % 2 == 0, "mac_required": index in (1, 4, 6),
            "status": "active" if index < 6 else "disabled",
        }
        for index, (code, name) in enumerate(cities, start=1)
    ]


def _seed_users() -> list[dict[str, Any]]:
    names = (
        "yangjh", "alice", "bob", "chen", "david", "emma", "frank", "grace", "helen", "ivan",
        "james", "karen", "leo", "mike", "nina", "oliver", "peter", "queen", "robin", "susan",
        "tom", "ursula", "victor", "wendy", "xavier", "yuki", "zack", "anita", "brian", "cathy",
        "doris", "eric", "fiona", "george", "harris", "irene", "jack", "kelly", "louis", "mary",
    )
    statuses = ("active", "active", "active", "frozen", "pending", "password_expired")
    site_ids = ("site-sh", "site-sz", "site-bj", "site-hz", "site-cd", "site-hk")
    return [
        {
            "user_id": f"u-{1000 + index}", "user_name": name, "display_name": f"Mock {name.title()}",
            "status": statuses[(index - 1) % len(statuses)], "role_ids": (f"r-{((index - 1) % 8) + 1:03}",),
            "site_ids": (site_ids[(index - 1) % len(site_ids)],) if index % 7 else (),
            "login_failed_count": index % 4, "last_login": f"2026-08-{(index % 20) + 1:02} 09:{index % 60:02}:00",
        }
        for index, name in enumerate(names, start=1)
    ]


def _seed_menus() -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    modules = (
        ("system", "系统管理", ("数据字典", "系统菜单", "在线日志", "服务状态")),
        ("right", "用户权限", ("用户管理", "角色管理", "站点管理", "权限配置")),
        ("fund", "产品资金", ("产品管理", "项目资产", "资金账号", "关联关系")),
        ("risk", "风控管理", ("风控参数", "风控规则", "异常事件", "审计记录")),
    )
    for module_index, (code, title, children) in enumerate(modules, start=1):
        parent_id = f"m-{code}"
        rows.append({"menu_id": parent_id, "menu_name": title, "parent_id": "", "enabled": True, "sort": module_index})
        for child_index, child in enumerate(children, start=1):
            rows.append({
                "menu_id": f"{parent_id}-{child_index}", "menu_name": child, "parent_id": parent_id,
                "enabled": not (code == "risk" and child_index == 4), "sort": child_index,
            })
    return rows


def _seed_dictionaries() -> list[dict[str, Any]]:
    names = (
        ("user-status", "用户状态", "system"), ("currency", "币种", "fund"),
        ("order-status", "委托状态", "trade"), ("risk-level", "风控级别", "risk"),
        ("fund-status", "产品状态", "fund"), ("site-status", "站点状态", "system"),
        ("permission", "权限类型", "right"), ("event-level", "事件级别", "risk"),
    )
    return [
        {
            "dict_id": f"dict-{code}", "dict_name": title, "category": category,
            "enabled": index != 3, "detail_count": 3 + index, "version": 1,
        }
        for index, (code, title, category) in enumerate(names, start=1)
    ]


def _seed_dictionary_details() -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    groups = (
        ("user-status", ("启用", "冻结", "待审核", "密码过期")), ("currency", ("人民币", "美元", "港币", "欧元")),
        ("order-status", ("已报", "部分成交", "全部成交", "已撤销")), ("risk-level", ("提示", "警告", "严重", "紧急")),
        ("fund-status", ("启用", "暂停", "清算中", "已关闭")), ("site-status", ("启用", "维护", "停用", "隔离")),
        ("permission", ("只读", "编辑", "复核", "管理员")), ("event-level", ("INFO", "WARN", "ERROR", "CRITICAL")),
    )
    for dictionary_index, (code, labels) in enumerate(groups, start=1):
        for sort, label in enumerate(labels, start=1):
            rows.append({
                "detail_id": f"detail-{dictionary_index:02}-{sort:02}", "dict_id": f"dict-{code}",
                "value": f"{code}-{sort}", "label": label, "sort": sort,
                "enabled": not (sort == 4 and dictionary_index % 3 == 0),
            })
    return rows


def _seed_logs() -> list[dict[str, Any]]:
    levels = ("INFO", "INFO", "INFO", "WARN", "ERROR")
    events = ("登录 Mock 客户端", "查询系统菜单", "刷新用户列表", "权限不足示例", "Mock 服务延迟告警")
    operators = ("yangjh", "alice", "bob", "chen")
    return [
        {
            "log_id": f"log-{index:04}", "event_time": f"2026-08-{(index % 20) + 1:02} {8 + (index % 10):02}:{index % 60:02}:00",
            "operator": operators[index % len(operators)], "level": levels[index % len(levels)],
            "event": events[index % len(events)], "trace_id": f"mock-trace-{index:04}",
        }
        for index in range(1, 61)
    ]


def _seed_role_rights() -> list[dict[str, Any]]:
    permissions = ("read", "read/write", "approve", "admin")
    return [
        {
            "right_id": f"rr-{index:03}", "role_id": f"r-{((index - 1) % 8) + 1:03}",
            "menu_id": f"m-{('system', 'right', 'fund', 'risk')[(index - 1) % 4]}",
            "permission": permissions[(index - 1) % len(permissions)], "enabled": index % 9 != 0,
        }
        for index in range(1, 33)
    ]


def _seed_products() -> list[dict[str, Any]]:
    strategies = ("量化一号", "稳健增强", "指数增强", "中性精选", "低波动组合", "套利精选")
    return [
        {
            "fund_id": f"fund-{index:03}", "fund_name": f"{strategies[(index - 1) % len(strategies)]}-{index:02}",
            "status": ("active", "active", "paused", "liquidating")[index % 4],
            "project_ids": (f"project-{index * 2 - 1:03}", f"project-{index * 2:03}"),
            "manager": ("yangjh", "alice", "chen")[index % 3], "currency": "CNY",
        }
        for index in range(1, 13)
    ]


def _seed_projects() -> list[dict[str, Any]]:
    templates = ("A股多因子", "指数增强", "期货趋势", "ETF轮动", "市场中性", "低波动")
    return [
        {
            "project_id": f"project-{index:03}", "project_name": f"{templates[(index - 1) % len(templates)]}-{index:02}",
            "fund_id": f"fund-{((index - 1) // 2) + 1:03}", "status": ("active", "active", "paused")[index % 3],
            "market": ("A股", "期货", "ETF")[index % 3],
        }
        for index in range(1, 25)
    ]


def _seed_accounts() -> list[dict[str, Any]]:
    return [
        {
            "account_id": f"acct-{index:03}", "account_name": f"Mock 资金账号 {index:02}",
            "fund_id": f"fund-{((index - 1) % 12) + 1:03}", "available": float(600_000 + index * 87_500),
            "frozen": float((index % 4) * 25_000), "currency": "CNY", "status": "active" if index % 7 else "frozen",
        }
        for index in range(1, 19)
    ]


def _seed_fund_project_links() -> list[dict[str, Any]]:
    return [
        {
            "link_id": f"link-{index:03}", "fund_id": f"fund-{((index - 1) // 2) + 1:03}",
            "project_id": f"project-{index:03}", "allocation": round(0.35 + (index % 3) * 0.15, 2),
            "status": "active" if index % 5 else "paused",
        }
        for index in range(1, 25)
    ]


def _seed_risk_rules() -> list[dict[str, Any]]:
    names = ("单笔限额", "日累计限额", "撤单频率", "自成交检查", "仓位限制", "价格偏离", "黑名单限制")
    return [
        {
            "rule_id": f"risk-{index:03}", "rule_name": f"{names[(index - 1) % len(names)]}-{index:02}",
            "enabled": index % 6 != 0, "limit": 20 if index % 3 == 0 else 100_000 * index,
            "unit": "次/分钟" if index % 3 == 0 else "CNY", "scope": ("account", "user", "fund", "system")[index % 4],
            "severity": ("INFO", "WARN", "ERROR")[index % 3],
        }
        for index in range(1, 16)
    ]


def _seed_risk_parameters() -> list[dict[str, Any]]:
    return [
        {
            "parameter_id": f"param-{index:03}", "parameter_name": f"Mock 风控参数 {index:02}",
            "value": str(100_000 * index), "unit": "CNY", "scope": ("account", "user", "fund", "system")[index % 4],
            "updated_by": ("yangjh", "alice", "chen")[index % 3], "version": 1 + index // 5,
        }
        for index in range(1, 16)
    ]


def _seed_risk_events() -> list[dict[str, Any]]:
    levels = ("INFO", "WARN", "ERROR")
    return [
        {
            "event_id": f"event-{index:03}", "event_time": f"2026-08-{(index % 20) + 1:02} 10:{index % 60:02}:30",
            "rule_id": f"risk-{((index - 1) % 15) + 1:03}", "level": levels[index % len(levels)],
            "message": f"Mock 风控事件 {index:02}", "handled": index % 4 == 0,
            "handled_by": "yangjh" if index % 4 == 0 else "",
        }
        for index in range(1, 41)
    ]


@dataclass(slots=True)
class MockDataStore:
    """可变的 Mock 业务存储，仅由服务层修改，页面只接收响应快照。"""

    users: list[dict[str, Any]] = field(default_factory=_seed_users)
    roles: list[dict[str, Any]] = field(default_factory=_seed_roles)
    sites: list[dict[str, Any]] = field(default_factory=_seed_sites)
    menus: list[dict[str, Any]] = field(default_factory=_seed_menus)
    dictionaries: list[dict[str, Any]] = field(default_factory=_seed_dictionaries)
    dictionary_details: list[dict[str, Any]] = field(default_factory=_seed_dictionary_details)
    system_logs: list[dict[str, Any]] = field(default_factory=_seed_logs)
    role_rights: list[dict[str, Any]] = field(default_factory=_seed_role_rights)
    products: list[dict[str, Any]] = field(default_factory=_seed_products)
    projects: list[dict[str, Any]] = field(default_factory=_seed_projects)
    accounts: list[dict[str, Any]] = field(default_factory=_seed_accounts)
    fund_project_links: list[dict[str, Any]] = field(default_factory=_seed_fund_project_links)
    risk_rules: list[dict[str, Any]] = field(default_factory=_seed_risk_rules)
    risk_parameters: list[dict[str, Any]] = field(default_factory=_seed_risk_parameters)
    risk_events: list[dict[str, Any]] = field(default_factory=_seed_risk_events)

    def resource(self, name: str) -> tuple[dict[str, Any], ...]:
        value = getattr(self, name, None)
        if not isinstance(value, list):
            raise KeyError(f"unknown mock resource: {name}")
        return tuple(dict(row) for row in value)  # 查询返回独立快照，页面不能通过结果修改后端 Mock 状态。

    def mutable_resource(self, name: str) -> list[dict[str, Any]]:
        value = getattr(self, name, None)
        if not isinstance(value, list):
            raise KeyError(f"unknown mock resource: {name}")
        return value  # 只有 MockServiceRouter 的 CRUD 操作持有这个可变引用。

    def next_identifier(self, resource: str) -> str:
        primary_key, prefix = RESOURCE_IDENTITIES[resource]
        rows = self.mutable_resource(resource)
        numeric_suffixes = [
            int(str(row.get(primary_key, "")).rsplit("-", 1)[-1])
            for row in rows
            if str(row.get(primary_key, "")).rsplit("-", 1)[-1].isdigit()
        ]
        return f"{prefix}-{(max(numeric_suffixes, default=0) + 1):03}"  # 不复用禁用记录 ID，模拟真实主键不可回收语义。
