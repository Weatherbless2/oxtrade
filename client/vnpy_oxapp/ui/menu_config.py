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


# 左侧系统菜单：page_index 对应 OxAppMainWindow._build_pages 的添加顺序。
# 该映射是迁移期间的兼容约束，修改顺序时必须同步 UI 测试和主窗口挂载逻辑。
SYSTEM_MENUS: tuple[MenuItem, ...] = (
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


# 提供 O(1) 查询，供后续权限过滤、菜单高亮或配置校验复用。
PRIMARY_BY_ID = {item.item_id: item for item in PRIMARY_MENUS}
SYSTEM_BY_ID = {item.item_id: item for item in SYSTEM_MENUS}
