"""Mock 登录会话页面。"""

from __future__ import annotations

from typing import Any

from ..connection import ClientProtocolClient
from ..logging import ClientLogger
from ..message import ClientResponse
from ..services.user_service import UserService
from ..session import ClientSession
from .qt_bridge import AsyncQtBridge

try:
    from PySide6.QtCore import Signal
    from PySide6.QtWidgets import QComboBox, QFormLayout, QFrame, QLabel, QLineEdit, QPushButton, QVBoxLayout, QWidget
    PYSIDE_AVAILABLE = True
except ImportError:
    PYSIDE_AVAILABLE = False


if PYSIDE_AVAILABLE:
    class LoginPage(QWidget):
        """只模拟登录和会话更新，不保存真实密码，也不宣称已接入真实券商认证。"""

        session_changed = Signal(dict)

        def __init__(self, client: ClientProtocolClient, bridge: AsyncQtBridge, logger: ClientLogger, session: ClientSession, parent=None) -> None:
            super().__init__(parent)
            self.bridge = bridge
            self.logger = logger
            self.session = session
            self.service = UserService(client, session)
            self._pending: set[str] = set()
            self._logout_requests: set[str] = set()
            self._users = QComboBox()
            self._users.addItems(["yangjh", "quant.user02", "quant.user03", "quant.user04"])
            self._password = QLineEdit("mock-password")
            self._password.setEchoMode(QLineEdit.EchoMode.Password)
            submit = QPushButton("登录 Mock 会话")
            logout = QPushButton("退出 Mock 会话")
            submit.clicked.connect(self._login)
            logout.clicked.connect(self._logout)
            self._state = QLabel("未登录。Mock 密码：mock-password")
            self._state.setObjectName("tableSummary")
            form = QFormLayout()
            form.addRow("用户名", self._users)
            form.addRow("密码", self._password)
            form.addRow("", submit)
            form.addRow("", logout)
            panel = QFrame()
            panel.setObjectName("summaryBar")
            panel.setLayout(form)
            layout = QVBoxLayout(self)
            layout.setContentsMargins(24, 24, 24, 24)
            layout.addWidget(QLabel("登录会话"))
            layout.addWidget(panel)
            layout.addWidget(self._state)
            layout.addStretch(1)
            bridge.response_received.connect(self._on_response)

        def _login(self) -> None:
            future = self.service.login(self._users.currentText(), self._password.text())
            request_id = getattr(future, "request_id", "")
            if not request_id:
                self._state.setText("登录请求未生成关联编号")
                return
            self._pending.add(request_id)
            future.add_done_callback(self._publish_future)
            self._state.setText("正在验证 Mock 用户...")

        def _logout(self) -> None:
            session_id, user_name, _display_name, authenticated = self.session.snapshot()
            if not authenticated:
                self._state.setText("当前没有已登录的 Mock 会话")
                return
            future = self.service.logout(user_name)
            request_id = getattr(future, "request_id", "")
            if not request_id:
                self._state.setText("注销请求未生成关联编号")
                return
            self._pending.add(request_id)
            self._logout_requests.add(request_id)
            future.add_done_callback(self._publish_future)
            self._state.setText(f"正在退出会话 {session_id}...")

        def _publish_future(self, future: Any) -> None:
            try:
                self.bridge.publish_response(future.result())
            except Exception as error:
                self.logger.error(f"login_response_publish_failed error={error!r}", module="oxApp.login")

        def _on_response(self, response: ClientResponse) -> None:
            if response.request_id not in self._pending:
                return
            self._pending.discard(response.request_id)
            if response.request_id in self._logout_requests:
                self._logout_requests.discard(response.request_id)
                if response.is_success:
                    self.session.logout()
                    self._state.setText("Mock 会话已退出")
                    self.session_changed.emit({"user_name": "未登录", "display_name": "", "session_id": "mock-session"})
                else:
                    self._state.setText(f"注销失败：{response.error_message}")
                return
            if not response.is_success:
                self._state.setText(f"登录失败：{response.error_message}")
                return
            self.session.update_from_login(dict(response.payload))
            session_id, user_name, display_name, _ = self.session.snapshot()
            self._state.setText(f"已登录：{display_name or user_name} | 会话：{session_id}")
            self.session_changed.emit({"user_name": user_name, "display_name": display_name, "session_id": session_id})
else:
    class LoginPage:  # type: ignore[no-redef]
        def __init__(self, *args, **kwargs) -> None:
            del args, kwargs
            raise RuntimeError("Qt UI requires PySide6; install client/requirements.txt first")
