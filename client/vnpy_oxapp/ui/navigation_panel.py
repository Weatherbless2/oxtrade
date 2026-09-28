"""工作台左侧固定系统导航。

可见导航严格对齐原型中的六个系统入口；迁移期间保留一个隐藏的九项
兼容列表，供旧页面按历史索引访问，不会污染新壳层视觉。
"""

from __future__ import annotations

try:
    from PySide6.QtCore import Qt, Signal
    from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QListWidget, QListWidgetItem, QPushButton, QVBoxLayout, QWidget
    PYSIDE_AVAILABLE = True
except ImportError:
    PYSIDE_AVAILABLE = False

from .menu_config import LEGACY_SYSTEM_MENUS, SYSTEM_MENUS


if PYSIDE_AVAILABLE:
    class NavigationPanel(QFrame):
        """固定宽度、不可浮动的图标+文字导航栏。"""

        menu_selected = Signal(str)
        toggle_requested = Signal()

        def __init__(self, parent: QWidget | None = None) -> None:
            """创建可见导航和迁移兼容列表。"""
            super().__init__(parent)
            self.setObjectName("navigationPanel")
            self.setFixedWidth(210)
            self._collapsed = False
            self._menu_items = SYSTEM_MENUS
            self._legacy_menu_items = LEGACY_SYSTEM_MENUS
            self._build()

        def _build(self) -> None:
            """构造原型可见菜单，并保留隐藏的历史索引句柄。"""
            layout = QVBoxLayout(self)
            layout.setContentsMargins(10, 14, 10, 10)
            layout.setSpacing(6)

            head = QFrame(self)
            head.setObjectName("navigationHeader")
            head_layout = QHBoxLayout(head)
            head_layout.setContentsMargins(0, 0, 0, 0)
            head_layout.setSpacing(6)
            brand = QLabel("系统菜单")
            brand.setObjectName("navigationKicker")
            head_layout.addWidget(brand)
            head_layout.addStretch(1)
            toggle = QPushButton("‹")
            toggle.setObjectName("navigationToggleButton")
            toggle.setFixedSize(28, 28)
            toggle.setToolTip("收起系统菜单")
            toggle.clicked.connect(lambda _checked=False: self.toggle_requested.emit())
            head_layout.addWidget(toggle)
            self._brand = brand
            self._toggle = toggle
            layout.addWidget(head)

            self._list = QListWidget()
            self._list.setObjectName("navigationList")
            self._list.setFrameShape(QFrame.Shape.NoFrame)
            self._list.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
            self._list.currentRowChanged.connect(self._on_row_changed)
            for item in self._menu_items:
                row = QListWidgetItem(f"{item.icon}  {item.label}")
                row.setData(Qt.ItemDataRole.UserRole, item.item_id)
                row.setToolTip(item.label)
                self._list.addItem(row)
            layout.addWidget(self._list, 1)

            footer = QLabel("")
            footer.setObjectName("navigationFooter")
            self._footer = footer
            layout.addWidget(footer)

            # 隐藏兼容列表不参与布局，旧调用仍可通过属性按历史索引切换页面。
            self._legacy_list = QListWidget(self)
            self._legacy_list.setObjectName("legacyNavigationList")
            self._legacy_list.setVisible(False)
            for item in self._legacy_menu_items:
                row = QListWidgetItem(f"{item.icon}  {item.label}")
                row.setData(Qt.ItemDataRole.UserRole, item.item_id)
                self._legacy_list.addItem(row)

        @property
        def list_widget(self) -> QListWidget:
            """返回新壳层实际可见导航列表。"""
            return self._list

        @property
        def compatibility_list_widget(self) -> QListWidget:
            """返回迁移期间的旧九项索引列表。"""
            return self._legacy_list

        @property
        def toggle_button(self) -> QPushButton:
            """返回左栏收起按钮，供主窗口统一更新 ShellState。"""
            return self._toggle

        @property
        def collapsed(self) -> bool:
            """返回当前是否为仅图标的收起状态。"""
            return self._collapsed

        def set_collapsed(self, collapsed: bool) -> None:
            """切换左栏宽度和文字可见性，中央布局由父级自动重新分配。"""
            collapsed = bool(collapsed)
            if collapsed == self._collapsed:
                return
            self._collapsed = collapsed
            self.setFixedWidth(56 if collapsed else 210)
            self._brand.setVisible(not collapsed)
            self._footer.setVisible(not collapsed)
            self._toggle.setText("›" if collapsed else "‹")
            self._toggle.setToolTip("显示系统菜单" if collapsed else "收起系统菜单")
            self._list.setProperty("compact", collapsed)
            for row, item in enumerate(self._menu_items):
                list_item = self._list.item(row)
                if list_item is not None:
                    list_item.setText(item.icon if collapsed else f"{item.icon}  {item.label}")
            self._list.style().unpolish(self._list)
            self._list.style().polish(self._list)
            self.updateGeometry()

        def _on_row_changed(self, row: int) -> None:
            """把可见列表行号转换成稳定菜单 ID。"""
            if 0 <= row < len(self._menu_items):
                self.menu_selected.emit(self._menu_items[row].item_id)

        def select(self, item_id: str, emit: bool = True) -> None:
            """按菜单 ID 选中可见导航，可选是否广播路由事件。"""
            for row, item in enumerate(self._menu_items):
                if item.item_id == item_id:
                    self._list.blockSignals(not emit)
                    self._list.setCurrentRow(row)
                    self._list.blockSignals(False)
                    return

        def select_legacy_page(self, page_index: int) -> None:
            """同步兼容列表的历史页面索引，不改变可见选中态。"""
            if 0 <= page_index < self._legacy_list.count():
                self._legacy_list.blockSignals(True)
                self._legacy_list.setCurrentRow(page_index)
                self._legacy_list.blockSignals(False)

else:
    class NavigationPanel:  # type: ignore[no-redef]
        """无 PySide6 时的明确失败边界。"""

        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
