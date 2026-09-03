"""工作台右侧的账号与配置面板。

本模块只负责展示壳层配置和发出用户意图信号，不直接修改窗口布局、
不保存密码，也不调用认证服务。主窗口接收信号后，再统一更新
``ShellState`` 和会话状态。
"""

from __future__ import annotations

try:
    from PySide6.QtCore import Qt, Signal
    from PySide6.QtWidgets import (
        QCheckBox,
        QComboBox,
        QFormLayout,
        QFrame,
        QHBoxLayout,
        QLabel,
        QPushButton,
        QVBoxLayout,
        QWidget,
    )
    PYSIDE_AVAILABLE = True
except ImportError:
    # PySide6 是可选依赖。核心通信层在无 Qt 环境下仍应能够被单元测试导入。
    PYSIDE_AVAILABLE = False


if PYSIDE_AVAILABLE:
    class ConfigPanel(QFrame):
        """固定宽度的右侧面板。

        面板包含四类信息：当前用户和连接状态、运行环境和主题入口、
        自动刷新/启动欢迎页开关，以及会话编号和退出登录操作。
        ``toggle_requested``、``theme_requested`` 和 ``logout_requested``
        只表示用户意图，具体行为由 ``OxAppMainWindow`` 决定，避免面板
        与窗口策略或后端服务耦合。
        """

        toggle_requested = Signal()
        logout_requested = Signal()
        theme_requested = Signal()

        def __init__(self, parent: QWidget | None = None) -> None:
            """创建面板并填充默认的 Mock 展示状态。"""
            super().__init__(parent)
            self.setObjectName("configPanel")
            self.setFixedWidth(282)  # 三栏原型规定的固定右栏宽度，隐藏时中央区自动扩展。
            self._user_name = "未登录"
            self._display_name = ""
            self._build()

        def _build(self) -> None:
            """构造面板控件，并将按钮连接到语义化信号。"""
            layout = QVBoxLayout(self)
            layout.setContentsMargins(14, 14, 14, 14)
            layout.setSpacing(10)

            # 顶部只保留标题和折叠按钮；折叠行为由主窗口处理。
            head = QHBoxLayout()
            title = QLabel("账号与配置")
            title.setObjectName("panelTitle")
            head.addWidget(title)
            head.addStretch(1)
            toggle = QPushButton("›")
            toggle.setObjectName("panelToggleButton")
            toggle.setToolTip("隐藏配置面板")
            toggle.setFixedSize(28, 28)
            toggle.clicked.connect(self.toggle_requested)
            self._toggle = toggle
            head.addWidget(toggle)
            layout.addLayout(head)

            # 账号卡片展示会话摘要，密码等敏感信息永远不会进入控件。
            profile = QFrame()
            profile.setObjectName("profileCard")
            profile_layout = QHBoxLayout(profile)
            profile_layout.setContentsMargins(12, 10, 12, 10)
            profile_layout.setSpacing(9)
            avatar = QLabel("U")
            avatar.setObjectName("profileAvatar")
            avatar.setAlignment(Qt.AlignmentFlag.AlignCenter)
            avatar.setFixedSize(36, 36)
            profile_text = QVBoxLayout()
            profile_text.setContentsMargins(0, 0, 0, 0)
            profile_text.setSpacing(2)
            self._profile_name = QLabel("未登录")
            self._profile_name.setObjectName("profileName")
            self._profile_status = QLabel("● 未建立会话")
            self._profile_status.setObjectName("profileStatus")
            profile_text.addWidget(self._profile_name)
            profile_text.addWidget(self._profile_status)
            profile_layout.addWidget(avatar)
            profile_layout.addLayout(profile_text)
            layout.addWidget(profile)

            # 通用配置目前是壳层演示项，后续可绑定正式配置存储。
            general = QLabel("通用配置")
            general.setObjectName("sectionKicker")
            layout.addWidget(general)

            form = QFormLayout()
            form.setContentsMargins(4, 0, 4, 0)
            form.setHorizontalSpacing(8)
            form.setVerticalSpacing(8)
            self._environment = QComboBox()
            self._environment.addItems(["Mock / 开发", "测试环境"])
            self._environment.setObjectName("environmentSelect")
            form.addRow("当前环境", self._environment)

            self._theme = QPushButton("深蓝主题")
            self._theme.setObjectName("configValueButton")
            self._theme.clicked.connect(self.theme_requested)
            form.addRow("主题", self._theme)

            self._auto_refresh = QCheckBox("启用")
            self._auto_refresh.setChecked(True)
            self._auto_refresh.setObjectName("configSwitch")
            form.addRow("自动刷新", self._auto_refresh)

            self._welcome = QCheckBox("启用")
            self._welcome.setChecked(True)
            self._welcome.setObjectName("configSwitch")
            form.addRow("启动欢迎页", self._welcome)
            layout.addLayout(form)

            # 会话编号和登录时间只用于帮助验收会话状态，不作为认证凭证。
            session_title = QLabel("会话操作")
            session_title.setObjectName("sectionKicker")
            layout.addWidget(session_title)
            session = QFormLayout()
            session.setContentsMargins(4, 0, 4, 0)
            session.setHorizontalSpacing(8)
            session.setVerticalSpacing(8)
            self._session_id = QLabel("mock-session")
            self._session_id.setObjectName("configValue")
            self._login_time = QLabel("--")
            self._login_time.setObjectName("configValue")
            session.addRow("会话编号", self._session_id)
            session.addRow("登录时间", self._login_time)
            layout.addLayout(session)

            # 明确标记当前边界，避免把 Mock 状态误认为生产认证结果。
            notice = QLabel("本阶段仅使用 Mock 会话，不保存真实密码或连接生产账户。")
            notice.setObjectName("configNotice")
            notice.setWordWrap(True)
            layout.addWidget(notice)
            layout.addStretch(1)

            logout = QPushButton("退出登录")
            logout.setObjectName("logoutButton")
            logout.clicked.connect(self.logout_requested)
            layout.addWidget(logout)

        @property
        def panel_toggle_button(self) -> QPushButton:
            """返回主窗口用于同步按钮方向和提示文字的折叠按钮。"""
            return self._toggle

        def set_user(self, user_name: str, display_name: str = "", session_id: str = "mock-session") -> None:
            """把登录成功后的非敏感会话摘要刷新到面板。"""
            self._user_name = user_name or "未登录"
            self._display_name = display_name or self._user_name
            initial = self._display_name[:1].upper() if self._display_name else "U"
            self._profile_name.setText(self._display_name)
            self._profile_status.setText("● 已连接 · Mock 环境")
            self._profile_status.setProperty("connected", True)
            self._session_id.setText(session_id or "mock-session")
            self._login_time.setText("刚刚")
            # 头像标签没有单独保存引用，通过 objectName 找到它可减少状态字段。
            avatar = self.findChild(QLabel, "profileAvatar")
            if avatar is not None:
                avatar.setText(initial)

        def set_logged_out(self) -> None:
            """清理面板中的用户展示，恢复到未登录状态。"""
            self._profile_name.setText("未登录")
            self._profile_status.setText("● 未建立会话")
            self._profile_status.setProperty("connected", False)
            self._session_id.setText("mock-session")
            self._login_time.setText("--")
            avatar = self.findChild(QLabel, "profileAvatar")
            if avatar is not None:
                avatar.setText("U")

        def set_connection(self, state: str, reason: str = "") -> None:
            """显示 Transport 的连接状态，并刷新 QSS 的 connected 属性。"""
            connected = state.lower() in {"connected", "ready", "正常"}
            suffix = f" · {reason}" if reason else ""
            self._profile_status.setText(f"● {'已连接' if connected else state}{suffix}")
            self._profile_status.setProperty("connected", connected)
            self._profile_status.style().unpolish(self._profile_status)
            self._profile_status.style().polish(self._profile_status)

else:
    class ConfigPanel:  # type: ignore[no-redef]
        """无 PySide6 时的明确失败边界，避免静默创建不可用控件。"""

        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
