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
            self.bridge = bridge  # 保存跨线程消息入口，窗口不直接依赖 Transport。
            self.setWindowTitle("oxApp 管理客户端")  # 用户看到的公共窗口标题。
            self.resize(1180, 760)  # 固定初始工作区，后续页面在右侧扩展。
            self._navigation = QListWidget()  # 左侧导航只负责选择页面索引。
            self._navigation.setFixedWidth(190)  # 左侧功能区尺寸稳定，右侧页面可独立扩展。
            self._pages = QStackedWidget()  # 右侧页面容器按索引切换，不销毁已创建页面。
            self._build_layout()  # 创建根布局、分隔线和状态栏。
            self._build_pages()  # 创建 #16 公共占位页，后续工单替换内容。
            self._navigation.currentRowChanged.connect(self._pages.setCurrentIndex)  # 导航变化只改变右侧当前页。
            bridge.state_changed.connect(self._show_connection_state)  # Qt 主线程更新连接状态栏。
            bridge.response_received.connect(self._handle_response)  # 响应先经过 bridge，再由页面层解释 payload。
            self._navigation.setCurrentRow(0)  # 首次展示概览页。

        def _build_layout(self) -> None:
            root = QWidget(self)  # 根控件承载左右两栏，生命周期归 QMainWindow。
            layout = QHBoxLayout(root)  # 水平布局保证左导航和右内容并排。
            layout.setContentsMargins(8, 8, 8, 8)  # 给窗口边缘稳定留白。
            layout.setSpacing(8)  # 导航、分隔线和页面之间保持固定间距。
            layout.addWidget(self._navigation)  # 导航由布局接管所有权。
            line = QFrame()  # 视觉分隔，不参与业务状态。
            line.setFrameShape(QFrame.Shape.VLine)  # 显示竖线，帮助用户区分两栏。
            layout.addWidget(line)  # 分隔线加入根布局。
            layout.addWidget(self._pages, 1)  # 右侧使用伸缩因子吸收剩余空间。
            self.setCentralWidget(root)  # QMainWindow 接管根控件释放责任。
            self._connection_status = QLabel("未连接")  # 状态栏初始值与 Transport 初态一致。
            status = QStatusBar(self)  # 状态栏由窗口持有。
            status.addPermanentWidget(self._connection_status)  # 状态文本固定在右侧。
            self.setStatusBar(status)  # 安装到窗口底部。

        def _build_pages(self) -> None:
            pages = (
                ("概览", "客户端通信已就绪，等待业务页面接入"),
                ("系统管理", "#18 将接入数据字典、菜单和日志页面"),
                ("用户权限", "#19 将接入用户、角色、站点和权限页面"),
                ("产品资金", "#20 将接入产品、资产和资金账号页面"),
                ("风控管理", "#21 将接入参数和规则页面"),
            )
            for title, placeholder in pages:
                self._navigation.addItem(QListWidgetItem(title))  # 导航项索引与页面堆叠索引一一对应。
                content = QLabel(placeholder)  # 工单 16 只放占位内容，不宣称页面业务已实现。
                content.setAlignment(Qt.AlignmentFlag.AlignCenter)  # 占位文案居中显示。
                self._pages.addWidget(PageContainer(content))  # 页面容器统一提供加载/空/错误/权限状态。

        def _show_connection_state(self, state: str, reason: str) -> None:
            self._connection_status.setText(f"连接状态: {state} | {reason}")  # 状态和原因来自后台回调，经 Qt 主线程安全更新。

        def _handle_response(self, response: object) -> None:
            # #16 只验证 UI 接收路径；具体页面在 #18-#21 自己解释 payload。
            del response  # #16 只验证事件到达，具体 payload 解释留给 #18-#21 页面。
else:
    class OxAppMainWindow:  # type: ignore[no-redef]
        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
