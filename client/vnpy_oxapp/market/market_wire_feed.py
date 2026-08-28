"""oxMBS MarketWire 的客户端 Feed 适配；Socket 生命周期由上层传输适配器负责。"""

from __future__ import annotations

import struct
from dataclasses import replace
from datetime import datetime
from math import isfinite
from threading import RLock

from .event import MarketEvent, MarketPayloadError
from .feed import EventCallback, FeedState, IMarketFeed, MarketFeedMetrics, MarketFeedStatus, StateCallback


MAGIC = 0x4F584D57
VERSION = 1
HEADER_SIZE = 100
MAX_FRAME_SIZE = 1024 * 1024
PAYLOAD_SIZE = 8 + 8 + 8 + 5 * 16 + 5 * 16
QUOTE_MESSAGE_TYPE = 1
_HEADER_PREFIX = struct.Struct("<IHHIHHQQQQIHHI")


class MarketWireError(ValueError):
    """MarketWire 固定头、长度或 Quote 载荷不满足协议约束。"""


class IncompleteMarketWire(MarketWireError):
    """等待更多 TCP/HP-Socket 字节，不应被当成坏包。"""


def _normalize_symbol(symbol: str) -> str:
    ticker, separator, exchange = symbol.strip().upper().partition(".")
    exchange = {"SSE": "SH", "SZSE": "SZ"}.get(exchange, exchange)
    if not separator or not ticker or not exchange or any(not (char.isalnum() or char in "_-") for char in ticker):
        raise ValueError(f"invalid market symbol: {symbol!r}")
    return f"{ticker}.{exchange}"


def decode_frame(data: bytes | bytearray | memoryview) -> tuple[dict[str, int | str], bytes, int]:
    """解出一帧并返回 ``(header, payload, consumed)``；数据不足时抛出 Incomplete。"""

    raw = bytes(data)
    if len(raw) < 12:
        raise IncompleteMarketWire("market wire header is incomplete")
    prefix = _HEADER_PREFIX.unpack_from(raw, 0)
    magic, version, header_size, frame_size, message_type, flags, request_id, session_id, source_sequence, event_time_ns, payload_size, instrument_size, exchange_size, _ = prefix
    if magic != MAGIC:
        raise MarketWireError("market wire magic mismatch")
    if version != VERSION or header_size != HEADER_SIZE:
        raise MarketWireError("market wire version or header size mismatch")
    if frame_size < HEADER_SIZE:
        raise MarketWireError("market wire frame size is smaller than header")
    if frame_size > MAX_FRAME_SIZE:
        raise MarketWireError("market wire frame exceeds maximum size")
    if len(raw) < frame_size:
        raise IncompleteMarketWire("market wire frame is incomplete")
    if payload_size != frame_size - HEADER_SIZE:
        raise MarketWireError("market wire payload size does not match frame size")
    if not 1 <= instrument_size <= 31 or not 1 <= exchange_size <= 7:
        raise MarketWireError("market wire text field length is outside limit")
    instrument = raw[60:92][:instrument_size].decode("ascii", errors="strict")
    exchange = raw[92:100][:exchange_size].decode("ascii", errors="strict")
    return ({
        "message_type": message_type,
        "flags": flags,
        "request_id": request_id,
        "session_id": session_id,
        "source_sequence": source_sequence,
        "event_time_ns": event_time_ns,
        "instrument_id": instrument,
        "exchange": exchange,
    }, raw[HEADER_SIZE:frame_size], frame_size)


class MarketWireFeed(IMarketFeed):
    """把已建立传输连接交付的 MarketWire 字节转换为统一 ``MarketEvent``。"""

    def __init__(self, source: str = "oxMWH", *, max_buffer_size: int = 4 * MAX_FRAME_SIZE) -> None:
        self.source = source
        self.max_buffer_size = max(MAX_FRAME_SIZE, max_buffer_size)
        self._lock = RLock()
        self._buffer = bytearray()
        self._subscriptions: set[str] = set()
        self._last_sequences: dict[str, int] = {}
        self._event_callback: EventCallback | None = None
        self._state_callback: StateCallback | None = None
        self._status = MarketFeedStatus(source, FeedState.STOPPED, "MarketWire feed has not started", MarketFeedMetrics())

    @property
    def status(self) -> MarketFeedStatus:
        with self._lock:
            return self._status

    def start(self, event_callback: EventCallback, state_callback: StateCallback) -> None:
        with self._lock:
            self._event_callback = event_callback
            self._state_callback = state_callback
            self._buffer.clear()
            self._last_sequences.clear()
            self._set_status_locked(FeedState.CONNECTED, "MarketWire transport is ready")
            status = self._status
        if state_callback is not None:
            state_callback(status)  # 传输连接由外部注入；Feed 只发布统一状态，不伪造 HP-Socket 建连过程。

    def subscribe_symbols(self, symbols: list[str]) -> None:
        normalized = {_normalize_symbol(symbol) for symbol in symbols}
        with self._lock:
            self._subscriptions.update(normalized)  # 服务端也会过滤，但客户端保留一道闸门防止误推送污染页面。

    def stop(self) -> None:
        with self._lock:
            self._event_callback = None
            self._state_callback = None
            self._buffer.clear()
            self._last_sequences.clear()
            self._set_status_locked(FeedState.STOPPED, "MarketWire feed stopped")

    def feed_bytes(self, data: bytes | bytearray | memoryview) -> bool:
        """注入网络收到的字节；返回 False 表示协议坏包或 Feed 未启动。"""

        callbacks: list[MarketEvent] = []
        state_callback: StateCallback | None = None
        state: MarketFeedStatus | None = None
        callback: EventCallback | None = None
        with self._lock:
            if self._event_callback is None:
                return False
            if len(self._buffer) + len(data) > self.max_buffer_size:
                self._buffer.clear()  # 失去帧边界时清流，等待传输层重连，不能无限保留攻击性输入。
                self._record_bad_payload_locked("MarketWire receive buffer exceeded limit")
                state_callback, state = self._state_callback, self._status
                ok = False
            else:
                self._buffer.extend(data)
                ok = True
                while self._buffer:
                    try:
                        header, payload, consumed = decode_frame(self._buffer)
                    except IncompleteMarketWire:
                        break  # 半包留在缓冲区，下一次 feed_bytes 继续解码。
                    except (MarketWireError, UnicodeError, struct.error) as error:
                        self._buffer.clear()  # 坏头无法可靠重同步，交由外部传输层断线重连。
                        self._record_bad_payload_locked(str(error))
                        state_callback, state = self._state_callback, self._status
                        ok = False
                        break
                    del self._buffer[:consumed]  # 粘包只消费一帧，后续帧留在循环中继续处理。
                    try:
                        event = self._decode_quote(header, payload)
                    except (MarketWireError, MarketPayloadError, ValueError, OverflowError) as error:
                        self._record_bad_payload_locked(str(error))
                        state_callback, state = self._state_callback, self._status
                        ok = False
                        continue
                    source_sequence = int(header["source_sequence"])
                    previous = self._last_sequences.get(event.symbol)
                    if source_sequence and previous is not None and source_sequence <= previous:
                        self._record_bad_payload_locked("duplicate or out-of-order market sequence")
                        continue  # 服务端重启或网络重放的旧 Tick 不得覆盖客户端当前快照。
                    if source_sequence:
                        self._last_sequences[event.symbol] = source_sequence
                    if self._subscriptions and event.symbol not in self._subscriptions:
                        continue
                    callbacks.append(event)
                    self._status = replace(self._status, metrics=replace(self._status.metrics, delivered=self._status.metrics.delivered + 1))
                callback = self._event_callback
        for event in callbacks:
            if callback is not None:
                callback(event)  # 回调在锁外执行，不能阻塞后续收包或造成 Feed 重入死锁。
        if state_callback is not None and state is not None:
            state_callback(state)
        return ok

    def _decode_quote(self, header: dict[str, int | str], payload: bytes) -> MarketEvent:
        if header["message_type"] != QUOTE_MESSAGE_TYPE:
            raise MarketWireError("MarketWire feed only accepts Quote messages")
        if len(payload) != PAYLOAD_SIZE:
            raise MarketWireError("market quote payload length is invalid")
        symbol = _normalize_symbol(str(header["instrument_id"]))
        source_time = datetime.fromtimestamp(int(header["event_time_ns"]) / 1_000_000_000)
        last_price, volume, turnover = struct.unpack_from("<dQd", payload, 0)
        offset = 24
        bid_prices: list[float] = []
        bid_volumes: list[int] = []
        ask_prices: list[float] = []
        ask_volumes: list[int] = []
        for prices, volumes in ((bid_prices, bid_volumes), (ask_prices, ask_volumes)):
            for _ in range(5):
                price, level_volume = struct.unpack_from("<dQ", payload, offset)
                prices.append(price)
                volumes.append(level_volume)
                offset += 16
        values = (last_price, turnover, *bid_prices, *ask_prices)
        if not all(isfinite(value) and value >= 0 for value in values) or last_price <= 0:
            raise MarketWireError("market quote contains a non-finite or invalid price")
        return MarketEvent(
            symbol=symbol,
            ticker=symbol.split(".", 1)[0],
            name=symbol,
            exchange=str(header["exchange"]),
            source=self.source,
            source_time=source_time,
            received_at=datetime.now(),
            last_price=last_price,
            volume=int(volume),
            turnover=turnover,
            pre_close=0.0,
            open_price=0.0,
            high_price=0.0,
            low_price=0.0,
            bid_prices=tuple(bid_prices),
            bid_volumes=tuple(bid_volumes),
            ask_prices=tuple(ask_prices),
            ask_volumes=tuple(ask_volumes),
        )

    def _set_status_locked(self, state: FeedState, reason: str) -> None:
        self._status = MarketFeedStatus(self.source, state, reason, self._status.metrics)

    def _record_bad_payload_locked(self, reason: str) -> None:
        metrics = replace(self._status.metrics, bad_payloads=self._status.metrics.bad_payloads + 1)
        self._status = MarketFeedStatus(self.source, FeedState.FAILED, reason, metrics)


__all__ = ["HEADER_SIZE", "MAGIC", "MarketWireError", "MarketWireFeed", "decode_frame"]
