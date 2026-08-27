"""行情源抽象：行情生产线程只交付标准事件，绝不感知 Qt 或业务页面。"""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
from enum import Enum
from typing import Callable

from .event import MarketEvent


class FeedState(str, Enum):
    STOPPED = "stopped"
    CONNECTING = "connecting"
    CONNECTED = "connected"
    RECONNECTING = "reconnecting"
    FAILED = "failed"


@dataclass(frozen=True, slots=True)
class MarketFeedMetrics:
    delivered: int = 0
    reconnects: int = 0
    bad_payloads: int = 0


@dataclass(frozen=True, slots=True)
class MarketFeedStatus:
    source: str
    state: FeedState
    reason: str
    metrics: MarketFeedMetrics


EventCallback = Callable[[MarketEvent], None]
StateCallback = Callable[[MarketFeedStatus], None]


class IMarketFeed(ABC):
    """可替换行情源的最小契约；正式 C++ SDK 接入时沿用该边界。"""

    @property
    @abstractmethod
    def status(self) -> MarketFeedStatus:
        """返回不可变状态快照，调用方不持有数据源内部锁或 Socket。"""

    @abstractmethod
    def start(self, event_callback: EventCallback, state_callback: StateCallback) -> None:
        """启动后台接收；回调运行在行情线程，UI 必须自行切换到 Qt 主线程。"""

    @abstractmethod
    def subscribe_symbols(self, symbols: list[str]) -> None:
        """请求订阅；实现应幂等，重复调用不能重复注册同一标的。"""

    @abstractmethod
    def stop(self) -> None:
        """关闭此数据源拥有的线程、Socket 和定时器，调用方不重复清理资源。"""
