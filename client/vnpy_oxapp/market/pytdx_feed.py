"""连接 QuantFabric PyTdx Bridge 的 TCP JSON-lines 客户端。"""

from __future__ import annotations

from dataclasses import dataclass
import json
import socket
from threading import Event, Lock, RLock, Thread, current_thread

from .event import MarketEvent, MarketPayloadError, split_symbol
from .feed import EventCallback, FeedState, IMarketFeed, MarketFeedMetrics, MarketFeedStatus, StateCallback


@dataclass(frozen=True, slots=True)
class PyTdxFeedConfig:
    host: str = "127.0.0.1"
    port: int = 19001
    connect_timeout_seconds: float = 1.0
    reconnect_seconds: float = 2.0
    max_line_bytes: int = 65_536


class PyTdxJsonLineFeed(IMarketFeed):
    """只消费本机 PyTdx Bridge 的 JSON 行，不直接实现通达信私有二进制协议。"""

    def __init__(self, config: PyTdxFeedConfig | None = None) -> None:
        self.config = config or PyTdxFeedConfig()
        self._lock = RLock()
        self._write_lock = Lock()  # 连接建立期重放订阅和 UI 增量订阅可能并发，单独串行化 socket 写入防止 JSON 行交叉。
        self._stop_event = Event()
        self._thread: Thread | None = None
        self._socket: socket.socket | None = None
        self._event_callback: EventCallback | None = None
        self._state_callback: StateCallback | None = None
        self._subscriptions: set[str] = set()
        self._pending = b""  # 半包只能属于当前 TCP 连接；断线后不得与新连接的首包拼接。
        self._status = MarketFeedStatus("pytdx", FeedState.STOPPED, "PyTdx bridge has not started", MarketFeedMetrics())

    @property
    def status(self) -> MarketFeedStatus:
        with self._lock:
            return self._status

    def start(self, event_callback: EventCallback, state_callback: StateCallback) -> None:
        with self._lock:
            if self._thread is not None and self._thread.is_alive():
                return  # start() 幂等，已有接收线程时不能再创建第二条线程争抢同一订阅和 Socket。
            self._event_callback, self._state_callback = event_callback, state_callback  # 回调属于本次运行，行情线程只向上游 Hub 交付标准事件。
            self._stop_event.clear()  # 清除上一次 stop 标记，允许同一个 Feed 在 Mock 回切后重新启动。
            self._thread = Thread(target=self._run, name="oxapp-pytdx-market", daemon=True)  # 独立线程承受阻塞网络 I/O，绝不占用 Qt 主线程。
            self._thread.start()  # 线程拥有连接和重连循环，调用方立即返回继续构建页面。

    def subscribe_symbols(self, symbols: list[str]) -> None:
        commands: list[dict[str, str]] = []
        with self._lock:
            for symbol in symbols:
                normalized = symbol.strip().upper()
                try:
                    ticker, exchange = split_symbol(normalized)
                except ValueError:
                    continue  # PyTdx Bridge 当前只接受沪深六位证券，IF 等期货由正式行情 SDK 处理。
                if normalized not in self._subscriptions:
                    self._subscriptions.add(normalized)  # set 保证重复点击订阅不会重复登记；断线后以它为准完整重放。
                    commands.append({"type": "subscribe", "ticker": ticker, "exchange": exchange})  # 页面代码转换成 Bridge 明确的 JSON 命令字段。
            active_socket = self._socket  # 在状态锁内取得当时连接快照，实际写入再由专用写锁串行。
        if active_socket is not None:
            for command in commands:
                self._send_command(active_socket, command)  # 已连接时即时增量订阅，断线后的完整重放由 _send_subscriptions 负责。

    def stop(self) -> None:
        self._stop_event.set()  # 同时通知连接、recv 超时和退避 wait 三个阻塞边界尽快退出。
        with self._lock:
            active_socket, self._socket = self._socket, None
            thread = self._thread
        if active_socket is not None:
            try:
                active_socket.shutdown(socket.SHUT_RDWR)  # 主动打断另一线程正在执行的 recv/send，不能只等 300ms socket timeout。
            except OSError:
                pass
            active_socket.close()  # 关闭 socket 使 recv 立即退出，不能只等待 timeout 才释放行情线程。
        if thread is not None and thread is not current_thread():
            thread.join(timeout=max(1.0, self.config.connect_timeout_seconds + 1.0))  # 等待 Feed 自己清理连接；禁止当前线程 join 自己造成死锁。
        with self._lock:
            self._thread = None
        self._set_status(FeedState.STOPPED, "PyTdx bridge stopped")

    def _run(self) -> None:
        reconnects = 0
        while not self._stop_event.is_set():
            self._set_status(FeedState.CONNECTING if reconnects == 0 else FeedState.RECONNECTING, "connecting to PyTdx bridge")
            try:
                active_socket = socket.create_connection((self.config.host, self.config.port), self.config.connect_timeout_seconds)  # 标准 TCP connect 带有限超时，Bridge 不存在时可进入重连/回退而非永久挂住。
                active_socket.settimeout(0.3)  # 让 stop/reconnect 可定期检查，避免永久阻塞在 recv。
                with self._lock:
                    self._socket = active_socket
                    self._pending = b""  # 新连接从明确的帧边界开始，防止旧连接残包污染后续 JSON 行。
                self._send_subscriptions(active_socket)  # 每次新连接都重放完整订阅集，因为旧连接上的订阅状态不会自动迁移。
                self._set_status(FeedState.CONNECTED, f"connected to {self.config.host}:{self.config.port}")
                self._receive_lines(active_socket)
            except OSError as error:
                if not self._stop_event.is_set():
                    self._set_status(FeedState.RECONNECTING, f"PyTdx bridge unavailable: {error}")
            finally:
                with self._lock:
                    closing_socket, self._socket = self._socket, None  # 先取消共享句柄发布，再关闭局部对象，新增订阅不会误写已失效连接。
                if closing_socket is not None:
                    try:
                        closing_socket.close()
                    except OSError:
                        pass
            if not self._stop_event.is_set():
                reconnects += 1
                self._increment_reconnects()
                self._stop_event.wait(max(0.05, self.config.reconnect_seconds))  # 有退避等待，禁止连接失败时忙等占满 CPU。

    def _receive_lines(self, active_socket: socket.socket) -> None:
        while not self._stop_event.is_set():
            try:
                block = active_socket.recv(16_384)  # TCP 只保证字节顺序，不保证一条 recv 对应一行 JSON，后续必须自行拆帧。
            except socket.timeout:
                continue
            if not block:
                raise OSError("PyTdx bridge closed the connection")
            self.consume_bytes(block)  # 将任意大小的网络块交给换行帧解析器，半包会留到下一次 recv。

    def consume_bytes(self, block: bytes) -> None:
        """消费 TCP 字节块；显式保留半包直到换行，供 Socket 循环和协议单测共同使用。"""

        pending = getattr(self, "_pending", b"") + block  # 旧半包和新字节先拼接，只有看到换行才形成一条完整协议消息。
        if len(pending) > self.config.max_line_bytes and b"\n" not in pending:
            self._increment_bad_payloads()
            self._pending = b""  # 单行无限增长意味着协议失步，清空以保护客户端内存而不是积压到 OOM。
            return
        while b"\n" in pending:
            raw_line, pending = pending.split(b"\n", 1)  # 一次只剥离首行，循环同时处理同一 TCP 块中的多条粘包消息。
            if len(raw_line) > self.config.max_line_bytes:
                self._increment_bad_payloads()
                continue
            self._handle_line(raw_line)
        self._pending = pending  # 最后一段无换行字节继续归当前连接所有，等待后续 recv 补齐。

    def _handle_line(self, raw_line: bytes) -> None:
        try:
            payload = json.loads(raw_line.decode("utf-8"))  # 先完成 UTF-8 和 JSON 结构校验，外部字节不能直接进入 Hub。
            if not isinstance(payload, dict):
                raise MarketPayloadError("market JSON line must contain an object")
            event = MarketEvent.from_bridge_payload(payload)  # 再执行类型、证券代码、数值范围、五档和时间标准化。
        except (UnicodeDecodeError, json.JSONDecodeError, MarketPayloadError):
            self._increment_bad_payloads()
            return  # 坏包只计数和丢弃，绝不能让单个对端报文终止整个行情接收线程。
        callback = self._event_callback
        if callback is not None:
            callback(event)  # 回调运行在此 Socket 线程；约定只交给 Hub，禁止直接修改 QWidget。
        self._increment_delivered()  # 只有成功解析且完成交付的消息计入 delivered，坏包使用独立计数。

    def _send_subscriptions(self, active_socket: socket.socket) -> None:
        with self._lock:
            symbols = tuple(sorted(self._subscriptions))
        for symbol in symbols:
            ticker, exchange = split_symbol(symbol)
            self._send_command(active_socket, {"type": "subscribe", "ticker": ticker, "exchange": exchange})

    def _send_command(self, active_socket: socket.socket, command: dict[str, str]) -> None:
        payload = (json.dumps(command, separators=(",", ":")) + "\n").encode("utf-8")
        try:
            with self._write_lock:
                active_socket.sendall(payload)  # JSON 行边界由 Bridge 定义；sendall 保证订阅命令不出现半包截断。
        except OSError:
            pass  # 发送失败由读循环/下一轮连接统一进入重连状态，订阅集合仍会在重连后重放。

    def _increment_delivered(self) -> None:
        with self._lock:
            metrics = self._status.metrics
            self._status = MarketFeedStatus(self._status.source, self._status.state, self._status.reason, MarketFeedMetrics(metrics.delivered + 1, metrics.reconnects, metrics.bad_payloads))

    def _increment_reconnects(self) -> None:
        with self._lock:
            metrics = self._status.metrics
            self._status = MarketFeedStatus(self._status.source, self._status.state, self._status.reason, MarketFeedMetrics(metrics.delivered, metrics.reconnects + 1, metrics.bad_payloads))

    def _increment_bad_payloads(self) -> None:
        with self._lock:
            metrics = self._status.metrics
            self._status = MarketFeedStatus(self._status.source, self._status.state, self._status.reason, MarketFeedMetrics(metrics.delivered, metrics.reconnects, metrics.bad_payloads + 1))

    def _set_status(self, state: FeedState, reason: str) -> None:
        with self._lock:
            self._status = MarketFeedStatus("pytdx", state, reason, self._status.metrics)
            status = self._status
            callback = self._state_callback
        if callback is not None:
            callback(status)  # 锁外通知，避免上层状态回调读取 Hub 时发生锁重入或长时间占有 Socket 锁。
