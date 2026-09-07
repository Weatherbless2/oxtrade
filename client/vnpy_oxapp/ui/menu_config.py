"""oxApp 壳层菜单配置。

菜单文本、图标和页面索引集中在这里维护，标题栏和左侧导航只消费
配置，不把业务名称硬编码到布局组件中。后续确定正式产品命名时，
优先修改本文件而不是重写窗口结构。
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class MenuItem:
    """一个纯展示菜单项。

    ``item_id`` 是跨组件传递的稳定键；``label`` 和 ``icon`` 只影响视觉；
    ``page_index`` 是当前兼容旧页面堆栈的索引，未来可替换为显式页面 ID。
    菜单配置本身不负责发请求或执行业务权限判断。
    """

    item_id: str
    label: str
    icon: str
    page_index: int | None = None


# 标题栏一级菜单：负责切换工作域，未落地的域仍显示中性的占位首页。
PRIMARY_MENUS: tuple[MenuItem, ...] = (
    MenuItem("home", "首页", "⌂"),
    MenuItem("market", "行情", "⌁"),
    MenuItem("strategy", "策略", "◇"),
    MenuItem("trade", "交易", "⇄"),
    MenuItem("research", "研究", "▤"),
    MenuItem("admin", "管理", "⚙"),
)


# 原型中的左侧系统菜单只展示六个稳定入口，避免把旧业务页平铺到壳层。
# page_index 对应 OxAppMainWindow._build_pages 的添加顺序；None 表示暂时只
# 显示欢迎占位，不伪造尚未接入的业务页面。
SYSTEM_MENUS: tuple[MenuItem, ...] = (
    MenuItem("workbench", "工作台", "⌂", 0),
    MenuItem("market", "行情中心", "⌁", 5),
    MenuItem("strategy", "策略研究", "◇", None),
    MenuItem("trade", "交易终端", "⇄", 6),
    MenuItem("products", "产品管理", "▣", 3),
    MenuItem("system", "系统管理", "⚙", 1),
)


# 迁移期间保留旧页面索引的兼容配置。它不直接渲染到新左栏，只供旧测试
# 和尚未迁移的外部调用通过 NavigationPanel.compatibility_list_widget 访问。
LEGACY_SYSTEM_MENUS: tuple[MenuItem, ...] = (
    MenuItem("workbench", "工作台", "⌂", 0),
    MenuItem("system", "系统管理", "⚙", 1),
    MenuItem("rights", "用户权限", "◇", 2),
    MenuItem("funds", "产品资金", "▣", 3),
    MenuItem("risk", "风控管理", "△", 4),
    MenuItem("market", "行情中心", "⌁", 5),
    MenuItem("trade", "交易终端", "⇄", 6),
    MenuItem("session", "登录会话", "●", 7),
    MenuItem("logs", "运行日志", "≡", 8),
)


# 一级菜单下的二级上下文导航。显示文字可以在业务命名确认后调整，
# 但 item_id 保持稳定，避免把页面路由绑定到文案。
SECONDARY_MENUS: dict[str, tuple[MenuItem, ...]] = {
    "home": (
        MenuItem("overview", "总览", "·"),
        MenuItem("watchlist", "自选产品", "·"),
        MenuItem("recent", "最近访问", "·"),
    ),
    "market": (
        MenuItem("market_overview", "行情总览", "·"),
        MenuItem("market_watchlist", "自选合约", "·"),
        MenuItem("market_alerts", "行情预警", "·"),
    ),
    "strategy": (
        MenuItem("strategy_overview", "策略总览", "·"),
        MenuItem("strategy_research", "策略研究", "·"),
        MenuItem("strategy_runs", "运行记录", "·"),
    ),
    "trade": (
        MenuItem("trade_overview", "交易总览", "·"),
        MenuItem("trade_orders", "委托管理", "·"),
        MenuItem("trade_positions", "持仓明细", "·"),
    ),
    "research": (
        MenuItem("research_overview", "研究总览", "·"),
        MenuItem("research_models", "模型库", "·"),
        MenuItem("research_backtests", "回测记录", "·"),
    ),
    "admin": (
        MenuItem("admin_overview", "管理总览", "·"),
        MenuItem("admin_users", "用户与权限", "·"),
        MenuItem("admin_system", "系统设置", "·"),
    ),
}


# 提供 O(1) 查询，供后续权限过滤、菜单高亮或配置校验复用。
PRIMARY_BY_ID = {item.item_id: item for item in PRIMARY_MENUS}
SYSTEM_BY_ID = {item.item_id: item for item in SYSTEM_MENUS}
LEGACY_SYSTEM_BY_ID = {item.item_id: item for item in LEGACY_SYSTEM_MENUS}
