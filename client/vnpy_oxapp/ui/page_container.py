"""业务页面共用的加载、空数据、错误和无权限状态容器。

容器让所有状态占用同一个 ``QStackedWidget`` 区域，状态切换不会改变
外层三栏布局尺寸。业务页只需要在请求生命周期调用 ``show_state``，
不必重复实现加载动画、空结果和权限错误的布局。
"""

from __future__ import annotations

from enum import Enum

try:
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QLabel, QStackedWidget, QVBoxLayout, QWidget
    PYSIDE_AVAILABLE = True
except ImportError:
    PYSIDE_AVAILABLE = False


class PageState(str, Enum):
    """页面状态键；值用于日志、测试和后续状态映射。"""

    CONTENT = "content"
    LOADING = "loading"
    EMPTY = "empty"
    ERROR = "error"
    UNAUTHORIZED = "unauthorized"


if PYSIDE_AVAILABLE:
    class PageContainer(QWidget):
        """让业务内容与所有通用状态共享同一尺寸的容器。"""

        def __init__(self, content: QWidget, parent: QWidget | None = None) -> None:
            """创建内容插槽和四种预建状态标签。"""
            super().__init__(parent)
            self._stack = QStackedWidget(self)  # 所有页面状态共享一个堆叠容器，切换不改变外层布局尺寸。
            self._content = content  # 保存真实页面插槽，后续 #18-#21 只替换这里的内容。
            self._stack.addWidget(content)  # 首个页面是正常内容态。
            self._states = {state: self._state_label(text) for state, text in {  # 预建加载/空/错误/权限四种公共态。
                PageState.LOADING: "正在加载...",
                PageState.EMPTY: "暂无数据",
                PageState.ERROR: "请求失败",
                PageState.UNAUTHORIZED: "无访问权限",
            }.items()}
            for widget in self._states.values():
                self._stack.addWidget(widget)  # 所有状态对象由容器持有，避免临时控件被提前销毁。
            layout = QVBoxLayout(self)  # 外层只负责把固定尺寸的堆叠容器放入页面。
            layout.setContentsMargins(0, 0, 0, 0)  # 状态切换不额外引入边距跳动。
            layout.addWidget(self._stack)  # 内容和状态标签共享同一显示区域。
            self.show_state(PageState.CONTENT)  # 构造完成后默认显示真实内容。

        def show_state(self, state: PageState, detail: str = "") -> None:
            """切换可见状态；detail 非空时覆盖默认提示文案。"""
            if state is PageState.CONTENT:  # 正常数据已准备好，切回页面内容。
                self._stack.setCurrentWidget(self._content)  # 不销毁其他状态，后续请求可复用。
                return
            label = self._states[state]  # 取预建标签，避免每次错误响应创建临时 QWidget。
            label.setText(detail or label.property("default_text"))  # 有后端细节时显示细节，否则使用通用文案。
            self._stack.setCurrentWidget(label)  # 页面只切换可见项，所有权仍归容器。

        @staticmethod
        def _state_label(text: str) -> QLabel:
            """创建带默认文案和统一 objectName 的状态标签。"""
            label = QLabel(text)  # 标签承载统一状态文案。
            label.setObjectName("pageStateLabel")  # 交给全局工作台主题统一显示加载、空数据和异常态。
            label.setProperty("default_text", text)  # 保存默认文案，下一次无 detail 时恢复。
            label.setAlignment(Qt.AlignmentFlag.AlignCenter)  # 状态信息居中，避免页面内容偏移。
            return label  # 返回给堆叠容器接管生命周期。
else:
    class PageContainer:  # type: ignore[no-redef]
        """无 PySide6 时的明确失败边界。"""

        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
