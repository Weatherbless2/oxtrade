"""oxApp 基础壳层的状态原语。

这里保存的是可恢复的展示状态，不保存密码、Token 或其他认证秘密。
控件通过信号通知主窗口，主窗口再更新本数据类并同步视觉状态；这样
登录页、标题栏、左右面板不会各自维护一份互相矛盾的状态。
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class SessionState(str, Enum):
    """会话生命周期状态，值可直接用于日志和 Qt 信号。"""

    LOGGED_OUT = "logged_out"
    LOGGING_IN = "logging_in"
    LOGGED_IN = "logged_in"
    LOGGING_OUT = "logging_out"
    ERROR = "error"


class ThemeMode(str, Enum):
    """当前支持的主题模式；后续主题扩展可在此增加枚举值。"""

    DARK_NAVY = "dark_navy"


@dataclass
class ShellState:
    """工作台展示状态快照。

    默认值对应未登录、深蓝主题、右侧面板显示和第一个产品占位。
    ``selected_product_code`` 当前是展示文案，不代表真实合约或资金数据。
    """

    session: SessionState = SessionState.LOGGED_OUT
    primary_menu: str = "home"
    system_menu: str = "workbench"
    right_panel_visible: bool = True
    theme: ThemeMode = ThemeMode.DARK_NAVY
    selected_product: str = "沪深 300"
    selected_product_code: str = "IF 主力 · 模拟数据"
    window_maximized: bool = False
