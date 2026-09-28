"""oxApp 壳层和业务页面共用的主题 token。

QSS 以控件 ``objectName`` 为主要边界：标题栏、左右面板、欢迎页和登录
页拥有独立样式，通用控件样式则覆盖现有业务表格和表单。这样可以在
不修改业务页面结构的情况下统一视觉，并通过单一入口切换主题。
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
QWidget#titleBar {
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
QWidget#secondaryNav {
    background: #101b2e;
    border-bottom: 1px solid #243752;
}
QPushButton#secondaryNavButton {
    color: #8ea4be;
    background: transparent;
    border: 0;
    border-bottom: 2px solid transparent;
    padding: 0;
    min-height: 44px;
}
QPushButton#secondaryNavButton:hover { color: #e6eef9; }
QPushButton#secondaryNavButton:checked {
    color: #eef8ff;
    font-weight: 700;
    border-bottom-color: #39a8e8;
}
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

/* 固定三栏左右面板与导航列表样式。 */
QFrame#navigationPanel, QFrame#configPanel {
    background: #111c2e;
}
QFrame#navigationPanel { border-right: 1px solid #243752; }
QFrame#navigationHeader { background: transparent; }
QPushButton#navigationToggleButton {
    color: #9eb1c7;
    background: #13243c;
    border: 1px solid #345577;
    border-radius: 5px;
    padding: 0;
}
QPushButton#navigationToggleButton:hover { color: #f6fbff; border-color: #39a8e8; background: rgba(57,168,232,.13); }
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
QListWidget#navigationList[compact="true"]::item { padding-left: 0; padding-right: 0; }

/* 中央工作区、欢迎页卡片和产品列表样式。 */
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
QListWidget#productList::item { padding: 0; border: 0; }
QFrame#productRow { background: transparent; border-bottom: 1px solid rgba(36,55,82,.72); border-radius: 4px; }
QFrame#productRow:hover { background: rgba(57,168,232,.08); }
QFrame#productRow[selected="true"] { background: rgba(57,168,232,.16); }
QLabel#productName { color: #c9d9ea; font-size: 12px; }
QLabel#productCode { color: #657b95; font-size: 10px; }
QLabel#productChange { color: #43c78a; font-size: 12px; }
QLabel#productChange[trend="down"] { color: #ef747e; }
QLabel#cardStatus { color: #43c78a; font-size: 11px; }
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
QCheckBox#loginOption { color: #70859b; spacing: 5px; }
QCheckBox#loginOption::indicator { width: 14px; height: 14px; border-radius: 3px; background: #ffffff; border: 1px solid #b9c8d7; }
QCheckBox#loginOption::indicator:hover { border-color: #4ba4d3; }
QCheckBox#loginOption::indicator:checked { background: #2d91c7; border-color: #2d91c7; }
QCheckBox#configSwitch { color: #b9cce1; spacing: 6px; }

/* 现有业务页的表格、Tab、分组框统一复用深蓝控件风格。 */
QTabWidget::pane, QTableWidget {
    color: #d7e0e6;
    background: #101c30;
    border: 1px solid #243752;
    gridline-color: #213550;
    selection-background-color: #294b62;
    selection-color: #ffffff;
}
QFrame#summaryBar, QFrame#marketSummaryBar { background: transparent; }
QFrame#marketMetricCard, QFrame#marketDetailPanel {
    background: #101c30;
    border: 1px solid #243752;
    border-radius: 7px;
}
QLabel#marketMetricLabel, QLabel#marketDetailLabel,
QLabel#marketDetailSection, QLabel#marketPageMeta { color: #8ea4be; }
QLabel#marketMetricLabel { font-size: 11px; }
QLabel#marketMetricValue { color: #f5fbff; font-size: 21px; font-weight: 700; }
QLabel#marketMetricNote { color: #657b95; font-size: 10px; }
QLabel#marketMetricNote[status="ok"] { color: #43c78a; }
QLabel#marketDetailTitle { color: #39a8e8; font-size: 15px; font-weight: 700; }
QLabel#marketDetailSubtitle { color: #8ea4be; font-size: 11px; }
QLabel#marketDetailPrice { color: #f5fbff; font-size: 19px; font-weight: 700; }
QLabel#marketDetailChange { color: #8ea4be; font-size: 11px; }
QLabel#marketDetailChange[trend="up"] { color: #43c78a; }
QLabel#marketDetailChange[trend="down"] { color: #ef747e; }
QLabel#marketDetailValue { color: #e6eef9; font-size: 12px; }
QLabel#marketDetailEmpty { color: #657b95; font-size: 11px; }
QLabel#tableSummary { color: #8ea4be; font-size: 11px; }
QTableWidget::item { background: #101c30; border-bottom: 1px solid #1e344d; padding: 4px 6px; }
QTableWidget::item:hover { background: #162d45; }
QTableWidget::item:selected { background: #183f5d; color: #f5fbff; }
QHeaderView::section { min-height: 26px; }
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
QStatusBar QLabel { padding: 0 5px; }
QLabel#statusConnection { color: #43c78a; }
QLabel#statusConnection[connected="false"] { color: #f0ae65; }
QScrollBar:vertical { background: #0d1728; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #29415f; min-height: 28px; border-radius: 5px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }

/* 未登录欢迎页与工作台使用同一套深色蓝灰 token，避免出现突兀的白色表单块。 */
QFrame#loginPage { background: #08111f; }
QFrame#loginCard { background: #101c30; border: 1px solid #2b4c70; border-radius: 14px; }
QFrame#loginBrandPanel {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #13355b, stop:.70 #0e1c35, stop:1 #0d1728);
    border: 1px solid #2b4c70;
    border-radius: 14px 0 0 14px;
}
QFrame#loginFormPanel { background: #102a43; border-radius: 0 14px 14px 0; }
QLabel#loginBrandTitle { color: #f5fbff; font-size: 30px; font-weight: 700; }
QLabel#loginBrandText { color: #9fb7d2; }
QLabel#loginVersion { color: #6d8aaa; font-size: 11px; }
QLabel#loginFormTitle { color: #f5fbff; font-size: 22px; font-weight: 700; }
QLabel#loginHint, QLabel#loginLabel, QLabel#loginFoot { color: #9eb3c4; font-size: 12px; }
QPushButton#loginToolButton { color: #9eb3c4; background: transparent; border: 0; border-radius: 4px; padding: 0; }
QPushButton#loginToolButton:hover { color: #f5fbff; background: rgba(70,167,255,.1); }
QPushButton#loginTab, QPushButton#loginTabActive { color: #8ea4be; background: transparent; border: 0; border-bottom: 2px solid transparent; padding: 5px 0 9px; }
QPushButton#loginTabActive { color: #e8f1fa; font-weight: 700; border-bottom-color: #46a7ff; }
QLineEdit#loginInput { color: #e8f1fa; background: #0d2338; border: 1px solid #35536c; border-radius: 6px; padding: 8px 12px; min-height: 24px; selection-background-color: #285d84; }
QLineEdit#loginInput:focus { border-color: #46a7ff; }
QPushButton#loginSubmit { color: #041423; background: #46a7ff; border: 1px solid #69baff; border-radius: 6px; min-height: 28px; font-weight: 700; }
QPushButton#loginSubmit:hover { background: #69baff; }
QPushButton#loginSubmit:disabled { color: #d4e3ef; background: #527b9a; }
QLabel#loginError { color: #f07878; min-height: 18px; }
QLabel#loginState { color: #9eb3c4; font-size: 11px; }
QPushButton#loginLink { color: #8fc4ee; background: transparent; border: 0; padding: 0; }
"""

# 浅色主题复用同一套 objectName，只覆盖颜色、边框和控件表面，保证切换
# 时布局、选中态和业务页面状态都不会重建或丢失。
LIGHT_QSS = DARK_NAVY_QSS + """
QMainWindow, QWidget { background: #f3f6fa; color: #142536; }
QWidget#shellRoot, QWidget#workbenchBody, QWidget#centralWorkspace { background: #f3f6fa; }
QWidget#titleBar { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #ffffff,stop:.58 #eef4fb,stop:1 #ffffff); border-bottom-color: #b8c8d9; }
QLabel#brandName, QLabel#welcomeTitle, QLabel#metricValue, QLabel#cardTitle, QLabel#panelTitle, QLabel#profileName { color: #142536; }
QLabel#brandSubtitle, QLabel#navigationFooter, QLabel#navigationKicker, QLabel#sectionKicker,
QLabel#configValue, QLabel#cardMeta, QLabel#mutedLabel, QLabel#metricLabel, QLabel#welcomeDescription { color: #5c6c7c; }
QWidget#secondaryNav { background: #f7faff; border-bottom-color: #d7e0ea; }
QPushButton#secondaryNavButton, QPushButton#primaryNavButton { color: #5c6c7c; }
QPushButton#secondaryNavButton:hover, QPushButton#primaryNavButton:hover { color: #142536; background: rgba(36,107,206,.06); }
QPushButton#secondaryNavButton:checked, QPushButton#primaryNavButton:checked { color: #163653; border-bottom-color: #246bce; }
QWidget#primaryNav, QPushButton#titleConfigButton, QPushButton#titleThemeButton,
QPushButton#windowMinimizeButton, QPushButton#windowMaximizeButton, QPushButton#accountButton { color: #31516e; }
QLineEdit#globalSearch, QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox { color: #142536; background: #f7faff; border-color: #b8c8d9; }
QFrame#navigationPanel, QFrame#configPanel { background: #f9fbfd; }
QFrame#navigationPanel { border-right-color: #d7e0ea; }
QListWidget#navigationList::item { color: #5c6c7c; }
QListWidget#navigationList::item:hover { color: #142536; background: rgba(36,107,206,.06); }
QListWidget#navigationList::item:selected { color: #163653; background: rgba(36,107,206,.14); border-color: rgba(36,107,206,.38); }
QFrame#configPanel { border-left-color: #d7e0ea; }
QFrame#profileCard, QFrame#metricCard, QFrame#workbenchCard, QFrame#emptyBusinessCard { background: #ffffff; border-color: #d7e0ea; }
QFrame#loginPage { background: #edf3f9; }
QFrame#loginCard { background: #ffffff; border-color: #bdcddd; }
QFrame#loginBrandPanel { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #e4eef8,stop:.72 #f0f5fa,stop:1 #e7f0f8); border-color: #bdcddd; }
QFrame#loginFormPanel { background: #f7f9fc; }
QLabel#loginBrandTitle { color: #142536; }
QLabel#loginBrandText { color: #5c6c7c; }
QLabel#loginVersion { color: #7890a2; }
QLabel#loginFormTitle { color: #142536; }
QLabel#loginHint, QLabel#loginLabel, QLabel#loginFoot { color: #5c6c7c; }
QPushButton#loginToolButton { color: #5c6c7c; }
QPushButton#loginToolButton:hover { color: #142536; background: rgba(36,107,206,.08); }
QPushButton#loginTab, QPushButton#loginTabActive { color: #5c6c7c; }
QPushButton#loginTabActive { color: #163653; border-bottom-color: #246bce; }
QLineEdit#loginInput { color: #142536; background: #ffffff; border-color: #bdcddd; selection-background-color: #dce9f7; }
QLineEdit#loginInput:focus { border-color: #246bce; }
QPushButton#loginSubmit { color: #ffffff; background: #246bce; border-color: #246bce; }
QPushButton#loginSubmit:hover { background: #3f80dc; }
QPushButton#loginSubmit:disabled { color: #e8eef5; background: #89a9d7; }
QLabel#loginError { color: #c23b3b; }
QLabel#loginState { color: #5c6c7c; }
QPushButton#loginLink { color: #246bce; }
QLabel#profileStatus, QLabel#cardStatus, QLabel#productChange { color: #12805d; }
QLabel#productName { color: #142536; }
QLabel#productCode, QLabel#metricNote { color: #7b8b9a; }
QFrame#productRow { border-bottom-color: #d7e0ea; }
QFrame#productRow:hover, QFrame#productRow[selected="true"] { background: rgba(36,107,206,.08); }
QPushButton#secondaryButton, QPushButton#smallButton, QPushButton#logoutButton, QPushButton#configValueButton { color: #31516e; background: #f7faff; border-color: #b8c8d9; }
QPushButton#secondaryButton:hover, QPushButton#smallButton:hover, QPushButton#logoutButton:hover, QPushButton#configValueButton:hover { color: #163653; background: rgba(36,107,206,.08); border-color: #246bce; }
QStatusBar { color: #5c6c7c; background: #f7faff; border-top-color: #d7e0ea; }
QLabel#statusConnection { color: #12805d; }
QLabel#statusConnection[connected="false"] { color: #a66a00; }
QScrollBar:vertical { background: #eef3f8; }
QScrollBar::handle:vertical { background: #b8c8d9; }
QTabWidget::pane, QTableWidget, QGroupBox { color: #142536; background: #ffffff; border-color: #d7e0ea; }
QTabBar::tab, QHeaderView::section { color: #5c6c7c; background: #eef3f8; border-color: #d7e0ea; }
QTabBar::tab:hover { color: #142536; background: #e8eef5; }
QTabBar::tab:selected { color: #163653; background: #ffffff; border-bottom-color: #246bce; }
QFrame#marketMetricCard, QFrame#marketDetailPanel { background: #ffffff; border-color: #d7e0ea; }
QLabel#marketMetricLabel, QLabel#marketDetailLabel, QLabel#marketDetailSection, QLabel#marketPageMeta { color: #5c6c7c; }
QLabel#marketMetricValue, QLabel#marketDetailValue { color: #142536; }
QLabel#marketMetricNote { color: #7b8b9a; }
QLabel#marketMetricNote[status="ok"] { color: #12805d; }
QLabel#marketDetailTitle { color: #246bce; }
QLabel#marketDetailSubtitle, QLabel#marketDetailEmpty { color: #7b8b9a; }
QLabel#marketDetailPrice { color: #142536; }
QLabel#marketDetailChange { color: #5c6c7c; }
QLabel#marketDetailChange[trend="up"] { color: #12805d; }
QLabel#marketDetailChange[trend="down"] { color: #c23b3b; }
QLabel#tableSummary { color: #5c6c7c; }
QTableWidget::item { background: #ffffff; border-bottom-color: #e1e8ef; }
QTableWidget::item:hover { background: #edf5fb; }
QTableWidget::item:selected { background: #d8ebf7; color: #142536; }
QCheckBox, QCheckBox#configSwitch { color: #31516e; }
"""

# 保留旧的公开名称，避免现有业务页或下游测试因主题重构而无法导入。
WORKBENCH_QSS = DARK_NAVY_QSS
THEME_QSS = {"dark_navy": DARK_NAVY_QSS, "light": LIGHT_QSS}


def apply_workbench_theme(app: QApplication, mode: str = "dark_navy") -> str:
    """在 QApplication 级别应用指定主题并返回规范化模式名。"""
    normalized = str(mode).lower().strip()
    if normalized in {"light", "light_theme"}:
        normalized = "light"
    else:
        normalized = "dark_navy"
    if not PYSIDE_AVAILABLE:
        return normalized
    app.setStyle("Fusion")
    app.setStyleSheet(THEME_QSS[normalized])
    return normalized
