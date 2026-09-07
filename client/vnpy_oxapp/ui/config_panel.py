"""工作台右侧的账号与配置面板。

本模块只负责展示壳层配置和发出用户意图信号，不直接修改窗口布局、
不保存密码，也不调用认证服务。主窗口接收信号后，再统一更新状态和会话。
"""

from __future__ import annotations

try:
    from PySide6.QtCore import QRectF, Qt, Signal
    from PySide6.QtGui import QColor, QPainter
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
    PYSIDE_AVAILABLE = False


if PYSIDE_AVAILABLE:
    class SwitchControl(QCheckBox):
        """轻量自绘开关，视觉上与原型的 34×19px switch 对齐。"""

        def __init__(self, text: str = "启用", parent: QWidget | None = None) -> None:
            super().__init__(text, parent)
            self.setObjectName("configSwitch")
            self.setCursor(Qt.CursorShape.PointingHandCursor)
            self.setFixedSize(72, 24)
            self.toggled.connect(self.update)

        def paintEvent(self, event) -> None:  # noqa: N802 - Qt API
            """绘制轨道、圆点和右侧状态文字，不依赖平台原生 checkbox 外观。"""
            del event
            painter = QPainter(self)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            track = QRectF(0, 3, 34, 18)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor("#1e84ba" if self.isChecked() else "#304762"))
            painter.drawRoundedRect(track, 9, 9)
            knob_x = 17 if self.isChecked() else 2
            painter.setBrush(QColor("#f5fbff" if self.isChecked() else "#8da2bb"))
            painter.drawEllipse(QRectF(knob_x, 5, 14, 14))
            painter.setPen(QColor("#b9cce1"))
            painter.drawText(41, 0, 30, 24, Qt.AlignmentFlag.AlignVCenter, self.text())


    class ConfigPanel(QFrame):
        """固定宽度的右侧账号与配置面板。

        面板只展示非敏感会话摘要和壳层配置；所有布局、主题和退出
        行为通过语义化信号交给主窗口处理。
        """

        toggle_requested = Signal()
        logout_requested = Signal()
        theme_requested = Signal()

        def __init__(self, parent: QWidget | None = None) -> None:
            """创建面板并填充默认的正式业务状态。"""
            super().__init__(parent)
            self.setObjectName("configPanel")
            self.setFixedWidth(282)
            self._user_name = "未登录"
            self._display_name = ""
            self._build()

        def _build(self) -> None:
            """构造面板控件，并将按钮连接到语义化信号。"""
            layout = QVBoxLayout(self)
            layout.setContentsMargins(14, 14, 14, 14)
            layout.setSpacing(10)

            # 顶部只保留标题和折叠按钮，折叠策略由主窗口统一处理。
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

            # 账号卡片只显示显示名、连接状态和头像首字母，绝不显示密码。
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

            # 通用配置暂时是壳层展示项，后续可绑定正式配置存储。
            general = QLabel("通用配置")
            general.setObjectName("sectionKicker")
            layout.addWidget(general)

            form = QFormLayout()
            form.setContentsMargins(4, 0, 4, 0)
            form.setHorizontalSpacing(8)
            form.setVerticalSpacing(8)
            self._environment = QComboBox()
            self._environment.addItems(["正式环境", "测试环境"])
            self._environment.setObjectName("environmentSelect")
            form.addRow("当前环境", self._environment)

            self._theme = QPushButton("深蓝")
            self._theme.setObjectName("configValueButton")
            self._theme.clicked.connect(self.theme_requested)
            form.addRow("主题", self._theme)

            self._auto_refresh = SwitchControl()
            self._auto_refresh.setChecked(True)
            form.addRow("自动刷新", self._auto_refresh)

            self._welcome = SwitchControl()
            self._welcome.setChecked(True)
            form.addRow("启动欢迎页", self._welcome)
            layout.addLayout(form)

            # 会话编号和登录时间只展示非敏感摘要。
            session_title = QLabel("会话操作")
            session_title.setObjectName("sectionKicker")
            layout.addWidget(session_title)
            session = QFormLayout()
            session.setContentsMargins(4, 0, 4, 0)
            session.setHorizontalSpacing(8)
            session.setVerticalSpacing(8)
            self._session_id = QLabel("—")
            self._session_id.setObjectName("configValue")
            self._login_time = QLabel("--")
            self._login_time.setObjectName("configValue")
            session.addRow("会话编号", self._session_id)
            session.addRow("登录时间", self._login_time)
            layout.addLayout(session)

            layout.addStretch(1)

            logout = QPushButton("退出登录")
            logout.setObjectName("logoutButton")
            logout.clicked.connect(self.logout_requested)
            layout.addWidget(logout)

        @property
        def panel_toggle_button(self) -> QPushButton:
            """返回主窗口用于同步按钮方向和提示文字的折叠按钮。"""
            return self._toggle

        def set_user(self, user_name: str, display_name: str = "", session_id: str = "") -> None:
            """把登录成功后的非敏感会话摘要刷新到面板。"""
            self._user_name = user_name or "未登录"
            self._display_name = display_name or self._user_name
            initial = self._display_name[:1].upper() if self._display_name else "U"
            self._profile_name.setText(self._display_name)
            self._profile_status.setText("● 已连接")
            self._profile_status.setProperty("connected", True)
            self._session_id.setText(self._safe_session_id(session_id))
            self._login_time.setText("刚刚")
            avatar = self.findChild(QLabel, "profileAvatar")
            if avatar is not None:
                avatar.setText(initial)

        def set_logged_out(self) -> None:
            """清理面板中的用户展示，恢复到未登录状态。"""
            self._profile_name.setText("未登录")
            self._profile_status.setText("● 未建立会话")
            self._profile_status.setProperty("connected", False)
            self._session_id.setText("—")
            self._login_time.setText("--")
            avatar = self.findChild(QLabel, "profileAvatar")
            if avatar is not None:
                avatar.setText("U")

        def set_connection(self, state: str, reason: str = "") -> None:
            """显示 Transport 的连接状态，并刷新 QSS 的 connected 属性。"""
            connected = state.lower() in {"connected", "ready", "正常"}
            del reason
            self._profile_status.setText(f"● {'已连接' if connected else self._formal_connection_state(state)}")
            self._profile_status.setProperty("connected", connected)
            self._profile_status.style().unpolish(self._profile_status)
            self._profile_status.style().polish(self._profile_status)

        def set_theme(self, mode: str) -> None:
            """同步右侧主题选择显示，不重置其他配置。"""
            self._theme.setText("浅色" if str(mode).lower() == "light" else "深蓝")

        @staticmethod
        def _safe_session_id(session_id: str) -> str:
            """把内部会话标识转换为用户可读的摘要。"""
            value = str(session_id or "").strip()
            return "当前会话" if not value or value.lower().startswith("mock") else value

        @staticmethod
        def _formal_connection_state(state: str) -> str:
            """将传输层状态映射为正式界面文案。"""
            value = str(state or "").lower()
            if value in {"connecting", "连接中"}:
                return "连接中"
            if value in {"disconnected", "断开", "closed"}:
                return "未连接"
            return "连接异常"

else:
    class ConfigPanel:  # type: ignore[no-redef]
        """无 PySide6 时的明确失败边界。"""

        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
