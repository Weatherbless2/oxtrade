"""客户端传输抽象。

页面和服务只依赖该接口；MockTransport 与未来 HpSocketTransport 可以无缝替换。
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from enum import Enum
from typing import Callable

from .message import ClientRequest, ClientResponse, ErrorCode


class ConnectionState(str, Enum):
    DISCONNECTED = "disconnected"  # 没有可用链路；send 必须返回 False。
    CONNECTING = "connecting"  # 正在建立链路，页面显示过渡态而不是可发送态。
    CONNECTED = "connected"  # 允许 send，请求才会进入 Mock/未来 Socket。
    RECONNECTING = "reconnecting"  # 断线恢复过程，不能把中间态当成成功。
    FAILED = "failed"  # 连接建立失败，错误通过 ErrorCallback 解释。


ResponseCallback = Callable[[ClientResponse], None]
StateCallback = Callable[[ConnectionState, str], None]
ErrorCallback = Callable[[ErrorCode, str], None]


class IClientTransport(ABC):
    """异步请求传输的最小接口。"""

    @property
    @abstractmethod
    def state(self) -> ConnectionState:
        """当前连接状态；读取方不拥有底层 Socket。"""

    @abstractmethod
    def set_response_callback(self, callback: ResponseCallback) -> None:
        """设置后台线程收到完整响应时调用的回调，回调方负责线程切换。"""

    @abstractmethod
    def set_state_callback(self, callback: StateCallback) -> None:
        """设置连接状态变化回调，状态和原因一起交给上层。"""

    @abstractmethod
    def set_error_callback(self, callback: ErrorCallback) -> None:
        """设置传输层不可关联错误的回调，例如尚无 request_id 的连接失败。"""

    @abstractmethod
    def connect(self) -> bool:
        """发起连接；成功时必须进入 CONNECTED 状态。"""

    @abstractmethod
    def disconnect(self) -> None:
        """主动断开并释放该传输拥有的后台资源，调用者不重复销毁。"""

    @abstractmethod
    def send(self, request: ClientRequest) -> bool:
        """异步发送请求；False 代表请求没有进入传输层，客户端必须补失败结果。"""
