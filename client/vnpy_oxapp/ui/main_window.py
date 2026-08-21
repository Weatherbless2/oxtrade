"""工单 #16 公共管理页面壳：左侧导航、右侧切换区和连接状态栏。"""

from __future__ import annotations

try:
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import (
        QFrame, QHBoxLayout, QLabel, QListWidget, QListWidgetItem, QMainWindow,
        QStackedWidget, QStatusBar, QVBoxLayout, QWidget,
    )
    PYSIDE_AVAILABLE = True
except ImportError:
    PYSIDE_AVAILABLE = False

from .page_container import PageContainer, PageState
from .qt_bridge import AsyncQtBridge


if PYSIDE_AVAILABLE:
    class OxAppMainWindow(QMainWindow):
        """只提供公共壳；#18-#21 在对应页替换当前占位内容。"""

        def __init__(self, bridge: AsyncQtBridge, parent: QWidget | None = None) -> None:
            super().__init__(parent)
            self.bridge = bridge
            self.setWindowTitle("oxApp 管理客户端")
            self.resize(1180, 760)
            self._navigation = QListWidget()
            self._navigation.setFixedWidth(190)  # 左侧功能区尺寸稳定，右侧页面可独立扩展。
            self._pages = QStackedWidget()
            self._build_layout()
            self._build_pages()
            self._navigation.currentRowChanged.connect(self._pages.setCurrentIndex)
            bridge.state_changed.connect(self._show_connection_state)
            bridge.response_received.connect(self._handle_response)
            self._navigation.setCurrentRow(0)

        def _build_layout(self) -> None:
            root = QWidget(self)
            layout = QHBoxLayout(root)
            layout.setContentsMargins(8, 8, 8, 8)
            layout.setSpacing(8)
            layout.addWidget(self._navigation)
            line = QFrame()
            line.setFrameShape(QFrame.Shape.VLine)
            layout.addWidget(line)
            layout.addWidget(self._pages, 1)
            self.setCentralWidget(root)
            self._connection_status = QLabel("未连接")
            status = QStatusBar(self)
            status.addPermanentWidget(self._connection_status)
            self.setStatusBar(status)

        def _build_pages(self) -> None:
            pages = (
                ("概览", "客户端通信已就绪，等待业务页面接入"),
                ("系统管理", "#18 将接入数据字典、菜单和日志页面"),
                ("用户权限", "#19 将接入用户、角色、站点和权限页面"),
                ("产品资金", "#20 将接入产品、资产和资金账号页面"),
                ("风控管理", "#21 将接入参数和规则页面"),
            )
            for title, placeholder in pages:
                self._navigation.addItem(QListWidgetItem(title))
                content = QLabel(placeholder)
                content.setAlignment(Qt.AlignmentFlag.AlignCenter)
                self._pages.addWidget(PageContainer(content))

        def _show_connection_state(self, state: str, reason: str) -> None:
            self._connection_status.setText(f"连接状态: {state} | {reason}")

        def _handle_response(self, response: object) -> None:
            # #16 只验证 UI 接收路径；具体页面在 #18-#21 自己解释 payload。
            del response
else:
    class OxAppMainWindow:  # type: ignore[no-redef]
        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
