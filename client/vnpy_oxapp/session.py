"""客户端会话快照。

正式 HP-Socket 模式只把 Session、Token、过期时间和权限保存在进程内；
页面仍通过本对象读取会话标识，避免各页面写死 session_id 或接触密码。
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
    access_token: str = ""
    expires_at_unix_ms: int = 0
    permissions: tuple[str, ...] = ()
    _lock: RLock = field(default_factory=RLock, repr=False)

    def update_from_login(self, payload: dict[str, Any]) -> None:
        user = payload.get("user", {})
        if not isinstance(user, dict):
            user = {}
        session_id = str(payload.get("session_id", "")).strip()
        user_name = str(user.get("user_name", "")).strip()
        access_token = str(payload.get("access_token", "")).strip()
        if not session_id or not user_name:
            return
        if not access_token and payload.get("source") != "mock":
            return
        permissions = payload.get("permissions", [])
        if not isinstance(permissions, (list, tuple)):
            permissions = []
        expires_at = payload.get("expires_at_unix_ms", 0)
        try:
            expires_at_unix_ms = int(expires_at)
        except (TypeError, ValueError):
            expires_at_unix_ms = 0
        with self._lock:
            self.session_id = session_id  # 后续请求带同一 session_id，RequestTracker 继续校验回包会话边界。
            self.user_name = user_name
            self.display_name = str(user.get("display_name", user_name))
            self.access_token = access_token  # Token 只在内存中使用，不写入配置、日志或 Qt 文案。
            self.expires_at_unix_ms = expires_at_unix_ms
            self.permissions = tuple(str(item) for item in permissions if str(item).strip())
            self.authenticated = True

    def snapshot(self) -> tuple[str, str, str, bool]:
        with self._lock:
            return self.session_id, self.user_name, self.display_name, self.authenticated

    def logout(self) -> None:
        with self._lock:
            self.session_id = "mock-session"  # 注销后回到临时匿名会话，正式令牌由后续权限服务重新签发。
            self.user_name = "未登录"
            self.display_name = ""
            self.access_token = ""
            self.expires_at_unix_ms = 0
            self.permissions = ()
            self.authenticated = False

    def auth_snapshot(self) -> tuple[str, str, int, tuple[str, ...], bool]:
        """返回服务层需要的认证字段；调用方不得把 Token 写入日志或界面。"""
        with self._lock:
            return (self.session_id, self.access_token, self.expires_at_unix_ms,
                    self.permissions, self.authenticated)
