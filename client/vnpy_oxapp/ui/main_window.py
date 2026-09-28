"""oxApp 固定三栏工作台的主窗口和页面编排。

主窗口是壳层唯一的路由协调者：它把标题栏、左侧导航、中央页面堆栈、
右侧配置面板和登录页组合起来，再把用户操作映射为 ``ShellState``、
``ClientSession`` 或业务页面动作。通信与行情仍通过服务层和 Qt Bridge
进入页面，主窗口不直接解析业务协议。
"""

from __future__ import annotations

from datetime import datetime

try:
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtWidgets import (
        QApplication,
        QFrame,
        QHBoxLayout,
        QHeaderView,
        QLabel,
        QMainWindow,
        QPushButton,
        QStackedWidget,
        QStatusBar,
        QTableWidget,
        QTableWidgetItem,
        QVBoxLayout,
        QWidget,
    )
    PYSIDE_AVAILABLE = True
except ImportError:
    # PySide6 不是核心通信层的硬依赖；无 Qt 时导出明确的不可用边界。
    PYSIDE_AVAILABLE = False

from ..connection import ClientProtocolClient
from ..logging import ClientLogger, LoadTiming
from ..market.hub import MarketHub
from ..mock.mock_transport import MockTransport
from ..session import ClientSession
from .config_panel import ConfigPanel
from .login_page import LoginPage
from .navigation_panel import NavigationPanel
from .page_container import PageContainer
from .qt_bridge import AsyncQtBridge
from .shell_state import SessionState, ShellState, ThemeMode
from .secondary_navigation import SecondaryNavigation
from .theme import apply_workbench_theme
from .title_bar import TitleBar
from .welcome_page import WelcomePage
from .system_admin_page import SystemAdminPage
from .user_rights_page import UserRightsPage
from .table_page import ServiceTablePage
from .fund_management_page import FundManagementPage
from .risk_management_page import RiskManagementPage
from .market_page import MarketPage
from .trading_page import TradingPage


if PYSIDE_AVAILABLE:
    class LogPanel(QFrame):
        """中央工作区中的轻量运行日志页，不再占用可浮动 Dock。"""

        def __init__(self, parent: QWidget | None = None) -> None:
            """创建日志表格并填入三条壳层启动示例记录。"""
            super().__init__(parent)
            self.setObjectName("logPanel")
            self._table = QTableWidget(0, 4)
            self._table.setHorizontalHeaderLabels(["时间", "级别", "模块", "事件"])
            self._table.verticalHeader().setVisible(False)
            self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
            self._table.setSelectionMode(QTableWidget.SelectionMode.NoSelection)
            self._table.horizontalHeader().setSectionResizeMode(3, QHeaderView.ResizeMode.Stretch)
            layout = QVBoxLayout(self)
            layout.setContentsMargins(12, 12, 12, 12)
            layout.addWidget(self._table)
            for level, module, message in (
                ("INFO", "oxApp", "客户端连接已建立"),
                ("INFO", "oxApp.ui", "工作台布局加载完成"),
                ("INFO", "oxApp.ui", "等待管理页面请求"),
            ):
                self.append(level, module, message)

        def append(self, level: str, module: str, message: str) -> None:
            """追加一条展示日志；此页只负责显示，不写入正式日志文件。"""
            row = self._table.rowCount()
            self._table.insertRow(row)
            values = (datetime.now().strftime("%H:%M:%S.%f")[:-3], level, module, message)
            for column, value in enumerate(values):
                self._table.setItem(row, column, QTableWidgetItem(value))
            for column in range(3):
                self._table.resizeColumnToContents(column)


    class SessionInfoPage(QFrame):
        """兼容旧导航入口的会话信息页。

        页面只读取 ``ClientSession`` 快照并展示非敏感字段，保留该入口
        是为了让旧页面/测试在壳层迁移期间仍可访问。
        """

        def __init__(self, session: ClientSession, parent: QWidget | None = None) -> None:
            """创建会话摘要页面并立即渲染当前会话快照。"""
            super().__init__(parent)
            self.setObjectName("sessionInfoPage")
            layout = QVBoxLayout(self)
            layout.setContentsMargins(24, 24, 24, 24)
            title = QLabel("登录会话")
            title.setObjectName("pageTitle")
            self._summary = QLabel()
            self._summary.setObjectName("pageSubtitle")
            self._summary.setWordWrap(True)
            layout.addWidget(title)
            layout.addWidget(self._summary)
            layout.addStretch(1)
            self.refresh(session)

        def refresh(self, session: ClientSession) -> None:
            """从会话对象读取状态、用户和 session_id，刷新页面文案。"""
            session_id, user_name, display_name, authenticated = session.snapshot()
            state = "已登录" if authenticated else "未登录"
            self._summary.setText(
                f"状态：{state}\n用户：{display_name or user_name}\n会话：{session_id}\n\n"
                ""
            )


    class OxAppMainWindow(QMainWindow):
        """无边框固定三栏工作台。

        布局分为两个会话态：未登录时显示 ``LoginPage``，登录后显示
        ``WorkbenchBody``。中央 ``QStackedWidget`` 一次挂载现有业务页，
        这样可以在不改服务接口的情况下替换外壳；右侧面板只允许显示/隐藏，
        不支持 Dock 浮动、拖拽或布局持久化。
        """

        def __init__(
            self,
            bridge: AsyncQtBridge,
            client: ClientProtocolClient | None = None,
            parent: QWidget | None = None,
            logger: ClientLogger | None = None,
            market_hub: MarketHub | None = None,
            session_id: str = "mock-session",
        ) -> None:
            """初始化窗口、布局、页面、信号，并读取已有 Transport 状态。"""
            super().__init__(parent)
            app = QApplication.instance()
            if app is not None:
                apply_workbench_theme(app)
            self.logger = logger or ClientLogger()
            self.client = client
            self.market_hub = market_hub
            self.session = ClientSession(session_id=session_id)  # 正式 Transport 可注入部署会话；认证状态仍由登录服务决定。
            self.state = ShellState()
            self.load_timings: list[LoadTiming] = []
            self.bridge = bridge
            self._drag_enabled = True
            self.setObjectName("oxAppMainWindow")
            self.setWindowTitle("oxApp 量化工作台")
            # 无边框窗口由 TitleBar 提供拖动和最小化/最大化/关闭操作。
            self.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.Window)
            # 未登录时使用独立的小窗口；进入工作台后再切换到宽屏最小尺寸。
            self.resize(960, 600)
            self.setMinimumSize(820, 520)
            # 分开记录布局和页面构造耗时，便于后续定位业务页面加载瓶颈。
            with self.logger.measure("main_window_layout", sink=self.load_timings, module="oxApp.ui"):
                self._build_layout()
            with self.logger.measure("main_window_pages", sink=self.load_timings, module="oxApp.ui"):
                self._build_pages()
            self._connect_signals()
            self._apply_theme(self.state.theme)
            if self.client is not None:
                transport_name = type(self.client.transport).__name__
                self._latency_status.setText("延迟 -- ms")
                self._show_connection_state(
                    self.client.transport.state.value,
                    f"窗口初始化时读取 {transport_name} 当前状态",
                )
            self._navigation.setCurrentRow(0)
            self._navigation_panel.select("workbench")
            self._secondary_nav.set_primary("home")
            self._apply_session_visibility()
            self.logger.info(f"main_window_ready components={len(self.load_timings)}", module="oxApp.ui")

        def _build_layout(self) -> None:
            """构造标题栏、会话堆栈、固定三栏主体和底部状态栏。"""
            self._title_bar = TitleBar()
            self._navigation_panel = NavigationPanel()
            self._navigation = self._navigation_panel.compatibility_list_widget  # 旧索引兼容句柄；可见列表由 NavigationPanel 管理。
            self._config_panel = ConfigPanel()
            self._runtime_panel = self._config_panel  # Compatibility alias for status consumers.
            # _pages 保留为稳定兼容句柄，旧业务页和测试可以继续通过它访问中央堆栈。
            self._pages = QStackedWidget()
            self._pages.setObjectName("pageStack")
            # 测试只传 bridge 时使用纯 Python MockTransport；正式入口传入 C++ MockBackendTransport。
            login_client = self.client or ClientProtocolClient(MockTransport())
            self._login_page = LoginPage(login_client, self.bridge, self.logger, self.session)
            self._session_stack = QStackedWidget()
            self._session_stack.setObjectName("sessionStack")
            self._session_stack.addWidget(self._login_page)

            # WorkbenchBody 的三项顺序就是左栏/中央/右栏；中央再分为二级导航和页面堆栈。
            self._secondary_nav = SecondaryNavigation()
            central = QWidget()
            central.setObjectName("centralWorkspace")
            central_layout = QVBoxLayout(central)
            central_layout.setContentsMargins(0, 0, 0, 0)
            central_layout.setSpacing(0)
            central_layout.addWidget(self._secondary_nav)
            central_layout.addWidget(self._pages, 1)

            body = QWidget()
            body.setObjectName("workbenchBody")
            body_layout = QHBoxLayout(body)
            body_layout.setContentsMargins(0, 0, 0, 0)
            body_layout.setSpacing(0)
            body_layout.addWidget(self._navigation_panel)
            body_layout.addWidget(central, 1)
            body_layout.addWidget(self._config_panel)
            self._central_workspace = central
            self._workbench_body = body
            self._session_stack.addWidget(body)

            root = QWidget()
            root.setObjectName("shellRoot")
            root_layout = QVBoxLayout(root)
            root_layout.setContentsMargins(0, 0, 0, 0)
            root_layout.setSpacing(0)
            root_layout.addWidget(self._title_bar)
            root_layout.addWidget(self._session_stack, 1)
            self.setCentralWidget(root)

            # 状态栏仅在登录后显示，左右两侧分别对应原型的连接/任务/消息和版本/日期。
            self._connection_status = QLabel("● 连接正常")
            self._connection_status.setObjectName("statusConnection")
            self._task_status = QLabel("任务：0")
            self._message_status = QLabel("消息：0")
            self._latency_status = QLabel("延迟 -- ms")
            self._theme_status = QLabel("主题：深蓝")
            self._version_status = QLabel("oxApp v0.1.0")
            self._date_status = QLabel(datetime.now().strftime("%Y-%m-%d"))
            status = QStatusBar(self)
            status.setObjectName("statusBar")
            status.setSizeGripEnabled(False)
            status.addWidget(self._connection_status)
            status.addWidget(self._task_status)
            status.addWidget(self._message_status)
            status.addPermanentWidget(self._theme_status)
            status.addPermanentWidget(self._latency_status)
            status.addPermanentWidget(self._version_status)
            status.addPermanentWidget(self._date_status)
            status.showMessage("就绪")
            self.setStatusBar(status)

        def _build_pages(self) -> None:
            """按固定顺序挂载欢迎页、业务页、会话页和日志页。"""
            self._welcome_page = WelcomePage()
            self._add_page(self._welcome_page)
            if self.client is not None:
                # 业务页面复用现有服务和状态容器，只改变它们所在的视觉外壳。
                self._add_page(SystemAdminPage(self.client, self.bridge, self.logger, self.session))
                self._add_page(UserRightsPage(self.client, self.bridge, self.logger, self.session))
                self._add_page(FundManagementPage(self.client, self.bridge, self.logger, self.session))
                self._add_page(RiskManagementPage(self.client, self.bridge, self.logger, self.session))
                self._market_page = MarketPage(self.client, self.bridge, self.logger, self.session, self.market_hub)
                self._add_page(self._market_page)
                self._add_page(TradingPage(self.client, self.bridge, self.logger, self.session))
            else:
                # 无 client 的单元测试仍需要稳定的页面数量，因此放入占位页。
                for _ in range(6):
                    placeholder = QLabel("缺少客户端通信实例")
                    placeholder.setAlignment(Qt.AlignmentFlag.AlignCenter)
                    self._add_page(placeholder)
            self._session_page = SessionInfoPage(self.session)
            self._add_page(self._session_page)
            self._log_panel = LogPanel()
            self._add_page(self._log_panel)

        def _add_page(self, content: QWidget) -> None:
            """把页面加入中央堆栈，并为简单 QLabel 提供统一 PageContainer。"""
            if isinstance(content, ServiceTablePage):
                page = content
            elif isinstance(content, QLabel):
                content.setAlignment(Qt.AlignmentFlag.AlignCenter)
                page = PageContainer(content)
            else:
                page = content
            self._pages.addWidget(page)

        def _connect_signals(self) -> None:
            """集中连接跨组件信号，确保所有路由都经过主窗口。"""
            self.bridge.state_changed.connect(self._show_connection_state)
            self.bridge.state_changed.connect(self._reset_session_on_transport_loss)
            self.bridge.response_received.connect(self._handle_response)
            # 旧列表仍驱动历史 page_index；新可见导航通过 menu_selected 走稳定 ID 路由。
            self._navigation.currentRowChanged.connect(self._select_page)
            self._navigation_panel.menu_selected.connect(self._on_system_menu)
            self._secondary_nav.item_selected.connect(self._on_secondary_menu)
            self._title_bar.primary_menu_selected.connect(self._on_primary_menu)
            self._title_bar.search_submitted.connect(self._on_search)
            self._title_bar.config_requested.connect(self._show_config_panel)
            self._title_bar.theme_requested.connect(self._toggle_theme)
            self._title_bar.account_requested.connect(self._show_config_panel)
            self._title_bar.minimize_requested.connect(self.showMinimized)
            self._title_bar.maximize_requested.connect(self._toggle_maximize)
            self._title_bar.close_requested.connect(self.close)
            self._login_page.maximize_requested.connect(self._toggle_maximize)
            self._config_panel.toggle_requested.connect(self._toggle_config_panel)
            self._navigation_panel.toggle_requested.connect(self._toggle_navigation_panel)
            self._title_bar.navigation_requested.connect(self._toggle_navigation_panel)
            self._config_panel.theme_requested.connect(self._toggle_theme)
            self._config_panel.logout_requested.connect(self._logout)
            self._login_page.session_changed.connect(self._on_session_changed)
            self._login_page.login_state_changed.connect(self._on_login_state)
            self._welcome_page.product_selected.connect(self._on_product_selected)
            self._welcome_page.refresh_requested.connect(self._refresh_current)
            self._welcome_page.add_product_requested.connect(lambda: self._show_status("已打开产品选择"))

        def _select_page(self, index: int) -> None:
            """响应旧列表句柄的行变化，切换中央页面并刷新兼容页。"""
            if 0 <= index < self._pages.count():
                self._pages.setCurrentIndex(index)
                if hasattr(self, "_session_page"):
                    self._session_page.refresh(self.session)
                self._log_panel.append("INFO", "oxApp.ui", f"切换工作区: {self._navigation.item(index).text()}")

        def _on_system_menu(self, item_id: str) -> None:
            """按左侧菜单 ID 更新壳层状态和中央页面索引。"""
            self.state.system_menu = item_id
            item = next((entry for entry in self._navigation_panel._menu_items if entry.item_id == item_id), None)
            primary_for_side = {
                "workbench": "home",
                "market": "market",
                "strategy": "strategy",
                "trade": "trade",
                "products": "admin",
                "system": "admin",
            }
            primary_id = primary_for_side.get(item_id, self.state.primary_menu)
            self.state.primary_menu = primary_id
            self._title_bar.set_active(primary_id)
            self._secondary_nav.set_primary(primary_id)
            if item is not None and item.page_index is not None:
                self._pages.setCurrentIndex(item.page_index)
                self._navigation_panel.select_legacy_page(item.page_index)
            else:
                self._welcome_page.set_context(item.label if item else item_id)
                self._pages.setCurrentWidget(self._welcome_page)
            self._show_status(f"已打开{item.label if item else item_id}")

        def _on_primary_menu(self, item_id: str) -> None:
            """把标题栏一级菜单映射到左侧菜单或中性欢迎占位。"""
            self.state.primary_menu = item_id
            self._secondary_nav.set_primary(item_id)
            labels = {"home": "首页", "market": "行情", "strategy": "策略", "trade": "交易", "research": "研究", "admin": "管理"}
            label = labels.get(item_id, item_id)
            mapping = {"home": "workbench", "market": "market", "strategy": "strategy", "trade": "trade", "admin": "system"}
            side_id = mapping.get(item_id)
            if side_id is not None:
                self._navigation_panel.select(side_id)
            else:
                # 研究等暂未挂载业务页的一级菜单仍保留自己的二级上下文，
                # 左栏只静默回到工作台，不再次触发路由覆盖当前一级菜单。
                self._welcome_page.set_context(label)
                self._navigation_panel.select("workbench", emit=False)
                self._title_bar.set_active(item_id)
                self._secondary_nav.set_primary(item_id)
            self._show_status(f"已切换到{label}")

        def _on_secondary_menu(self, primary_id: str, item_id: str) -> None:
            """二级导航暂不连接业务请求，只反馈当前上下文。"""
            self.state.primary_menu = primary_id
            self._show_status(f"已打开{item_id}")

        def _on_search(self, query: str) -> None:
            """处理全局搜索提交；当前阶段只在状态栏反馈，不伪造搜索结果。"""
            self._show_status(f"搜索：{query}" if query else "请输入搜索内容")

        def _show_config_panel(self) -> None:
            """显示右侧配置面板，并同步折叠按钮状态和壳层状态。"""
            self._config_panel.setVisible(True)
            self.state.right_panel_visible = True
            self._config_panel.panel_toggle_button.setText("›")
            self._config_panel.panel_toggle_button.setToolTip("隐藏配置面板")
            self._sync_panel_geometry()
            self._show_status("系统配置已打开")

        def _toggle_navigation_panel(self) -> None:
            """切换左侧导航展开/收起，保留选中菜单和中央页面状态。"""
            collapsed = not self._navigation_panel.collapsed
            self._navigation_panel.set_collapsed(collapsed)
            self.state.left_navigation_expanded = not collapsed
            self._sync_panel_geometry()
            self._show_status("已收起系统菜单" if collapsed else "已展开系统菜单")

        def _toggle_config_panel(self) -> None:
            """切换右侧面板可见性；中央页面由 Qt layout 自动扩展。"""
            visible = not self.state.right_panel_visible
            self._config_panel.setVisible(visible)
            self.state.right_panel_visible = visible
            self._config_panel.panel_toggle_button.setText("›" if visible else "‹")
            self._config_panel.panel_toggle_button.setToolTip("隐藏配置面板" if visible else "显示配置面板")
            self._sync_panel_geometry()
            self._show_status("已显示右侧配置面板" if visible else "已隐藏右侧配置面板")

        def _sync_panel_geometry(self) -> None:
            """右栏隐藏且左栏收起时左栏不占宽度，中央工作区铺满剩余空间。"""
            hide_left = not self.state.left_navigation_expanded and not self.state.right_panel_visible
            self._navigation_panel.setVisible(not hide_left)

        def _toggle_theme(self) -> None:
            """在深蓝与浅色主题间切换，并保留当前工作台状态。"""
            next_mode = ThemeMode.LIGHT if self.state.theme is ThemeMode.DARK_NAVY else ThemeMode.DARK_NAVY
            self._apply_theme(next_mode)
            self._show_status(f"已切换为{'浅色' if next_mode is ThemeMode.LIGHT else '深蓝'}主题")

        def _apply_theme(self, mode: ThemeMode | str) -> None:
            """应用主题 token 并同步标题栏、右侧配置和状态栏摘要。"""
            if isinstance(mode, ThemeMode):
                requested = mode.value
            else:
                requested = str(mode)
            app = QApplication.instance()
            normalized = apply_workbench_theme(app, requested) if app is not None else requested
            self.state.theme = ThemeMode.LIGHT if normalized == ThemeMode.LIGHT.value else ThemeMode.DARK_NAVY
            label = "浅色" if self.state.theme is ThemeMode.LIGHT else "深蓝"
            self._config_panel.set_theme(self.state.theme.value)
            self._theme_status.setText(f"主题：{label}")
            self._title_bar.set_theme(self.state.theme.value)

        def _on_product_selected(self, name: str, code: str) -> None:
            """保存欢迎页的产品占位选择，供后续业务数据源接入。"""
            self.state.selected_product = name
            self.state.selected_product_code = code
            self._show_status(f"已选择{name}")

        def _refresh_current(self) -> None:
            """刷新当前页面，兼容表格页、业务页和欢迎页三类接口。"""
            page = self._pages.currentWidget()
            if page is self._welcome_page:
                self._show_status("工作区已刷新")
            elif hasattr(page, "_tabs") and hasattr(page, "_load_current"):
                page._load_current(page._tabs.currentIndex())
                self._show_status("当前工作区已刷新")
            elif hasattr(page, "refresh_all"):
                page.refresh_all()
                self._show_status("当前工作区已刷新")
            self._log_panel.append("INFO", "oxApp.ui", "刷新当前工作区")

        def _logout(self) -> None:
            """交给 LoginPage 发起退出请求，不在主窗口伪造认证结果。"""
            self._login_page.logout()
            self._show_status("正在退出登录…")

        def _on_login_state(self, state: str) -> None:
            """将登录页状态字符串转换为壳层枚举并反馈给状态栏。"""
            try:
                self.state.session = SessionState(state)
            except ValueError:
                self.state.session = SessionState.ERROR
            if state == SessionState.LOGGING_IN.value:
                self._show_status("正在登录…")
            elif state == SessionState.LOGGING_OUT.value:
                self._show_status("正在退出登录…")

        def _on_session_changed(self, data: dict) -> None:
            """接收登录/退出会话事件，统一刷新标题栏、配置面板和页面可见性。"""
            user_name = str(data.get("user_name", ""))
            display_name = self._display_name_for_ui(data.get("display_name", ""), user_name)
            session_id = str(data.get("session_id", ""))
            if user_name == "未登录":
                # 退出事件必须清理所有账号展示，并回到登录页。
                self.state.session = SessionState.LOGGED_OUT
                self._config_panel.set_logged_out()
                self._title_bar.set_account("未登录")
                self._welcome_page.set_user("用户")
                self._apply_session_visibility()
                self._show_status("已退出登录")
                self._log_panel.append("INFO", "oxApp.auth", "会话已退出")
                return
            # 登录成功只使用服务返回的非敏感摘要更新 UI。
            self.state.session = SessionState.LOGGED_IN
            self._config_panel.set_user(user_name, display_name, session_id)
            self._title_bar.set_account(user_name, display_name)
            self._welcome_page.set_user(display_name or user_name)
            self._session_page.refresh(self.session)
            self._apply_session_visibility()
            # 行情页在登录前会有一次匿名初始化请求；会话建立后立即重拉真实 QMT/PG 首页。
            if hasattr(self, "_market_page"):
                self._market_page.refresh_for_session()
            self._show_status("登录成功")
            self._log_panel.append("INFO", "oxApp.auth", f"会话已切换: {user_name}")

        def _apply_session_visibility(self) -> None:
            """根据 ClientSession 和 ShellState 的共同结果显示登录页或工作台。"""
            logged_in = self.session.authenticated and self.state.session is SessionState.LOGGED_IN
            self._session_stack.setCurrentIndex(1 if logged_in else 0)
            self._title_bar.setVisible(logged_in)
            self.statusBar().setVisible(logged_in)
            if not self.isMaximized():
                if logged_in:
                    self.setMinimumSize(1180, 720)
                    self.resize(1440, 900)
                else:
                    self.setMinimumSize(820, 520)
                    self.resize(960, 600)
            self._title_bar.set_maximized(self.isMaximized())
            self._login_page.set_maximized(self.isMaximized())

        @staticmethod
        def _display_name_for_ui(display_name: object, user_name: str = "") -> str:
            """清理演示服务的前缀，避免内部实现标签进入正式界面文案。"""
            value = str(display_name or "").strip()
            if value.lower().startswith("mock "):
                value = value[5:].strip()
            return value or str(user_name or "用户").strip() or "用户"

        def _toggle_maximize(self) -> None:
            """在普通和最大化窗口模式之间切换，并同步标题栏图标。"""
            if self.isMaximized():
                self.showNormal()
                QApplication.processEvents()
                self.state.window_maximized = False
            else:
                self.showMaximized()
                QApplication.processEvents()
                self.state.window_maximized = True
            self._title_bar.set_maximized(self.state.window_maximized)

        def _show_connection_state(self, state: str, reason: str) -> None:
            """把 Bridge 的 Transport 状态同步到状态栏、右面板和日志页。"""
            connected = state.lower() in {"connected", "ready", "正常"}
            formal_state = "连接正常" if connected else self._formal_connection_state(state)
            self._connection_status.setText(f"● {formal_state}")
            self._connection_status.setProperty("connected", connected)
            self._connection_status.style().unpolish(self._connection_status)
            self._connection_status.style().polish(self._connection_status)
            self._config_panel.set_connection(state, reason)
            self._log_panel.append("INFO", "oxApp.transport", f"连接状态: {state} | {reason}")

        def _reset_session_on_transport_loss(self, state: str, reason: str) -> None:
            """网络连接失效时清除本地认证态，重连后必须重新登录。"""
            del reason
            if str(state).lower() not in {"disconnected", "reconnecting", "failed", "未连接"}:
                return
            if not self.session.authenticated:
                return
            self.session.logout()  # 服务端已撤销 connection 绑定，客户端不能继续复用旧 Token。
            self.state.session = SessionState.LOGGED_OUT
            self._config_panel.set_logged_out()
            self._title_bar.set_account("未登录")
            self._apply_session_visibility()

        @staticmethod
        def _formal_connection_state(state: str) -> str:
            """将底层连接状态映射为用户可读的正式文案。"""
            value = str(state or "").lower()
            if value in {"connecting", "连接中"}:
                return "连接中"
            if value in {"disconnected", "断开", "closed"}:
                return "未连接"
            return "连接异常"

        def _show_status(self, message: str) -> None:
            """在底部状态栏显示一条短时反馈消息。"""
            self.statusBar().showMessage(message, 3500)

        def _handle_response(self, response: object) -> None:
            """保留统一响应入口；具体请求由各业务页按 request_id 消费。"""
            del response

        def changeEvent(self, event: QEvent) -> None:  # noqa: N802 - Qt API
            """监听系统窗口状态变化，避免外部最大化操作导致按钮状态过期。"""
            super().changeEvent(event)
            if event.type() == QEvent.Type.WindowStateChange and hasattr(self, "_title_bar"):
                self.state.window_maximized = self.isMaximized()
                self._title_bar.set_maximized(self.state.window_maximized)
                self._login_page.set_maximized(self.state.window_maximized)

else:
    class OxAppMainWindow:  # type: ignore[no-redef]
        """无 PySide6 时的明确失败边界。"""

        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
