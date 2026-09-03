"""oxApp 壳层和业务页面共用的深蓝主题。

QSS 以控件 ``objectName`` 为主要边界：标题栏、左右面板、欢迎页和登录
页拥有独立样式，通用控件样式则覆盖现有业务表格和表单。这样可以在
不修改业务页面结构的情况下统一视觉，并为后续主题 token/多主题扩展
保留单一入口。
"""

from __future__ import annotations

try:
    from PySide6.QtWidgets import QApplication
    PYSIDE_AVAILABLE = True
except ImportError:
    # 主题模块可在无 PySide6 的核心测试中导入；真正应用样式时再检查依赖。
    PYSIDE_AVAILABLE = False


# 全局基础色、字体和通用控件样式。
DARK_NAVY_QSS = """
QMainWindow, QWidget {
    background: #0b1220;
    color: #e6eef9;
    font-family: "Microsoft YaHei UI", "Noto Sans CJK SC", sans-serif;
    font-size: 13px;
}
QLabel, QCheckBox { background: transparent; }
QWidget#shellRoot { background: #0b1220; }
QFrame#titleBar {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
        stop:0 #101f36, stop:.58 #132b48, stop:1 #102139);
    border-bottom: 1px solid #345577;
}
QWidget#titleBrand { background: transparent; }
QLabel#brandMark, QLabel#profileAvatar {
    color: #ffffff;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #2db7ee, stop:1 #1b65a5);
    border: 1px solid rgba(255,255,255,.16);
    border-radius: 9px;
    font-weight: 800;
}
QLabel#brandName { color: #f5fbff; font-size: 17px; font-weight: 700; }
QLabel#brandSubtitle, QLabel#navigationFooter { color: #8ea4be; font-size: 11px; }
QWidget#primaryNav { background: transparent; }
QPushButton#primaryNavButton {
    color: #a9bdd5;
    background: transparent;
    border: 0;
    border-bottom: 3px solid transparent;
    padding: 0 14px;
    min-height: 57px;
}
QPushButton#primaryNavButton:hover { color: #f2f8ff; background: rgba(255,255,255,.04); }
QPushButton#primaryNavButton:checked {
    color: #f5fbff;
    font-weight: 700;
    border-bottom: 3px solid #39a8e8;
}
QLineEdit#globalSearch {
    color: #e6eef9;
    background: #0a1526;
    border: 1px solid #243752;
    border-radius: 6px;
    padding: 7px 10px;
    selection-background-color: #2178b1;
}
QLineEdit#globalSearch:focus { border: 1px solid #39a8e8; }
QPushButton#titleConfigButton, QPushButton#titleThemeButton,
QPushButton#windowMinimizeButton, QPushButton#windowMaximizeButton,
QPushButton#windowCloseButton, QPushButton#panelToggleButton {
    color: #b9cbe0;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 5px;
    padding: 0;
}
QPushButton#titleConfigButton:hover, QPushButton#titleThemeButton:hover,
QPushButton#windowMinimizeButton:hover, QPushButton#windowMaximizeButton:hover,
QPushButton#panelToggleButton:hover { color: #ffffff; background: rgba(57,168,232,.13); border-color: #345577; }
QPushButton#windowCloseButton:hover { color: #ffffff; background: #c84d5b; }
QPushButton#accountButton {
    color: #cfdeef;
    background: transparent;
    border: 0;
    border-left: 1px solid #243752;
    padding: 5px 8px 5px 12px;
}
QPushButton#accountButton:hover { color: #ffffff; background: rgba(255,255,255,.04); }

# 固定三栏左右面板与导航列表样式。
QFrame#navigationPanel, QFrame#configPanel {
    background: #111c2e;
}
QFrame#navigationPanel { border-right: 1px solid #243752; }
QLabel#navigationKicker, QLabel#sectionKicker {
    color: #657b95;
    font-size: 10px;
    letter-spacing: 1px;
}
QListWidget#navigationList {
    background: transparent;
    border: 0;
    outline: 0;
    padding: 6px 0;
}
QListWidget#navigationList::item {
    color: #9eb1c7;
    padding: 11px 12px;
    margin: 2px 0;
    border: 1px solid transparent;
    border-radius: 7px;
}
QListWidget#navigationList::item:hover { color: #e9f4ff; background: rgba(255,255,255,.035); }
QListWidget#navigationList::item:selected {
    color: #f6fbff;
    background: rgba(47,145,208,.28);
    border: 1px solid rgba(57,168,232,.35);
}

# 中央工作区、欢迎页卡片和产品列表样式。
QWidget#workbenchBody, QWidget#centralWorkspace { background: #0d1728; }
QFrame#configPanel { border-left: 1px solid #243752; }
QLabel#panelTitle, QLabel#profileName { color: #e6eef9; font-weight: 700; }
QFrame#profileCard, QFrame#metricCard, QFrame#workbenchCard, QFrame#emptyBusinessCard {
    background: #101c30;
    border: 1px solid #243752;
    border-radius: 8px;
}
QLabel#profileStatus { color: #43c78a; font-size: 10px; }
QLabel#configValue, QLabel#cardMeta, QLabel#mutedLabel, QLabel#metricLabel,
QLabel#welcomeDescription { color: #8ea4be; }
QLabel#configNotice {
    color: #e8ba82;
    background: rgba(240,174,101,.08);
    border: 1px solid rgba(240,174,101,.32);
    border-radius: 6px;
    padding: 9px 10px;
    font-size: 11px;
}
QLabel#eyebrow { color: #39a8e8; letter-spacing: 2px; font-size: 11px; }
QLabel#welcomeTitle { color: #f5fbff; font-size: 25px; font-weight: 700; }
QLabel#metricValue { color: #f5fbff; font-size: 22px; font-weight: 700; }
QLabel#metricNote { color: #657b95; font-size: 11px; }
QLabel#cardTitle { color: #d9e8f7; font-size: 13px; font-weight: 700; }
QListWidget#productList {
    color: #c9d9ea;
    background: transparent;
    border: 0;
    outline: 0;
}
QListWidget#productList::item { padding: 12px 8px; border-bottom: 1px solid rgba(36,55,82,.72); }
QListWidget#productList::item:hover { background: rgba(57,168,232,.08); }
QListWidget#productList::item:selected { color: #f5fbff; background: rgba(57,168,232,.16); }
QPushButton#primaryButton, QPushButton#secondaryButton, QPushButton#smallButton,
QPushButton#logoutButton, QPushButton#configValueButton {
    border-radius: 6px;
    padding: 6px 13px;
    min-height: 18px;
}
QPushButton#primaryButton {
    color: #041b2d;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #55c8f5, stop:1 #2999d4);
    border: 1px solid #5ac6f8;
    font-weight: 700;
}
QPushButton#secondaryButton, QPushButton#smallButton, QPushButton#logoutButton, QPushButton#configValueButton {
    color: #b9cce1;
    background: #13243c;
    border: 1px solid #345577;
}
QPushButton#secondaryButton:hover, QPushButton#smallButton:hover,
QPushButton#logoutButton:hover, QPushButton#configValueButton:hover {
    color: #ffffff; background: rgba(57,168,232,.13); border-color: #39a8e8;
}
QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {
    color: #dce5eb;
    background: #0b1728;
    border: 1px solid #364d68;
    border-radius: 5px;
    padding: 6px 8px;
    min-height: 18px;
}
QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: #39a8e8; }
QComboBox QAbstractItemView { color: #dce5eb; background: #101c30; selection-background-color: #2b5575; }
QCheckBox { color: #b9cce1; spacing: 5px; }
QCheckBox::indicator { width: 32px; height: 18px; border-radius: 9px; background: #304762; }
QCheckBox::indicator:checked { background: #1e84ba; }

# 现有业务页的表格、Tab、分组框统一复用深蓝控件风格。
QTabWidget::pane, QTableWidget {
    color: #d7e0e6;
    background: #101c30;
    border: 1px solid #243752;
    gridline-color: #213550;
    selection-background-color: #294b62;
    selection-color: #ffffff;
}
QTabBar::tab { color: #96a6b1; background: #14233a; border: 1px solid #243752; padding: 8px 18px; min-width: 72px; }
QTabBar::tab:hover { color: #e5edf2; background: #1d3b56; }
QTabBar::tab:selected { color: #ffffff; background: #214562; border-bottom: 2px solid #55a8da; }
QHeaderView::section { color: #b9c7d0; background: #14233a; border: 0; border-right: 1px solid #34414a; border-bottom: 1px solid #34414a; padding: 7px 8px; font-weight: 600; }
QGroupBox { color: #dce5eb; background: #101c30; border: 1px solid #243752; border-radius: 6px; margin-top: 12px; padding: 12px 10px 8px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }
QPushButton { color: #edf4f8; background: #28475c; border: 1px solid #3e6d89; padding: 6px 14px; min-height: 18px; }
QPushButton:hover { background: #35627e; }
QPushButton:pressed { background: #1f394a; }

QStatusBar { color: #8ea4be; background: #0b1423; border-top: 1px solid #243752; }
QStatusBar::item { border: 0; }
QScrollBar:vertical { background: #0d1728; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #29415f; min-height: 28px; border-radius: 5px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }

# 未登录欢迎页使用独立的深色品牌区+浅色表单区，便于区分会话边界。
QFrame#loginPage { background: #08111f; }
QFrame#loginCard { background: #101c30; border: 1px solid #2b4c70; border-radius: 14px; }
QFrame#loginBrandPanel {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #13355b, stop:.70 #0e1c35, stop:1 #0d1728);
    border: 1px solid #2b4c70;
    border-radius: 14px 0 0 14px;
}
QFrame#loginFormPanel { background: #f1f5f9; border-radius: 0 14px 14px 0; }
QLabel#loginBrandTitle { color: #f5fbff; font-size: 30px; font-weight: 700; }
QLabel#loginBrandText { color: #9fb7d2; }
QLabel#loginVersion { color: #6d8aaa; font-size: 11px; }
QLabel#loginFormTitle { color: #172c45; font-size: 22px; font-weight: 700; }
QLabel#loginHint, QLabel#loginLabel, QLabel#loginFoot { color: #788ba0; font-size: 12px; }
QPushButton#loginToolButton { color: #7589a0; background: transparent; border: 0; border-radius: 4px; padding: 0; }
QPushButton#loginToolButton:hover { color: #1b527d; background: #dce8f2; }
QPushButton#loginTab, QPushButton#loginTabActive { color: #98a8b8; background: transparent; border: 0; border-bottom: 2px solid transparent; padding: 5px 0 9px; }
QPushButton#loginTabActive { color: #1a5c8c; font-weight: 700; border-bottom-color: #328bc0; }
QLineEdit#loginInput { color: #233b55; background: #ffffff; border: 1px solid #ccd8e4; border-radius: 6px; padding: 8px 12px; min-height: 24px; }
QLineEdit#loginInput:focus { border-color: #4ba4d3; }
QPushButton#loginSubmit { color: #ffffff; background: #287db6; border: 0; border-radius: 6px; min-height: 28px; font-weight: 700; }
QPushButton#loginSubmit:hover { background: #3fa9df; }
QPushButton#loginSubmit:disabled { color: #d4e3ef; background: #7ca8c4; }
QLabel#loginError { color: #cc4f58; min-height: 18px; }
QLabel#loginState { color: #788ba0; font-size: 11px; }
QPushButton#loginLink { color: #2d86bb; background: transparent; border: 0; padding: 0; }
"""

# 保留旧的公开名称，避免现有业务页或下游测试因主题重构而无法导入。
WORKBENCH_QSS = DARK_NAVY_QSS


def apply_workbench_theme(app: QApplication, mode: str = "dark_navy") -> None:
    """在 QApplication 级别应用工作台主题。

    ``mode`` 先保留为扩展参数；当前阶段只有 ``dark_navy``，因此不会
    根据未知值伪造其他主题。调用方可以安全地在正式入口和单元测试中
    重复调用本函数，后一次调用会覆盖应用级 QSS。
    """
    del mode  # 第一阶段只启用深蓝主题，后续新增枚举后再按模式选择 QSS。
    if not PYSIDE_AVAILABLE:
        # 无 Qt 时静默返回，让纯 Python 测试不被视觉依赖阻断。
        return
    app.setStyle("Fusion")
    app.setStyleSheet(DARK_NAVY_QSS)
