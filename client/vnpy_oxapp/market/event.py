"""将 PyTdx Bridge 的 JSON 行转换为客户端统一行情事件。"""

from __future__ import annotations

from dataclasses import dataclass, replace
from datetime import datetime
from math import isfinite
from typing import Any, Mapping


class MarketPayloadError(ValueError):
    """外部行情字段缺失、类型错误或不满足最小业务语义。"""


_EXCHANGE_SUFFIX = {"SSE": "SH", "SZSE": "SZ"}


def _number(payload: Mapping[str, Any], key: str, *, default: float | None = None) -> float:
    raw = payload.get(key, default)  # Bridge 字段先按原始对象读取；缺省值仅用于协议允许缺失的数值字段。
    if raw is None or isinstance(raw, bool):
        raise MarketPayloadError(f"market field {key!r} must be a number")
    try:
        value = float(raw)  # 统一接收 JSON 整数/浮点/数字字符串，输出稳定的 Python float 给 Hub。
    except (TypeError, ValueError) as error:
        raise MarketPayloadError(f"market field {key!r} must be a number") from error
    if not isfinite(value) or value < 0:  # NaN/Inf 会破坏排序和 K 线计算，负成交量/价格也不进入内部行情模型。
        raise MarketPayloadError(f"market field {key!r} must be a finite non-negative number")
    return value


def _integer(payload: Mapping[str, Any], key: str, *, default: int | None = None) -> int:
    value = _number(payload, key, default=default)
    if int(value) != value:
        raise MarketPayloadError(f"market field {key!r} must be an integer")
    return int(value)


def _levels(payload: Mapping[str, Any], key: str, *, integer: bool) -> tuple[float, ...] | tuple[int, ...]:
    raw = payload.get(key, ())  # 五档可缺省为空，但出现时必须是数组，不能把字符串逐字符当档位。
    if not isinstance(raw, (list, tuple)):
        raise MarketPayloadError(f"market field {key!r} must be an array")
    values: list[float] = []
    for value in raw[:5]:  # 超过五档的数据在本工单模型边界截断，页面不会意外扩张固定五档布局。
        if isinstance(value, bool):
            raise MarketPayloadError(f"market field {key!r} contains an invalid level")
        try:
            parsed = float(value)
        except (TypeError, ValueError) as error:
            raise MarketPayloadError(f"market field {key!r} contains an invalid level") from error
        if not isfinite(parsed) or parsed < 0:
            raise MarketPayloadError(f"market field {key!r} contains an invalid level")
        if integer and int(parsed) != parsed:
            raise MarketPayloadError(f"market field {key!r} contains a non-integer volume")
        values.append(parsed)
    values.extend([0.0] * (5 - len(values)))  # Bridge 少于五档时补零，保证下游可安全使用固定下标 0..4。
    return tuple(int(value) for value in values) if integer else tuple(values)


def split_symbol(symbol: str) -> tuple[str, str]:
    """把 ``600519.SH`` 拆成 PyTdx Bridge 使用的 ticker 和 exchange。"""

    ticker, separator, suffix = symbol.strip().upper().partition(".")  # 页面统一代码形如 600519.SH，订阅协议需要拆成 ticker/exchange。
    exchange = {"SH": "SSE", "SZ": "SZSE"}.get(suffix)  # 只映射当前 Bridge 明确支持的沪深市场，期货不会被猜成股票。
    if not separator or len(ticker) != 6 or not ticker.isdigit() or exchange is None:
        raise ValueError(f"unsupported A-share symbol: {symbol!r}")
    return ticker, exchange


@dataclass(frozen=True, slots=True)
class MarketEvent:
    """统一 Tick 快照；``sequence`` 只能由本机 Hub 分配，外部数据源不能伪造顺序。"""

    symbol: str
    ticker: str
    name: str
    exchange: str
    source: str
    source_time: datetime
    received_at: datetime
    last_price: float
    volume: int
    turnover: float
    pre_close: float
    open_price: float
    high_price: float
    low_price: float
    bid_prices: tuple[float, ...]
    bid_volumes: tuple[int, ...]
    ask_prices: tuple[float, ...]
    ask_volumes: tuple[int, ...]
    sequence: int = 0

    @property
    def source_timestamp_ms(self) -> int:
        """同一交易日内的行情排序键；Bridge 未提供日期时以接收日补齐。"""

        return (
            ((self.source_time.hour * 60 + self.source_time.minute) * 60 + self.source_time.second) * 1_000
            + self.source_time.microsecond // 1_000
        )

    @property
    def update_time(self) -> str:
        return self.source_time.strftime("%H:%M:%S.%f")[:-3]

    @property
    def fingerprint(self) -> tuple[object, ...]:
        """同时间的完全相同快照是重复包，不能重复驱动 UI、Tick 缓冲或 K 线。"""

        return (
            self.source_timestamp_ms, self.last_price, self.volume, self.turnover,
            self.bid_prices, self.bid_volumes, self.ask_prices, self.ask_volumes,
        )

    def with_sequence(self, sequence: int) -> "MarketEvent":
        return replace(self, sequence=sequence)

    @classmethod
    def from_bridge_payload(cls, payload: Mapping[str, Any], *, received_at: datetime | None = None) -> "MarketEvent":
        """解析 QuantFabric PyTdx Bridge 的 ``stock_quote`` JSON，不接受未知行情类型。"""

        if str(payload.get("type", "")).strip() != "stock_quote":
            raise MarketPayloadError("market payload type must be stock_quote")
        ticker = str(payload.get("ticker", "")).strip()
        exchange = str(payload.get("exchange", "")).strip().upper()
        if len(ticker) != 6 or not ticker.isdigit() or exchange not in _EXCHANGE_SUFFIX:
            raise MarketPayloadError("market payload has unsupported ticker or exchange")
        actual_received_at = received_at or datetime.now()  # 本机接收时钟只用于延迟排查和补日期，不冒充交易所时间。
        update_time = str(payload.get("update_time", "")).strip()
        millisec = _integer(payload, "millisec", default=0)
        if not update_time or not 0 <= millisec <= 999:
            raise MarketPayloadError("market payload has invalid update_time or millisec")
        try:
            if "T" in update_time or len(update_time) >= 19:
                parsed_time = datetime.fromisoformat(update_time)  # 兼容后续行情商提供完整 ISO 日期时间的情况。
            else:
                parsed_time = datetime.combine(actual_received_at.date(), datetime.strptime(update_time, "%H:%M:%S").time())  # 旧 Bridge 只有时分秒，因此用接收日补齐排序所需日期。
        except ValueError as error:
            raise MarketPayloadError("market payload has invalid update_time") from error
        source_time = parsed_time.replace(microsecond=millisec * 1_000)  # 协议毫秒转成 datetime 微秒单位，Hub 据此执行同日乱序判断和分钟归桶。
        return cls(
            symbol=f"{ticker}.{_EXCHANGE_SUFFIX[exchange]}",
            ticker=ticker,
            name=str(payload.get("name", ticker)).strip() or ticker,
            exchange=exchange,
            source=str(payload.get("source", "pytdx")).strip() or "pytdx",
            source_time=source_time,
            received_at=actual_received_at,
            last_price=_number(payload, "last_price", default=0),
            volume=_integer(payload, "volume", default=0),
            turnover=_number(payload, "turnover", default=0),
            pre_close=_number(payload, "pre_close", default=0),
            open_price=_number(payload, "open", default=0),
            high_price=_number(payload, "high", default=0),
            low_price=_number(payload, "low", default=0),
            bid_prices=_levels(payload, "bid_prices", integer=False),
            bid_volumes=_levels(payload, "bid_volumes", integer=True),
            ask_prices=_levels(payload, "ask_prices", integer=False),
            ask_volumes=_levels(payload, "ask_volumes", integer=True),
        )
