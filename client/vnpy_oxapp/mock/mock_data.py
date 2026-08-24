"""供 #18-#21 页面复用的有业务关系 Mock 数据。"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True, slots=True)
class MockDataStore:
    """数据关系显式可读，后续可以由真实服务响应替换而不改页面字段。"""

    users: tuple[dict[str, Any], ...] = (  # 用户记录通过 role_ids/site_ids 保留后续权限页面的关联键。
        {"user_id": "u-1001", "user_name": "yangjh", "status": "active", "role_ids": ("r-admin",), "site_ids": ("site-sh",)},
        {"user_id": "u-1002", "user_name": "alice", "status": "active", "role_ids": ("r-operator",), "site_ids": ("site-sh", "site-sz")},
        {"user_id": "u-1003", "user_name": "bob", "status": "frozen", "role_ids": ("r-viewer",), "site_ids": ()},
        {"user_id": "u-1004", "user_name": "chen", "status": "pending", "role_ids": (), "site_ids": ("site-bj",)},
    )
    roles: tuple[dict[str, Any], ...] = (  # 角色只保存可展示字段和权限数量，不声称真实权限落库。
        {"role_id": "r-admin", "role_name": "系统管理员", "status": "active", "permission_count": 18},
        {"role_id": "r-operator", "role_name": "交易操作员", "status": "active", "permission_count": 9},
        {"role_id": "r-viewer", "role_name": "只读用户", "status": "active", "permission_count": 4},
    )
    sites: tuple[dict[str, Any], ...] = (  # 站点数据提供 IP 策略和多点登录展示用字段。
        {"site_id": "site-sh", "site_name": "上海办公点", "ip_policy": "10.10.0.0/16", "multi_login": False},
        {"site_id": "site-sz", "site_name": "深圳办公点", "ip_policy": "10.20.0.0/16", "multi_login": True},
        {"site_id": "site-bj", "site_name": "北京办公点", "ip_policy": "10.30.0.0/16", "multi_login": False},
    )
    menus: tuple[dict[str, Any], ...] = (  # 菜单通过 parent_id 表达树关系，enabled 表达显示开关。
        {"menu_id": "m-system", "menu_name": "系统管理", "parent_id": "", "enabled": True},
        {"menu_id": "m-user", "menu_name": "用户权限", "parent_id": "", "enabled": True},
        {"menu_id": "m-fund", "menu_name": "产品资金", "parent_id": "", "enabled": True},
        {"menu_id": "m-risk", "menu_name": "风控管理", "parent_id": "", "enabled": True},
    )
    products: tuple[dict[str, Any], ...] = (  # 产品与 project_ids 建立资金页面的上下游关系。
        {"fund_id": "fund-001", "fund_name": "量化一号", "status": "active", "project_ids": ("project-001", "project-002")},
        {"fund_id": "fund-002", "fund_name": "稳健增强", "status": "paused", "project_ids": ("project-003",)},
    )
    projects: tuple[dict[str, Any], ...] = (  # 项目反向携带 fund_id，便于页面按产品过滤。
        {"project_id": "project-001", "project_name": "A股多因子", "fund_id": "fund-001", "status": "active"},
        {"project_id": "project-002", "project_name": "指数增强", "fund_id": "fund-001", "status": "active"},
        {"project_id": "project-003", "project_name": "低波动组合", "fund_id": "fund-002", "status": "paused"},
    )
    accounts: tuple[dict[str, Any], ...] = (  # 资金账号只提供演示金额和币种，不连接真实账户。
        {"account_id": "acct-001", "account_name": "测试资金账号一", "fund_id": "fund-001", "available": 1_200_000.0, "currency": "CNY"},
        {"account_id": "acct-002", "account_name": "测试资金账号二", "fund_id": "fund-002", "available": 680_000.0, "currency": "CNY"},
    )
    risk_rules: tuple[dict[str, Any], ...] = (  # 风控规则用 enabled/limit/unit/scope 表达页面所需的静态字段。
        {"rule_id": "risk-001", "rule_name": "单笔限额", "enabled": True, "limit": 500_000, "unit": "CNY", "scope": "account"},
        {"rule_id": "risk-002", "rule_name": "撤单频率", "enabled": True, "limit": 20, "unit": "次/分钟", "scope": "user"},
        {"rule_id": "risk-003", "rule_name": "自成交检查", "enabled": True, "limit": 0, "unit": "次", "scope": "account"},
        {"rule_id": "risk-004", "rule_name": "紧急停止", "enabled": False, "limit": 0, "unit": "开关", "scope": "system"},
    )

    def resource(self, name: str) -> tuple[dict[str, Any], ...]:
        value = getattr(self, name, None)  # 路由按资源名动态取快照字段。
        if not isinstance(value, tuple):  # 未知资源不能返回 None 或任意对象，必须走统一 KeyError。
            raise KeyError(f"unknown mock resource: {name}")
        return value  # 返回不可变 tuple，调用方自行复制后筛选，数据源保持只读。
