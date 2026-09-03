"""工作台左侧固定系统导航。

导航项来自 ``menu_config.py``，这里只负责把配置转换成 Qt 列表并发出
选中事件；页面索引和具体业务请求仍由主窗口管理。
"""

from __future__ import annotations

try:
    from PySide6.QtCore import Qt, Signal
    from PySide6.QtWidgets import (
        QFrame,
        QLabel,
        QListWidget,
        QListWidgetItem,
        QVBoxLayout,
        QWidget,
    )
    PYSIDE_AVAILABLE = True
except ImportError:
    # 保持 UI 依赖可选，让无 Qt 的通信和服务测试继续运行。
    PYSIDE_AVAILABLE = False

from .menu_config import SYSTEM_MENUS


if PYSIDE_AVAILABLE:
    class NavigationPanel(QFrame):
        """固定宽度、不可浮动的图标+文字导航栏。

        ``menu_selected`` 只携带配置中的 ``item_id``，不携带页面对象，
        这样导航层不会依赖业务页面实现。
        """

        menu_selected = Signal(str)

        def __init__(self, parent: QWidget | None = None) -> None:
            """创建导航栏并根据系统菜单配置填充列表。"""
            super().__init__(parent)
            self.setObjectName("navigationPanel")
            self.setFixedWidth(210)  # 原型固定左栏宽度，避免业务页布局随菜单文字抖动。
            self._menu_items = SYSTEM_MENUS
            self._build()

        def _build(self) -> None:
            """构造标题、菜单列表和环境提示脚注。"""
            layout = QVBoxLayout(self)
            layout.setContentsMargins(10, 14, 10, 10)
            layout.setSpacing(6)

            brand = QLabel("系统菜单")
            brand.setObjectName("navigationKicker")
            layout.addWidget(brand)

            # QListWidget 仅作为视觉列表；真实路由使用每行 UserRole 中的 item_id。
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

            footer = QLabel("基础壳层 · Mock 环境")
            footer.setObjectName("navigationFooter")
            layout.addWidget(footer)

        @property
        def list_widget(self) -> QListWidget:
            """返回兼容旧页面/测试的列表句柄。"""
            return self._list

        def _on_row_changed(self, row: int) -> None:
            """把列表行号转换成稳定的菜单 ID，再通知主窗口路由。"""
            if 0 <= row < len(self._menu_items):
                self.menu_selected.emit(self._menu_items[row].item_id)

        def select(self, item_id: str) -> None:
            """按菜单 ID 选中一行，供标题栏一级菜单反向驱动左侧导航。"""
            for row, item in enumerate(self._menu_items):
                if item.item_id == item_id:
                    self._list.setCurrentRow(row)
                    return

else:
    class NavigationPanel:  # type: ignore[no-redef]
        """无 Qt 时的明确失败边界。"""

        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
