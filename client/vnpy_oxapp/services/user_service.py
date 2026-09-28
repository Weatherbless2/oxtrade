"""用户、角色、站点和权限关系的服务接口。"""

from __future__ import annotations

from concurrent.futures import Future
from dataclasses import replace
import hashlib
import secrets
from uuid import uuid4

from .base import BaseService
from ..hpsocket_transport import HpSocketTransport
from ..message import ClientResponse
from ..message import ErrorCode, MessageStatus


class UserService(BaseService):
    def __init__(self, client, session_id="mock-session") -> None:
        super().__init__(client, "users", session_id)  # 用户页面的基础资源名。

    def list_users(self, **filters) -> Future[ClientResponse]:
        return self.page(payload=filters)  # 用户列表复用公共分页链路。

    def list_roles(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="roles", func_id="page", payload=filters)  # 角色独立路由，读取动态会话而不是保存旧 session_id。

    def list_sites(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="sites", func_id="page", payload=filters)  # 站点列表由 sites 资源映射到 Mock 快照。

    def list_role_rights(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="role_rights", func_id="page", payload=filters)

    def list_cololist(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="cololist", func_id="page", payload=filters)  # 托管服务配置属于权限域，Mock 仅提供配置/心跳快照。

    def list_user_menu_rights(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="user_menu_rights", func_id="page", payload=filters)

    def list_fund_rights(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="fund_rights", func_id="page", payload=filters)

    def list_project_rights(self, **filters) -> Future[ClientResponse]:
        return self.client.request(session_id=self.current_session_id, msg_type="project_rights", func_id="page", payload=filters)

    def login(self, user_name: str, password: str | None = None) -> Future[ClientResponse]:
        """正式模式执行 challenge/login 两阶段认证，Mock 模式保留旧 users.login 契约。"""
        if not isinstance(getattr(self.client, "transport", None), HpSocketTransport):
            return self.request_resource("users", "login", {
                "user_name": user_name,
                "password": password or "mock-password",
            })  # Mock 登录仍经过请求关联，不把演示成功冒充成正式 Session。

        outer: Future[ClientResponse] = Future()
        outer.request_id = f"auth-login-{uuid4().hex}"
        client_nonce = secrets.token_hex(16)
        challenge_future = self.client.request(
            session_id=self.current_session_id,
            msg_type="auth",
            func_id="challenge",
            payload={"schema_version": 1, "username": user_name, "client_nonce": client_nonce},
        )

        def finish(response: ClientResponse) -> None:
            """把底层 challenge/login 的结果收口为 LoginPage 看到的一次 Future。"""
            if not outer.done():
                outer.set_result(replace(response, request_id=outer.request_id,
                                         msg_type="auth", func_id="login"))

        def on_challenge(done: Future[ClientResponse]) -> None:
            try:
                challenge = done.result()
            except Exception as error:
                outer.set_result(ClientResponse(
                    request_id=outer.request_id,
                    session_id=self.current_session_id,
                    msg_type="auth",
                    func_id="login",
                    status=MessageStatus.FAILED,
                    error_code=ErrorCode.CONNECTION_FAILED,
                    error_message=f"challenge request failed: {error}",
                ))
                return
            if not challenge.is_success:
                finish(challenge)
                return
            challenge_id = str(challenge.payload.get("challenge_id", "")).strip()
            server_nonce = str(challenge.payload.get("server_nonce", "")).strip()
            if not challenge_id or not server_nonce or not password:
                finish(replace(challenge,
                               status=MessageStatus.FAILED,
                               error_code=ErrorCode.VALIDATION_ERROR,
                               error_message="challenge response or password is invalid"))
                return
            password_digest = hashlib.sha256(password.encode("utf-8")).hexdigest()
            proof_input = f"{user_name}:{password_digest}:{challenge_id}:{client_nonce}:{server_nonce}"
            password_proof = hashlib.sha256(proof_input.encode("utf-8")).hexdigest()
            login_future = self.client.request(
                session_id=self.current_session_id,
                msg_type="auth",
                func_id="login",
                payload={
                    "schema_version": 1,
                    "username": user_name,
                    "challenge_id": challenge_id,
                    "client_nonce": client_nonce,
                    "password_proof": password_proof,
                },
            )
            login_future.add_done_callback(on_login)

        def on_login(done: Future[ClientResponse]) -> None:
            try:
                finish(done.result())
            except Exception as error:
                outer.set_result(ClientResponse(
                    request_id=outer.request_id,
                    session_id=self.current_session_id,
                    msg_type="auth",
                    func_id="login",
                    status=MessageStatus.FAILED,
                    error_code=ErrorCode.CONNECTION_FAILED,
                    error_message=f"login request failed: {error}",
                ))

        challenge_future.add_done_callback(on_challenge)
        return outer

    def logout(self, user_name: str) -> Future[ClientResponse]:
        if not isinstance(getattr(self.client, "transport", None), HpSocketTransport):
            return self.request_resource("users", "logout", {"user_name": user_name})  # Mock 注销保持原有页面回归契约。
        session_id, access_token, _expires_at, _permissions, authenticated = self.session_id.auth_snapshot() if hasattr(self.session_id, "auth_snapshot") else (self.current_session_id, "", 0, (), False)
        if not authenticated or not access_token:
            return self._failure("logout", ErrorCode.UNAUTHORIZED, "no authenticated session")
        return self.client.request(
            session_id=session_id,
            msg_type="auth",
            func_id="logout",
            payload={"schema_version": 1, "access_token": access_token},
        )

    def refresh(self) -> Future[ClientResponse]:
        """使用内存 Token 续期；服务端只返回新 Token，不写入磁盘。"""
        if not isinstance(getattr(self.client, "transport", None), HpSocketTransport):
            return self._failure("refresh", ErrorCode.SERVICE_ERROR, "refresh is unavailable in Mock mode")
        session_id, access_token, _expires_at, _permissions, authenticated = self.session_id.auth_snapshot() if hasattr(self.session_id, "auth_snapshot") else (self.current_session_id, "", 0, (), False)
        if not authenticated or not access_token:
            return self._failure("refresh", ErrorCode.UNAUTHORIZED, "no authenticated session")
        return self.client.request(
            session_id=session_id,
            msg_type="auth",
            func_id="refresh",
            payload={"schema_version": 1, "access_token": access_token},
        )

    def _failure(self, operation: str, code: ErrorCode, message: str) -> Future[ClientResponse]:
        future: Future[ClientResponse] = Future()
        future.request_id = f"auth-{operation}-{uuid4().hex}"
        future.set_result(ClientResponse(
            request_id=future.request_id,
            session_id=self.current_session_id,
            msg_type="auth",
            func_id=operation,
            status=MessageStatus.FAILED,
            error_code=code,
            error_message=message,
        ))
        return future
