"""客户端会话快照。

Mock 阶段只保存临时登录资料；正式接入时由真实登录令牌和权限声明替换，
页面和服务仍通过本对象读取会话标识，避免各页面写死 session_id。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from threading import RLock
from typing import Any


@dataclass
class ClientSession:
    """跨页面共享的会话边界，不保存真实密码或资金密码。"""

    session_id: str = "mock-session"
    user_name: str = "未登录"
    display_name: str = ""
    authenticated: bool = False
    _lock: RLock = field(default_factory=RLock, repr=False)

    def update_from_login(self, payload: dict[str, Any]) -> None:
        user = payload.get("user", {})
        if not isinstance(user, dict):
            return
        session_id = str(payload.get("session_id", "")).strip()
        user_name = str(user.get("user_name", "")).strip()
        if not session_id or not user_name:
            return
        with self._lock:
            self.session_id = session_id  # 后续请求带同一 session_id，RequestTracker 继续校验回包会话边界。
            self.user_name = user_name
            self.display_name = str(user.get("display_name", user_name))
            self.authenticated = True

    def snapshot(self) -> tuple[str, str, str, bool]:
        with self._lock:
            return self.session_id, self.user_name, self.display_name, self.authenticated

    def logout(self) -> None:
        with self._lock:
            self.session_id = "mock-session"  # 注销后回到临时匿名会话，正式令牌由后续权限服务重新签发。
            self.user_name = "未登录"
            self.display_name = ""
            self.authenticated = False
