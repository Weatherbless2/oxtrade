"""oxApp 管理工作台的 vn.py 风格主题和公共控件样式。"""

from __future__ import annotations

try:
    from PySide6.QtWidgets import QApplication
    PYSIDE_AVAILABLE = True
except ImportError:
    PYSIDE_AVAILABLE = False


WORKBENCH_QSS = """
QMainWindow, QWidget {
    background: #11161b;
    color: #d9e1e8;
    font-family: "Microsoft YaHei UI", "Noto Sans CJK SC", sans-serif;
    font-size: 13px;
}
QMenuBar {
    background: #171d23;
    color: #cbd5dd;
    border-bottom: 1px solid #2b353e;
    padding: 2px 6px;
}
QMenuBar::item {
    padding: 6px 12px;
    background: transparent;
}
QMenuBar::item:selected, QMenu::item:selected {
    background: #263b4b;
    color: #f3f7fa;
}
QMenu {
    background: #1a2128;
    color: #d9e1e8;
    border: 1px solid #35414b;
    padding: 4px;
}
QToolBar {
    background: #171d23;
    border: 0;
    border-right: 1px solid #2b353e;
    spacing: 4px;
    padding: 5px 4px;
}
QToolButton {
    color: #aebbc5;
    border: 1px solid transparent;
    padding: 8px 5px;
    min-width: 64px;
}
QToolButton:hover, QToolButton:checked {
    background: #263b4b;
    border: 1px solid #3b6f93;
    color: #f3f7fa;
}
QDockWidget {
    color: #dce5eb;
    titlebar-close-icon: none;
    titlebar-normal-icon: none;
}
QDockWidget::title {
    background: #202930;
    border: 1px solid #34414a;
    border-bottom: 0;
    padding: 7px 10px;
    text-align: left;
    font-weight: 600;
}
QFrame#navigationPanel, QFrame#runtimePanel, QFrame#pageHeader,
QFrame#summaryBar, QFrame#logPanel {
    background: #171d23;
    border: 1px solid #2d3942;
}
QLabel#brandLabel {
    color: #e9f1f6;
    font-size: 16px;
    font-weight: 700;
    padding: 5px 2px 2px;
}
QLabel#pageTitle {
    color: #edf4f8;
    font-size: 16px;
    font-weight: 600;
}
QLabel#pageSubtitle, QLabel#mutedLabel, QLabel#tableSummary {
    color: #8797a3;
}
QLabel#connectionDot {
    color: #68c28a;
    font-weight: 600;
}
QListWidget#navigationList {
    background: transparent;
    border: 0;
    outline: 0;
    padding: 6px 4px;
}
QListWidget#navigationList::item {
    padding: 9px 12px;
    margin: 2px 0;
    border-left: 3px solid transparent;
    color: #aebbc5;
}
QListWidget#navigationList::item:hover {
    background: #202d36;
    color: #e5edf2;
}
QListWidget#navigationList::item:selected {
    background: #263b4b;
    border-left: 3px solid #4b9ccc;
    color: #ffffff;
}
QTabWidget::pane {
    background: #171d23;
    border: 1px solid #2d3942;
    top: -1px;
}
QTabBar::tab {
    background: #202930;
    color: #96a6b1;
    border: 1px solid #2d3942;
    padding: 8px 18px;
    min-width: 72px;
}
QTabBar::tab:hover {
    color: #e5edf2;
    background: #26343e;
}
QTabBar::tab:selected {
    background: #2b4354;
    color: #ffffff;
    border-bottom: 2px solid #55a8da;
}
QLineEdit, QComboBox {
    background: #10151a;
    color: #dce5eb;
    border: 1px solid #36434d;
    padding: 6px 8px;
    min-height: 18px;
}
QLineEdit:focus, QComboBox:focus {
    border: 1px solid #4b9ccc;
}
QComboBox QAbstractItemView {
    background: #1a2128;
    color: #dce5eb;
    selection-background-color: #2b4354;
}
QPushButton {
    background: #28475c;
    color: #edf4f8;
    border: 1px solid #3e6d89;
    padding: 6px 14px;
    min-height: 18px;
}
QPushButton:hover {
    background: #35627e;
}
QPushButton:pressed {
    background: #1f394a;
}
QTableWidget {
    background: #12181d;
    alternate-background-color: #171f25;
    color: #d7e0e6;
    border: 1px solid #2d3942;
    gridline-color: #263139;
    selection-background-color: #294b62;
    selection-color: #ffffff;
    outline: 0;
}
QTableWidget::item {
    padding: 5px 7px;
}
QHeaderView::section {
    background: #202930;
    color: #b9c7d0;
    border: 0;
    border-right: 1px solid #34414a;
    border-bottom: 1px solid #34414a;
    padding: 7px 8px;
    font-weight: 600;
}
QHeaderView::section:hover {
    background: #2a3943;
}
QStatusBar {
    background: #171d23;
    color: #8797a3;
    border-top: 1px solid #2b353e;
}
QStatusBar::item {
    border: 0;
}
QProgressBar {
    background: #10151a;
    border: 1px solid #34414a;
    text-align: center;
    color: #dce5eb;
}
QProgressBar::chunk {
    background: #3d789d;
}
QScrollBar:vertical {
    background: #12181d;
    width: 11px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: #3a4852;
    min-height: 28px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
}
"""


def apply_workbench_theme(app: QApplication) -> None:
    """在 QApplication 级别统一主题，确保新建的 Dock 和页面控件风格一致。"""
    if not PYSIDE_AVAILABLE:
        return
    app.setStyle("Fusion")  # Fusion 在 WSL/offscreen 与 Windows 上都能稳定应用 QSS。
    app.setStyleSheet(WORKBENCH_QSS)  # 主题集中管理，页面不再各自散落颜色常量。
