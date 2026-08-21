"""后续管理页面共用的加载、空数据、错误和无权限状态容器。"""

from __future__ import annotations

from enum import Enum

try:
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QLabel, QStackedWidget, QVBoxLayout, QWidget
    PYSIDE_AVAILABLE = True
except ImportError:
    PYSIDE_AVAILABLE = False


class PageState(str, Enum):
    CONTENT = "content"
    LOADING = "loading"
    EMPTY = "empty"
    ERROR = "error"
    UNAUTHORIZED = "unauthorized"


if PYSIDE_AVAILABLE:
    class PageContainer(QWidget):
        """页面内容与所有通用状态共用同一尺寸容器，切换不引起主布局跳动。"""

        def __init__(self, content: QWidget, parent: QWidget | None = None) -> None:
            super().__init__(parent)
            self._stack = QStackedWidget(self)
            self._content = content
            self._stack.addWidget(content)
            self._states = {state: self._state_label(text) for state, text in {
                PageState.LOADING: "正在加载...",
                PageState.EMPTY: "暂无数据",
                PageState.ERROR: "请求失败",
                PageState.UNAUTHORIZED: "无访问权限",
            }.items()}
            for widget in self._states.values():
                self._stack.addWidget(widget)
            layout = QVBoxLayout(self)
            layout.setContentsMargins(0, 0, 0, 0)
            layout.addWidget(self._stack)
            self.show_state(PageState.CONTENT)

        def show_state(self, state: PageState, detail: str = "") -> None:
            if state is PageState.CONTENT:
                self._stack.setCurrentWidget(self._content)
                return
            label = self._states[state]
            label.setText(detail or label.property("default_text"))
            self._stack.setCurrentWidget(label)

        @staticmethod
        def _state_label(text: str) -> QLabel:
            label = QLabel(text)
            label.setProperty("default_text", text)
            label.setAlignment(Qt.AlignmentFlag.AlignCenter)
            return label
else:
    class PageContainer:  # type: ignore[no-redef]
        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
