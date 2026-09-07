"""oxApp 未登录欢迎页和会话入口。

页面负责采集并校验账号/密码、提交 ``UserService`` 请求以及把结果
转换成 ``session_changed``/``login_state_changed`` 信号。它不保存明文
密码，也不直接决定主窗口显示哪个页面；会话路由由 ``OxAppMainWindow``
根据 ``ClientSession`` 统一处理。
"""

from __future__ import annotations

from typing import Any

from ..connection import ClientProtocolClient
from ..logging import ClientLogger
from ..message import ClientResponse
from ..services.user_service import UserService
from ..session import ClientSession
from .qt_bridge import AsyncQtBridge
from .shell_state import SessionState

try:
    from PySide6.QtCore import Qt, Signal
    from PySide6.QtWidgets import (
        QCheckBox,
        QFrame,
        QGridLayout,
        QHBoxLayout,
        QLabel,
        QLineEdit,
        QPushButton,
        QVBoxLayout,
        QWidget,
    )
    PYSIDE_AVAILABLE = True
except ImportError:
    # UI 依赖可选；服务和通信模块在无 Qt 环境下仍需可导入和测试。
    PYSIDE_AVAILABLE = False


if PYSIDE_AVAILABLE:
    class LoginPage(QFrame):
        """双栏欢迎/登录页面，保持现有 UserService 边界不变。

        ``session_changed`` 传递登录成功或退出后的非敏感摘要；
        ``login_state_changed`` 传递登录中、退出中、成功和错误等壳层态。
        ``_pending`` 以 request_id 防止重复响应或重复提交污染会话状态。
        """

        session_changed = Signal(dict)
        login_state_changed = Signal(str)

        def __init__(
            self,
            client: ClientProtocolClient,
            bridge: AsyncQtBridge,
            logger: ClientLogger,
            session: ClientSession,
            parent: QWidget | None = None,
        ) -> None:
            """初始化服务适配器、会话引用和异步响应监听。"""
            super().__init__(parent)
            self.setObjectName("loginPage")
            self.bridge = bridge
            self.logger = logger
            self.session = session
            self.service = UserService(client, session)
            # 用请求 ID 跟踪异步操作，登录和注销共用响应回调但分开识别。
            self._pending: set[str] = set()
            self._logout_requests: set[str] = set()
            self._build()
            bridge.response_received.connect(self._on_response)

        def _build(self) -> None:
            """构造原型中的品牌区、登录表单、辅助入口和错误提示。"""
            root = QHBoxLayout(self)
            # 原型规定约 920×560 的居中卡片；外层不拉伸卡片，避免在大屏上变成整页深色布局。
            root.setContentsMargins(0, 0, 0, 0)
            root.setSpacing(0)

            # 外层卡片拆成品牌区与表单区，后续可替换品牌资源而不影响认证逻辑。
            card = QFrame()
            card.setObjectName("loginCard")
            card_layout = QHBoxLayout(card)
            card_layout.setContentsMargins(0, 0, 0, 0)
            card_layout.setSpacing(0)

            brand = QFrame()
            brand.setObjectName("loginBrandPanel")
            brand_layout = QVBoxLayout(brand)
            brand_layout.setContentsMargins(52, 52, 52, 30)
            brand_layout.setSpacing(10)
            mark = QLabel("OX")
            mark.setObjectName("brandMark")
            mark.setAlignment(Qt.AlignmentFlag.AlignCenter)
            mark.setFixedSize(58, 58)
            brand_title = QLabel("欢迎使用 oxApp")
            brand_title.setObjectName("loginBrandTitle")
            brand_text = QLabel("面向量化研究、行情分析和交易管理的一体化工作台。")
            brand_text.setObjectName("loginBrandText")
            brand_text.setWordWrap(True)
            brand_layout.addStretch(1)
            brand_layout.addWidget(mark)
            brand_layout.addSpacing(12)
            brand_layout.addWidget(brand_title)
            brand_layout.addWidget(brand_text)
            brand_layout.addStretch(2)
            version = QLabel("专业工作台 · v1.1")
            version.setObjectName("loginVersion")
            brand_layout.addWidget(version)

            form_panel = QFrame()
            form_panel.setObjectName("loginFormPanel")
            form_layout = QVBoxLayout(form_panel)
            form_layout.setContentsMargins(54, 48, 54, 42)
            form_layout.setSpacing(10)

            # 辅助入口反馈保持简洁，具体能力由后续服务接入。
            tools = QHBoxLayout()
            tools.setSpacing(3)
            tools.addStretch(1)
            for text, tooltip in (("▦", "二维码登录"), ("⚙", "设置"), ("—", "最小化"), ("×", "关闭")):
                tool = QPushButton(text)
                tool.setObjectName("loginToolButton")
                tool.setToolTip(tooltip)
                tool.setFixedSize(30, 28)
                if text == "—":
                    tool.clicked.connect(lambda: self.window().showMinimized())
                elif text == "×":
                    tool.clicked.connect(lambda: self.window().close())
                else:
                    tool.clicked.connect(lambda _checked=False, message=tooltip: self._set_error(message))
                tools.addWidget(tool)
            form_layout.addLayout(tools)

            title = QLabel("登录工作台")
            title.setObjectName("loginFormTitle")
            hint = QLabel("请输入账号和密码以继续")
            hint.setObjectName("loginHint")
            form_layout.addWidget(title)
            form_layout.addWidget(hint)

            tabs = QHBoxLayout()
            tabs.setSpacing(18)
            password_tab = QPushButton("密码登录")
            password_tab.setObjectName("loginTabActive")
            sms_tab = QPushButton("短信登录")
            sms_tab.setObjectName("loginTab")
            password_tab.clicked.connect(lambda: self._select_login_mode("password"))
            sms_tab.clicked.connect(lambda: self._select_login_mode("sms"))
            tabs.addWidget(password_tab)
            tabs.addWidget(sms_tab)
            tabs.addStretch(1)
            form_layout.addLayout(tabs)
            self._password_tab = password_tab
            self._sms_tab = sms_tab

            # 账号和密码只存在于输入控件与当前请求生命周期，不写入 ShellState。
            fields = QGridLayout()
            fields.setVerticalSpacing(7)
            fields.setContentsMargins(0, 8, 0, 0)
            account_label = QLabel("账号")
            account_label.setObjectName("loginLabel")
            self._account = QLineEdit("yangjh")
            self._account.setObjectName("loginInput")
            self._account.setFixedHeight(40)
            self._account.setPlaceholderText("请输入账号")
            password_label = QLabel("密码")
            password_label.setObjectName("loginLabel")
            self._password = QLineEdit()
            self._password.setObjectName("loginInput")
            self._password.setFixedHeight(40)
            self._password.setPlaceholderText("请输入密码")
            self._password.setEchoMode(QLineEdit.EchoMode.Password)
            fields.addWidget(account_label, 0, 0)
            fields.addWidget(self._account, 1, 0)
            fields.addWidget(password_label, 2, 0)
            fields.addWidget(self._password, 3, 0)
            form_layout.addLayout(fields)

            options = QHBoxLayout()
            self._remember = QCheckBox("记住密码")
            self._remember.setObjectName("loginOption")
            self._remember.setChecked(True)
            self._auto_login = QCheckBox("自动登录")
            self._auto_login.setObjectName("loginOption")
            options.addWidget(self._remember)
            options.addStretch(1)
            options.addWidget(self._auto_login)
            form_layout.addLayout(options)

            self._submit = QPushButton("登录")
            self._submit.setObjectName("loginSubmit")
            self._submit.clicked.connect(self._login)
            self._account.returnPressed.connect(self._login)
            self._password.returnPressed.connect(self._login)
            form_layout.addWidget(self._submit)
            self._error = QLabel("")
            self._error.setObjectName("loginError")
            self._error.setWordWrap(True)
            form_layout.addWidget(self._error)

            foot = QHBoxLayout()
            forgot = QPushButton("忘记密码")
            forgot.setObjectName("loginLink")
            forgot.clicked.connect(lambda: self._set_error("请联系管理员重置密码"))
            register = QPushButton("注册账号")
            register.setObjectName("loginLink")
            register.clicked.connect(lambda: self._set_error("请联系管理员开通账号"))
            foot.addWidget(forgot)
            foot.addStretch(1)
            foot.addWidget(QLabel("还没有账号？"))
            foot.addWidget(register)
            form_layout.addLayout(foot)
            form_layout.addStretch(1)

            card_layout.addWidget(brand, 105)
            card_layout.addWidget(form_panel, 95)
            card.setFixedSize(920, 560)
            root.addWidget(card, 0, Qt.AlignmentFlag.AlignCenter)

            self._state = QLabel("未登录")
            self._state.setObjectName("loginState")
            self._state.setVisible(False)

        @property
        def account_input(self) -> QLineEdit:
            """返回账号输入框，供自动化测试和宿主表单控制使用。"""
            return self._account

        @property
        def password_input(self) -> QLineEdit:
            """返回密码输入框；调用方不得读取后持久化其内容。"""
            return self._password

        @property
        def submit_button(self) -> QPushButton:
            """返回登录按钮，便于测试登录中禁用状态。"""
            return self._submit

        @property
        def error_label(self) -> QLabel:
            """返回统一错误提示控件。"""
            return self._error

        def _select_login_mode(self, mode: str) -> None:
            """切换密码/短信登录方式。"""
            password_active = mode == "password"
            self._password_tab.setObjectName("loginTabActive" if password_active else "loginTab")
            self._sms_tab.setObjectName("loginTab" if password_active else "loginTabActive")
            for button in (self._password_tab, self._sms_tab):
                button.style().unpolish(button)
                button.style().polish(button)
            if not password_active:
                self._set_error("短信登录暂不可用，请使用密码登录")
            else:
                self._set_error("")

        def _set_error(self, message: str) -> None:
            """设置错误文案，并在有错误时把壳层状态标记为 error。"""
            self._error.setText(message)
            if message:
                self.login_state_changed.emit(SessionState.ERROR.value)

        def _login(self) -> None:
            """校验输入并异步提交登录请求，避免阻塞 Qt 主线程。"""
            account = self._account.text().strip()
            password = self._password.text().strip()
            if not account or not password:
                self._set_error("请输入账号和密码")
                return
            if self._pending:
                # 已有请求未完成时忽略再次点击，避免同一账号产生多个会话。
                return
            self._error.clear()
            self._submit.setText("登录中…")
            self._submit.setEnabled(False)
            self.login_state_changed.emit(SessionState.LOGGING_IN.value)
            future = self.service.login(account, password)
            request_id = getattr(future, "request_id", "")
            if not request_id:
                self._submit.setText("登录")
                self._submit.setEnabled(True)
                self._set_error("登录请求未生成关联编号")
                return
            self._pending.add(request_id)
            future.add_done_callback(self._publish_future)

        def logout(self) -> None:
            """公开注销入口，实际逻辑交给内部异步方法。"""
            self._logout()

        def _logout(self) -> None:
            """验证当前会话后提交注销请求，不直接清理本地状态。"""
            _session_id, user_name, _display_name, authenticated = self.session.snapshot()
            if not authenticated:
                self._set_error("当前没有已登录会话")
                return
            # 先发布“退出中”，再发送请求：零延迟 MockTransport 可能在调用返回前完成。
            self.login_state_changed.emit(SessionState.LOGGING_OUT.value)
            future = self.service.logout(user_name)
            request_id = getattr(future, "request_id", "")
            if not request_id:
                self._set_error("注销请求未生成关联编号")
                return
            self._pending.add(request_id)
            self._logout_requests.add(request_id)
            future.add_done_callback(self._publish_future)

        def _publish_future(self, future: Any) -> None:
            """在线程池回调中取出结果，再交给 Qt Bridge 排队到主线程。"""
            try:
                self.bridge.publish_response(future.result())
            except Exception as error:
                self.logger.error(f"login_response_publish_failed error={error!r}", module="oxApp.login")

        def _on_response(self, response: ClientResponse) -> None:
            """消费与本页相关的响应，并分别处理登录和注销结果。"""
            if response.request_id not in self._pending:
                # 其他业务页的响应不属于当前登录流程，必须忽略。
                return
            self._pending.discard(response.request_id)
            if response.request_id in self._logout_requests:
                # 注销成功后才清理 ClientSession，失败时保留已登录态便于重试。
                self._logout_requests.discard(response.request_id)
                if response.is_success:
                    self.session.logout()
                    self._state.setText("已退出登录")
                    self.login_state_changed.emit(SessionState.LOGGED_OUT.value)
                    self.session_changed.emit({"user_name": "未登录", "display_name": "", "session_id": "mock-session"})
                else:
                    self._set_error(f"注销失败：{response.error_message}")
                    self.login_state_changed.emit(SessionState.ERROR.value)
                return
            # 登录请求无论成功失败都要恢复按钮，失败时额外清理密码输入。
            self._submit.setText("登录")
            self._submit.setEnabled(True)
            if not response.is_success:
                self._password.clear()
                self._set_error(f"登录失败：{response.error_message}")
                self.login_state_changed.emit(SessionState.ERROR.value)
                return
            # ClientSession 只接收服务返回的会话字段，密码不会写入会话对象。
            self.session.update_from_login(dict(response.payload))
            session_id, user_name, display_name, _ = self.session.snapshot()
            self._state.setText(f"已登录：{display_name or user_name} | 会话：{session_id}")
            self.login_state_changed.emit(SessionState.LOGGED_IN.value)
            self.session_changed.emit({"user_name": user_name, "display_name": display_name, "session_id": session_id})

else:
    class LoginPage:  # type: ignore[no-redef]
        """无 PySide6 时的明确失败边界。"""

        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
