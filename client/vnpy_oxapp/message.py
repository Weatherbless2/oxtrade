"""工单 #16 的临时客户端消息模型。

这些字段是页面和未来 HP-Socket 适配层之间的稳定契约，不等同于尚未确认的 PackMessage。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Mapping
import time


JsonPayload = Mapping[str, Any]


class MessageStatus(str, Enum):
    """一次客户端请求的最终状态。"""

    PENDING = "pending"
    SUCCEEDED = "succeeded"
    FAILED = "failed"
    TIMED_OUT = "timed_out"
    UNAUTHORIZED = "unauthorized"


class ErrorCode(str, Enum):
    """页面可稳定处理的错误分类，不泄漏未来后台内部错误码。"""

    NONE = "none"
    CONNECTION_FAILED = "connection_failed"
    DISCONNECTED = "disconnected"
    TIMEOUT = "timeout"
    UNAUTHORIZED = "unauthorized"
    SERVICE_ERROR = "service_error"
    PROTOCOL_ERROR = "protocol_error"
    DUPLICATE_RESPONSE = "duplicate_response"


def unix_time_ms() -> int:
    """返回协议可携带的绝对截止时间，跨线程/跨进程均可解释。"""

    return time.time_ns() // 1_000_000


@dataclass(frozen=True, slots=True)
class ClientRequest:
    """页面发出的临时适配请求。"""

    request_id: str
    session_id: str
    msg_type: str
    func_id: str
    payload: JsonPayload = field(default_factory=dict)
    deadline_unix_ms: int = 0

    def __post_init__(self) -> None:
        if not self.request_id:
            raise ValueError("request_id must not be empty")
        if not self.session_id:
            raise ValueError("session_id must not be empty")
        if not self.msg_type:
            raise ValueError("msg_type must not be empty")
        if not self.func_id:
            raise ValueError("func_id must not be empty")
        if self.deadline_unix_ms <= 0:
            raise ValueError("deadline_unix_ms must be a positive absolute timestamp")


@dataclass(frozen=True, slots=True)
class ClientResponse:
    """传输层交回的最终结果，必须带回请求关联字段。"""

    request_id: str
    session_id: str
    msg_type: str
    func_id: str
    status: MessageStatus
    payload: JsonPayload = field(default_factory=dict)
    error_code: ErrorCode = ErrorCode.NONE
    error_message: str = ""

    @property
    def is_success(self) -> bool:
        return self.status is MessageStatus.SUCCEEDED

    @classmethod
    def failure(
        cls,
        request: ClientRequest,
        error_code: ErrorCode,
        error_message: str,
        *,
        status: MessageStatus = MessageStatus.FAILED,
    ) -> "ClientResponse":
        """从原请求复制关联字段，防止错误回包丢失 request/session 对应关系。"""

        return cls(
            request_id=request.request_id,
            session_id=request.session_id,
            msg_type=request.msg_type,
            func_id=request.func_id,
            status=status,
            error_code=error_code,
            error_message=error_message,
        )
