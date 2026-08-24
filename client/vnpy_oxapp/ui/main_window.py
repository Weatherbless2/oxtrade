"""oxApp 管理工作台：沿用 vn.py 的菜单、Dock 和监控表格组织方式。"""

from __future__ import annotations

from datetime import datetime

try:
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QAction
    from PySide6.QtWidgets import (
        QApplication, QDockWidget, QFrame, QHBoxLayout, QLabel, QListWidget,
        QListWidgetItem, QMainWindow, QProgressBar, QStyle, QStackedWidget,
        QStatusBar, QTableWidget, QTableWidgetItem, QToolBar, QVBoxLayout, QWidget,
    )
    PYSIDE_AVAILABLE = True
except ImportError:
    PYSIDE_AVAILABLE = False

from .page_container import PageContainer
from .qt_bridge import AsyncQtBridge
from ..logging import ClientLogger, LoadTiming
from ..connection import ClientProtocolClient
from .system_admin_page import SystemAdminPage
from .user_rights_page import UserRightsPage
from .table_page import ServiceTablePage
from .fund_management_page import FundManagementPage
from .risk_management_page import RiskManagementPage
from .theme import apply_workbench_theme


if PYSIDE_AVAILABLE:
    class RuntimePanel(QFrame):
        """右侧运行状态监控，展示客户端壳的连接与服务状态。"""

        def __init__(self, parent: QWidget | None = None) -> None:
            super().__init__(parent)
            self.setObjectName("runtimePanel")
            self._connection = QLabel("●  未连接")
            self._connection.setObjectName("connectionDot")
            self._user = QLabel("用户：yangjh (Mock)")
            self._endpoint = QLabel("通道：等待连接")
            self._latency = QLabel("最近响应：-- ms")
            self._services = QTableWidget(0, 3)
            self._services.setHorizontalHeaderLabels(["服务", "状态", "延迟"])
            self._services.verticalHeader().setVisible(False)
            self._services.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
            self._services.setSelectionMode(QTableWidget.SelectionMode.NoSelection)
            self._services.setAlternatingRowColors(True)
            self._services.horizontalHeader().setStretchLastSection(True)
            for name, status, latency in (
                ("客户端通信", "正常", "2 ms"),
                ("系统管理", "Mock", "5 ms"),
                ("权限服务", "Mock", "5 ms"),
                ("风控服务", "Mock", "6 ms"),
            ):
                row = self._services.rowCount()
                self._services.insertRow(row)
                for column, value in enumerate((name, status, latency)):
                    self._services.setItem(row, column, QTableWidgetItem(value))
            self._services.resizeColumnsToContents()
            layout = QVBoxLayout(self)
            layout.setContentsMargins(12, 12, 12, 12)
            layout.setSpacing(9)
            layout.addWidget(QLabel("会话状态"))
            layout.addWidget(self._connection)
            layout.addWidget(self._user)
            layout.addWidget(self._endpoint)
            layout.addWidget(self._latency)
            layout.addSpacing(8)
            label = QLabel("服务监控")
            label.setObjectName("pageSubtitle")
            layout.addWidget(label)
            layout.addWidget(self._services, 1)

        def set_connection(self, state: str, reason: str) -> None:
            connected = state.lower() in {"connected", "ready", "正常"}
            display = "已连接" if connected else state
            self._connection.setText(f"●  {display}  |  {reason}")
            color = "#68c28a" if connected else "#d89b61"
            self._connection.setStyleSheet(f"color: {color}; font-weight: 600;")

        def set_transport_name(self, transport_name: str) -> None:
            self._endpoint.setText(f"通道：{transport_name}")  # 窗口后创建时仍主动显示实际 Transport，不能用旧的 MockTransport 静态文案误导联调结果。


    class LogPanel(QFrame):
        """底部轻量日志监控器，模拟 vn.py LogMonitor 的固定底部 Dock。"""

        def __init__(self, parent: QWidget | None = None) -> None:
            super().__init__(parent)
            self.setObjectName("logPanel")
            self._table = QTableWidget(0, 4)
            self._table.setHorizontalHeaderLabels(["时间", "级别", "模块", "事件"])
            self._table.verticalHeader().setVisible(False)
            self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
            self._table.setSelectionMode(QTableWidget.SelectionMode.NoSelection)
            self._table.horizontalHeader().setStretchLastSection(True)
            layout = QVBoxLayout(self)
            layout.setContentsMargins(8, 6, 8, 6)
            layout.addWidget(self._table)
            for level, module, message in (
                ("INFO", "oxApp", "Mock 客户端已连接"),
                ("INFO", "oxApp.ui", "工作台布局加载完成"),
                ("INFO", "oxApp.ui", "等待管理页面请求"),
            ):
                self.append(level, module, message)

        def append(self, level: str, module: str, message: str) -> None:
            row = self._table.rowCount()
            self._table.insertRow(row)
            values = (datetime.now().strftime("%H:%M:%S.%f")[:-3], level, module, message)
            for column, value in enumerate(values):
                self._table.setItem(row, column, QTableWidgetItem(value))
            self._table.resizeColumnsToContents()


    class OverviewPage(QFrame):
        """工作台首页，提供与 vn.py Trader 类似的状态总览。"""

        def __init__(self, parent: QWidget | None = None) -> None:
            super().__init__(parent)
            title = QLabel("oxApp 管理工作台")
            title.setObjectName("pageTitle")
            subtitle = QLabel("Mock-first 客户端 | 页面通过通信抽象访问服务")
            subtitle.setObjectName("pageSubtitle")
            progress = QProgressBar()
            progress.setRange(0, 100)
            progress.setValue(100)
            progress.setFormat("客户端框架就绪   %p%")
            metrics = QTableWidget(0, 3)
            metrics.setHorizontalHeaderLabels(["工作区", "当前状态", "说明"])
            metrics.verticalHeader().setVisible(False)
            metrics.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
            metrics.setSelectionMode(QTableWidget.SelectionMode.NoSelection)
            metrics.horizontalHeader().setStretchLastSection(True)
            rows = (
                ("系统管理", "可用", "菜单、字典、在线日志"),
                ("用户权限", "可用", "用户、角色、站点、角色菜单权限"),
                ("产品资金", "可用", "产品、项目、资金账号及关联"),
                ("风控管理", "可用", "参数、规则和异常事件"),
            )
            for values in rows:
                row = metrics.rowCount()
                metrics.insertRow(row)
                for column, value in enumerate(values):
                    metrics.setItem(row, column, QTableWidgetItem(value))
            metrics.resizeColumnsToContents()
            layout = QVBoxLayout(self)
            layout.setContentsMargins(24, 22, 24, 22)
            layout.setSpacing(12)
            layout.addWidget(title)
            layout.addWidget(subtitle)
            layout.addSpacing(10)
            layout.addWidget(progress)
            layout.addSpacing(12)
            section = QLabel("功能工作区")
            section.setObjectName("pageSubtitle")
            layout.addWidget(section)
            layout.addWidget(metrics)
            layout.addStretch(1)


    class OxAppMainWindow(QMainWindow):
        """中心工作区 + 左侧导航 + 右侧状态 + 底部日志的管理终端布局。"""

        def __init__(self, bridge: AsyncQtBridge, client: ClientProtocolClient | None = None, parent: QWidget | None = None, logger: ClientLogger | None = None) -> None:
            super().__init__(parent)
            app = QApplication.instance()
            if app is not None:
                apply_workbench_theme(app)  # 测试构造窗口和正式入口使用同一套 vn.py 风格主题。
            self.logger = logger or ClientLogger()
            self.client = client  # 页面只依赖客户端抽象，Mock/真实 Transport 可替换。
            self.load_timings: list[LoadTiming] = []
            self.bridge = bridge
            self.setWindowTitle("oxApp 管理工作台 | vn.py 风格客户端")
            self.resize(1440, 900)
            self.setMinimumSize(1100, 680)
            self.setDockNestingEnabled(True)
            self.setDockOptions(QMainWindow.DockOption.AllowNestedDocks | QMainWindow.DockOption.AnimatedDocks)
            self._navigation = QListWidget()
            self._navigation.setObjectName("navigationList")
            self._pages = QStackedWidget()
            with self.logger.measure("main_window_layout", sink=self.load_timings, module="oxApp.ui"):
                self._build_layout()
            with self.logger.measure("main_window_pages", sink=self.load_timings, module="oxApp.ui"):
                self._build_pages()
            self._navigation.currentRowChanged.connect(self._select_page)
            bridge.state_changed.connect(self._show_connection_state)
            bridge.response_received.connect(self._handle_response)
            if self.client is not None:
                transport_name = type(self.client.transport).__name__
                self._runtime_panel.set_transport_name(transport_name)
                self._latency_status.setText(f"{transport_name} | -- ms")
                self._show_connection_state(
                    self.client.transport.state.value,
                    f"窗口初始化时读取 {transport_name} 当前状态",
                )  # 连接可能早于 Qt signal 订阅完成，必须主动补发当前状态快照。
            self._navigation.setCurrentRow(0)
            self.logger.info(f"main_window_ready components={len(self.load_timings)}", module="oxApp.ui")

        def _build_layout(self) -> None:
            self._build_menu()
            self._build_toolbar()
            self._build_navigation_dock()
            self._build_runtime_dock()
            self._build_log_dock()
            self.setCentralWidget(self._pages)  # 中心区只承载当前工作页，避免所有业务表格同时挤在首页。
            self._connection_status = QLabel("连接状态: 未连接")
            self._latency_status = QLabel("Mock | -- ms")
            status = QStatusBar(self)
            status.showMessage("就绪 | Mock 数据模式")
            status.addPermanentWidget(self._latency_status)
            status.addPermanentWidget(self._connection_status)
            self.setStatusBar(status)

        def _build_menu(self) -> None:
            bar = self.menuBar()
            bar.setNativeMenuBar(False)  # Linux/WSL 下保持菜单嵌入窗口，和 vn.py 的跨平台行为一致。
            system = bar.addMenu("系统")
            system.addAction(self._action("连接 Mock 服务", self._connect_mock))
            system.addAction(self._action("断开服务", self._disconnect_mock))
            system.addSeparator()
            system.addAction(self._action("退出", self.close))
            features = bar.addMenu("功能")
            for index, name in enumerate(("概览", "系统管理", "用户权限", "产品资金", "风控管理")):
                features.addAction(self._action(name, lambda checked=False, i=index: self._select_page(i)))
            view = bar.addMenu("视图")
            view.addAction(self._action("恢复默认布局", self._restore_layout))
            help_menu = bar.addMenu("帮助")
            help_menu.addAction(self._action("关于 oxApp", self._show_about))

        def _build_toolbar(self) -> None:
            toolbar = QToolBar("工具栏", self)
            toolbar.setObjectName("mainToolbar")
            toolbar.setMovable(False)
            toolbar.setFloatable(False)
            refresh = self._action("刷新当前", self._refresh_current, QStyle.StandardPixmap.SP_BrowserReload)
            connect = self._action("连接 Mock", self._connect_mock, QStyle.StandardPixmap.SP_DialogApplyButton)
            layout = self._action("默认布局", self._restore_layout, QStyle.StandardPixmap.SP_FileDialogDetailedView)
            toolbar.addAction(refresh)
            toolbar.addAction(connect)
            toolbar.addAction(layout)
            self.addToolBar(Qt.ToolBarArea.TopToolBarArea, toolbar)

        def _build_navigation_dock(self) -> None:
            panel = QFrame()
            panel.setObjectName("navigationPanel")
            layout = QVBoxLayout(panel)
            layout.setContentsMargins(10, 12, 10, 10)
            layout.setSpacing(5)
            brand = QLabel("oxApp")
            brand.setObjectName("brandLabel")
            subtitle = QLabel("管理工作台")
            subtitle.setObjectName("pageSubtitle")
            layout.addWidget(brand)
            layout.addWidget(subtitle)
            layout.addSpacing(8)
            for title in ("概览", "系统管理", "用户权限", "产品资金", "风控管理"):
                self._navigation.addItem(QListWidgetItem(title))
            layout.addWidget(self._navigation, 1)
            dock = QDockWidget("功能导航", self)
            dock.setObjectName("navigationDock")
            dock.setWidget(panel)
            dock.setFeatures(QDockWidget.DockWidgetFeature.DockWidgetMovable)
            dock.setMinimumWidth(185)
            dock.setMaximumWidth(245)
            self.addDockWidget(Qt.DockWidgetArea.LeftDockWidgetArea, dock)
            self._navigation_dock = dock

        def _build_runtime_dock(self) -> None:
            dock = QDockWidget("运行状态", self)
            dock.setObjectName("runtimeDock")
            self._runtime_panel = RuntimePanel()
            dock.setWidget(self._runtime_panel)
            dock.setMinimumWidth(260)
            dock.setMaximumWidth(360)
            dock.setFeatures(QDockWidget.DockWidgetFeature.DockWidgetMovable)
            self.addDockWidget(Qt.DockWidgetArea.RightDockWidgetArea, dock)
            self._runtime_dock = dock

        def _build_log_dock(self) -> None:
            dock = QDockWidget("运行日志", self)
            dock.setObjectName("logDock")
            self._log_panel = LogPanel()
            dock.setWidget(self._log_panel)
            dock.setMinimumHeight(135)
            dock.setMaximumHeight(235)
            dock.setFeatures(QDockWidget.DockWidgetFeature.DockWidgetMovable)
            self.addDockWidget(Qt.DockWidgetArea.BottomDockWidgetArea, dock)
            self._log_dock = dock

        def _build_pages(self) -> None:
            self._add_page(OverviewPage())
            if self.client is not None:
                self._add_page(SystemAdminPage(self.client, self.bridge, self.logger))
                self._add_page(UserRightsPage(self.client, self.bridge, self.logger))
                self._add_page(FundManagementPage(self.client, self.bridge, self.logger))
                self._add_page(RiskManagementPage(self.client, self.bridge, self.logger))
            else:
                for _ in range(4):
                    self._add_page(QLabel("缺少客户端通信实例"))

        def _add_page(self, content: QWidget) -> None:
            if isinstance(content, ServiceTablePage):
                page = content  # 业务页已经包含页签和请求状态，不再嵌套额外壳。
            elif isinstance(content, QLabel):
                content.setAlignment(Qt.AlignmentFlag.AlignCenter)
                page = PageContainer(content)
            else:
                page = content
            self._pages.addWidget(page)

        def _select_page(self, index: int) -> None:
            if 0 <= index < self._pages.count():
                self._pages.setCurrentIndex(index)  # 导航只切换工作区，不销毁已加载页面和请求状态。
                self._log_panel.append("INFO", "oxApp.ui", f"切换工作区: {self._navigation.item(index).text()}")

        def _refresh_current(self) -> None:
            page = self._pages.currentWidget()
            if hasattr(page, "_tabs") and hasattr(page, "_load_current"):
                page._load_current(page._tabs.currentIndex())  # 复用页面现有异步请求，不阻塞 Qt 主线程。
            self._log_panel.append("INFO", "oxApp.ui", "刷新当前工作区")

        def _connect_mock(self) -> None:
            if self.client is not None:
                self.client.connect()
            self._log_panel.append("INFO", "oxApp", "已发起 Mock 服务连接")

        def _disconnect_mock(self) -> None:
            if self.client is not None:
                self.client.disconnect()
            self._log_panel.append("INFO", "oxApp", "已断开 Mock 服务连接")

        def _restore_layout(self) -> None:
            self._navigation_dock.show()
            self._runtime_dock.show()
            self._log_dock.show()
            self._log_panel.append("INFO", "oxApp.ui", "已恢复默认工作台布局")

        def _show_about(self) -> None:
            self.statusBar().showMessage("oxApp | vn.py 风格管理客户端 | Mock-first", 5000)

        def _show_connection_state(self, state: str, reason: str) -> None:
            self._connection_status.setText(f"连接状态: {state}")
            self._runtime_panel.set_connection(state, reason)
            self._log_panel.append("INFO", "oxApp.transport", f"连接状态: {state} | {reason}")

        def _handle_response(self, response: object) -> None:
            del response  # 页面自行按 request_id 消费响应，窗口只保留公共状态壳。

        @staticmethod
        def _action(text: str, callback, icon: QStyle.StandardPixmap | None = None) -> QAction:
            action = QAction(text)
            if icon is not None:
                action.setIcon(QApplication.style().standardIcon(icon))
            action.triggered.connect(callback)
            return action

else:
    class OxAppMainWindow:  # type: ignore[no-redef]
        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
