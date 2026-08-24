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
    VALIDATION_ERROR = "validation_error"
    RISK_REJECTED = "risk_rejected"
    NOT_FOUND = "not_found"
    PROTOCOL_ERROR = "protocol_error"
    DUPLICATE_RESPONSE = "duplicate_response"


def unix_time_ms() -> int:
    """返回协议可携带的绝对截止时间，跨线程/跨进程均可解释。"""

    return time.time_ns() // 1_000_000  # 取 Unix 绝对毫秒时间；这个值会随请求跨线程/跨进程传输。


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
        if not self.request_id:  # request_id 是回包找回原 Future 的唯一索引，空值会让关联失效。
            raise ValueError("request_id must not be empty")
        if not self.session_id:  # session_id 绑定登录会话，防止其他会话的同名请求混入当前页面。
            raise ValueError("session_id must not be empty")
        if not self.msg_type:  # msg_type 告诉服务路由进入 users/funds/risk 等哪类业务。
            raise ValueError("msg_type must not be empty")
        if not self.func_id:  # func_id 是服务内操作名，例如 page 或 heartbeat。
            raise ValueError("func_id must not be empty")
        if self.deadline_unix_ms <= 0:  # 绝对截止时间必须可序列化；无截止时间的请求不能被可靠回收。
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
        return self.status is MessageStatus.SUCCEEDED  # 页面只把明确的 succeeded 当作成功，不用 payload 是否为空猜测。

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

        return cls(  # 失败也复制四个关联字段，保证超时/断线结果仍能回到原页面请求。
            request_id=request.request_id,  # 保留原索引，Future 和迟到包缓存按它定位。
            session_id=request.session_id,  # 保留会话边界，避免错误结果被别的登录会话消费。
            msg_type=request.msg_type,  # 让页面知道失败属于哪个业务模块。
            func_id=request.func_id,  # 让页面知道失败发生在哪个操作。
            status=status,  # 区分普通失败、超时和无权限等终态。
            error_code=error_code,  # 把底层原因转换为页面稳定可判断的分类。
            error_message=error_message,  # 保留可读原因供日志和页面提示使用。
        )
