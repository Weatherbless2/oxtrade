"""标题栏一级菜单下方的二级上下文导航。"""

from __future__ import annotations

try:
    from PySide6.QtCore import Qt, Signal
    from PySide6.QtWidgets import QFrame, QHBoxLayout, QPushButton, QSizePolicy, QWidget
    PYSIDE_AVAILABLE = True
except ImportError:
    PYSIDE_AVAILABLE = False

from .menu_config import SECONDARY_MENUS


if PYSIDE_AVAILABLE:
    class SecondaryNavigation(QFrame):
        """46px 高的二级导航，只负责显示和发出选中项。"""

        item_selected = Signal(str, str)

        def __init__(self, parent: QWidget | None = None) -> None:
            super().__init__(parent)
            self.setObjectName("secondaryNav")
            self.setFixedHeight(46)
            self._buttons: dict[str, QPushButton] = {}
            self._primary_id = "home"
            self._build()

        def _build(self) -> None:
            self._layout = QHBoxLayout(self)
            self._layout.setContentsMargins(22, 0, 22, 0)
            self._layout.setSpacing(26)
            self._layout.addStretch(1)
            self.set_primary(self._primary_id)

        def set_primary(self, primary_id: str) -> None:
            """按一级菜单重建二级项，并默认选中第一项。"""
            for button in self._buttons.values():
                button.deleteLater()
            self._buttons.clear()
            while self._layout.count() > 0:
                item = self._layout.takeAt(0)
                if item.widget() is not None:
                    item.widget().deleteLater()
            entries = SECONDARY_MENUS.get(primary_id, SECONDARY_MENUS["home"])
            self._primary_id = primary_id
            for entry in entries:
                button = QPushButton(entry.label, self)
                button.setObjectName("secondaryNavButton")
                button.setProperty("menu_id", entry.item_id)
                button.setCheckable(True)
                button.setCursor(Qt.CursorShape.PointingHandCursor)
                button.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Expanding)
                button.clicked.connect(lambda checked=False, item_id=entry.item_id: self._select(item_id))
                self._layout.addWidget(button)
                self._buttons[entry.item_id] = button
            self._layout.addStretch(1)
            first = entries[0].item_id if entries else ""
            self._select(first, emit=False)

        def _select(self, item_id: str, emit: bool = True) -> None:
            for key, button in self._buttons.items():
                button.setChecked(key == item_id)
            if emit:
                self.item_selected.emit(self._primary_id, item_id)

        @property
        def buttons(self) -> dict[str, QPushButton]:
            return dict(self._buttons)

        @property
        def current_item_id(self) -> str:
            return next((key for key, button in self._buttons.items() if button.isChecked()), "")

else:
    class SecondaryNavigation:  # type: ignore[no-redef]
        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
