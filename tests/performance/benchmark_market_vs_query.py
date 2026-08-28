#!/usr/bin/env python3
"""比较 PyTdx 行情链路与 C++ Mock 低频查询链路的可观测耗时。

本工具是独立验收程序，不修改生产对象的实现。行情源时间由当前 Bridge 在完成
PyTdx 拉取后生成，因此 ``bridge_to_feed`` 只代表“Bridge 发布到 oxApp 收到”，
不能解释为交易所到客户端延迟。
"""

from __future__ import annotations

import argparse
from concurrent.futures import Future
from dataclasses import asdict, dataclass, replace
from datetime import datetime, timedelta
from io import StringIO
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
from threading import Event, Lock, Thread
import time
from typing import Any, Callable, Sequence


PROJECT_ROOT = Path(__file__).resolve().parents[2]
CLIENT_ROOT = PROJECT_ROOT / "client"
if str(CLIENT_ROOT) not in sys.path:
    sys.path.insert(0, str(CLIENT_ROOT))  # 直接运行脚本时使用当前工作区源码，而不是环境中可能残留的旧安装包。
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")  # 性能验收不显示窗口，但仍使用真实 QApplication 和 QWidget 事件循环。

from vnpy_oxapp.connection import ClientProtocolClient  # noqa: E402
from vnpy_oxapp.logging import ClientLogger  # noqa: E402
from vnpy_oxapp.market.event import MarketEvent  # noqa: E402
from vnpy_oxapp.market.feed import FeedState, IMarketFeed, MarketFeedMetrics, MarketFeedStatus  # noqa: E402
from vnpy_oxapp.market.hub import MarketHub  # noqa: E402
from vnpy_oxapp.market.pytdx_feed import PyTdxFeedConfig, PyTdxJsonLineFeed  # noqa: E402
from vnpy_oxapp.mock.mock_backend_transport import MockBackendTransport, MockBackendTransportConfig  # noqa: E402
from vnpy_oxapp.services.market_service import MarketService  # noqa: E402
from vnpy_oxapp.services.user_service import UserService  # noqa: E402


@dataclass(frozen=True, slots=True)
class Distribution:
    samples: int
    minimum_ms: float
    mean_ms: float
    p50_ms: float
    p95_ms: float
    p99_ms: float
    maximum_ms: float


def distribution(values: Sequence[float]) -> Distribution:
    """使用最近秩百分位，避免少量样本通过插值产生实际未观测到的延迟。"""

    if not values:
        raise ValueError("cannot summarize an empty sample set")
    ordered = sorted(values)

    def percentile(percent: float) -> float:
        index = max(0, math.ceil(percent * len(ordered)) - 1)  # P99 取覆盖至少 99% 样本的实际观测值。
        return ordered[index]

    return Distribution(
        samples=len(ordered),
        minimum_ms=ordered[0],
        mean_ms=statistics.fmean(ordered),
        p50_ms=percentile(0.50),
        p95_ms=percentile(0.95),
        p99_ms=percentile(0.99),
        maximum_ms=ordered[-1],
    )


class BenchmarkFeed(PyTdxJsonLineFeed):
    """只在测试进程记录协议行入口时间，不改变父类的解析、校验和交付逻辑。"""

    def __init__(self, config: PyTdxFeedConfig) -> None:
        super().__init__(config)
        self.line_started_ns = 0

    def _handle_line(self, raw_line: bytes) -> None:
        self.line_started_ns = time.perf_counter_ns()  # 起点是 Feed 已取得一条完整 JSON 行，TCP 等待不包含在解析耗时内。
        super()._handle_line(raw_line)  # 仍调用生产实现，坏包闸门、标准化和 delivered 计数没有测试替身。


class ManualFeed(IMarketFeed):
    """Qt 合并刷新只需可控事件，不额外创建 Socket 线程污染页面耗时。"""

    def __init__(self) -> None:
        self._status = MarketFeedStatus("benchmark", FeedState.CONNECTED, "controlled input", MarketFeedMetrics())

    @property
    def status(self) -> MarketFeedStatus:
        return self._status

    def start(self, event_callback, state_callback) -> None:
        del event_callback
        state_callback(self._status)

    def subscribe_symbols(self, symbols: list[str]) -> None:
        del symbols

    def stop(self) -> None:
        self._status = MarketFeedStatus("benchmark", FeedState.STOPPED, "stopped", MarketFeedMetrics())


class UnusedClient:
    """实时 Hub 已有数据时页面不应误走低频 Transport；误调用会让验收立即失败。"""

    def request(self, **kwargs):
        raise AssertionError(f"market Qt benchmark unexpectedly used backend request: {kwargs}")


def wait_until(predicate: Callable[[], bool], timeout_seconds: float, *, pump: Callable[[], None] | None = None) -> bool:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if pump is not None:
            pump()  # Qt 没有进入 app.exec()，测试主线程必须主动交付 queued signal 和 QTimer。
        if predicate():
            return True
        time.sleep(0.001)
    if pump is not None:
        pump()
    return predicate()


def queue_ids() -> set[int]:
    completed = subprocess.run(["ipcs", "-q"], check=True, capture_output=True, text=True)
    ids: set[int] = set()
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].startswith("0x") and fields[1].isdigit():
            ids.add(int(fields[1]))  # 只比较内核 msqid；其他正在运行的 GUI 队列作为基线保留，不会被本工具删除。
    return ids


def descendants(pid: int) -> set[int]:
    found: set[int] = set()
    frontier = [pid]
    while frontier:
        parent = frontier.pop()
        result = subprocess.run(
            ["ps", "-o", "pid=", "--ppid", str(parent)], capture_output=True, text=True, check=False,
        )
        children = [int(value) for value in result.stdout.split() if value.isdigit()]
        for child in children:
            if child not in found:
                found.add(child)
                frontier.append(child)
    return found


def measure_real_market(
    host: str,
    port: int,
    sample_count: int,
    warmup_count: int,
    timeout_seconds: float,
) -> tuple[dict[str, Any], MarketEvent]:
    feed = BenchmarkFeed(PyTdxFeedConfig(host=host, port=port, connect_timeout_seconds=1.0, reconnect_seconds=0.2))
    hub = MarketHub(feed, ui_refresh_ms=200)
    samples_ready = Event()
    bridge_to_feed_ms: list[float] = []
    decode_normalize_ms: list[float] = []
    hub_publish_ms: list[float] = []
    received_events: list[MarketEvent] = []
    sample_lock = Lock()
    valid_events_seen = 0

    def receive(event: MarketEvent) -> None:
        nonlocal valid_events_seen
        before_publish_ns = time.perf_counter_ns()  # 父类此时已经完成 UTF-8、JSON、字段校验和 MarketEvent 标准化。
        accepted = hub.publish(event)
        after_publish_ns = time.perf_counter_ns()  # 终点覆盖 Hub 去重闸门、快照/Tick/K线更新以及监听回调。
        if event.source != "pytdx":
            return  # 基准禁止把 Fallback/Mock 行情混入真实 Bridge 样本。
        if not accepted:
            return  # Bridge 在休市时可能重复快照；重复包属于正确丢弃行为，不作为正常入库延迟样本。
        source_delay = (event.received_at - event.source_time).total_seconds() * 1_000
        with sample_lock:
            valid_events_seen += 1
            if valid_events_seen <= warmup_count:
                return  # 丢弃连接建立后的首批冷启动值，让 JSON、dataclass 和 Hub 路径进入稳态后再统计。
            if len(received_events) >= sample_count:
                return
            received_events.append(event)
            bridge_to_feed_ms.append(source_delay)  # 同机墙钟差只表示 Bridge 生成报文到 Feed 标准化时刻，不是交易所延迟。
            decode_normalize_ms.append((before_publish_ns - feed.line_started_ns) / 1_000_000)
            hub_publish_ms.append((after_publish_ns - before_publish_ns) / 1_000_000)
            if len(received_events) >= sample_count:
                samples_ready.set()

    # Hub.start() 会固定回调为 hub.publish；此处直接启动同一生产 Feed，把分段计时回调放在 Hub 之前。
    feed.start(receive, hub._on_feed_state)
    feed.subscribe_symbols(["000001.SZ", "600000.SH", "600519.SH"])
    try:
        if not samples_ready.wait(timeout_seconds):
            raise RuntimeError(
                f"PyTdx Bridge 在 {timeout_seconds:.1f}s 内完成预热后仅交付 {len(received_events)}/{sample_count} 条可接受 pytdx 行情",
            )
        snapshot_ms: list[float] = []
        service = MarketService(UnusedClient(), market_hub=hub)
        for _ in range(20):
            service.list_quotes(page=1, page_size=20).result()  # 预热 UUID、Future 和页面行转换，随后样本代表稳定快照读取。
        for _ in range(max(100, sample_count * 5)):
            started_ns = time.perf_counter_ns()
            response = service.list_quotes(page=1, page_size=20).result()
            snapshot_ms.append((time.perf_counter_ns() - started_ns) / 1_000_000)
            if not response.is_success or not response.payload.get("items"):
                raise AssertionError("MarketService did not return the live Hub snapshot")

        latest = received_events[-1]
        anomaly_hub = MarketHub(ManualFeed())
        first_accepted = anomaly_hub.publish(latest)
        duplicate_rejected = not anomaly_hub.publish(latest)  # 原样重放模拟网络重复发送，最新快照和 sequence 都不应变化。
        older = replace(
            latest,
            source_time=latest.source_time - timedelta(seconds=1),
            last_price=latest.last_price + 0.001,
        )
        out_of_order_rejected = not anomaly_hub.publish(older)  # 时间更旧但价格不同，专门证明这是乱序闸门而非指纹去重。
        bad_feed = PyTdxJsonLineFeed(PyTdxFeedConfig(max_line_bytes=4096))
        bad_before = bad_feed.status.metrics.bad_payloads
        bad_feed.consume_bytes(b"not-json\n")  # 单独 Feed 制造坏包，避免与真实 Socket 线程并发改动半包缓冲。
        bad_packet_counted = bad_feed.status.metrics.bad_payloads == bad_before + 1
        if not all((first_accepted, duplicate_rejected, out_of_order_rejected, bad_packet_counted)):
            raise AssertionError("market anomaly gates did not produce the expected accept/reject results")

        return ({
            "bridge_to_feed": asdict(distribution(bridge_to_feed_ms)),
            "json_decode_and_normalize": asdict(distribution(decode_normalize_ms)),
            "hub_publish": asdict(distribution(hub_publish_ms)),
            "service_snapshot": asdict(distribution(snapshot_ms)),
            "hub_metrics": hub.metrics,
            "feed_metrics": asdict(feed.status.metrics),
            "discarded_warmup_events": warmup_count,
            "anomaly_checks": {
                "duplicate_rejected": duplicate_rejected,
                "out_of_order_rejected": out_of_order_rejected,
                "bad_packet_counted": bad_packet_counted,
            },
        }, latest)
    finally:
        feed.stop()  # 无论样本不足还是断言失败，都关闭 Socket 并 join 行情线程。


def low_frequency_request(client: ClientProtocolClient) -> Future:
    return client.request(
        session_id="mock-session",
        msg_type="users",
        func_id="page",
        payload={"page": 1, "page_size": 10},
        timeout_ms=3_000,
    )


def measure_low_frequency(client: ClientProtocolClient, serial_samples: int, concurrent_samples: int, concurrency: int) -> dict[str, Any]:
    for _ in range(20):
        response = low_frequency_request(client).result(timeout=3)
        if not response.is_success or response.payload.get("source") != "mock-lbm":
            raise AssertionError("warm-up query did not traverse the C++ Mock LBM")

    serial_ms: list[float] = []
    for _ in range(serial_samples):
        started_ns = time.perf_counter_ns()
        response = low_frequency_request(client).result(timeout=3)
        serial_ms.append((time.perf_counter_ns() - started_ns) / 1_000_000)
        if not response.is_success:
            raise AssertionError(f"serial low-frequency query failed: {response.error_code.value}")

    concurrent_ms: list[float] = []
    completed_lock = Lock()
    submitted = 0
    wall_started_ns = time.perf_counter_ns()
    while submitted < concurrent_samples:
        group_size = min(concurrency, concurrent_samples - submitted)
        group: list[Future] = []
        for _ in range(group_size):
            started_ns = time.perf_counter_ns()
            future = low_frequency_request(client)

            def record(done: Future, start_ns: int = started_ns) -> None:
                response = done.result()
                if not response.is_success:
                    raise AssertionError(f"concurrent low-frequency query failed: {response.error_code.value}")
                with completed_lock:
                    concurrent_ms.append((time.perf_counter_ns() - start_ns) / 1_000_000)

            future.add_done_callback(record)  # 回调在真实后台读线程完成，避免按 Future 列表顺序等待造成虚假的完成时间。
            group.append(future)
        for future in group:
            future.result(timeout=3)
        submitted += group_size
    concurrent_wall_seconds = (time.perf_counter_ns() - wall_started_ns) / 1_000_000_000
    if len(concurrent_ms) != concurrent_samples:
        raise AssertionError(f"expected {concurrent_samples} concurrent completions, got {len(concurrent_ms)}")
    return {
        "serial_round_trip": asdict(distribution(serial_ms)),
        "concurrent_round_trip": asdict(distribution(concurrent_ms)),
        "concurrency": concurrency,
        "throughput_requests_per_second": concurrent_samples / concurrent_wall_seconds,
        "tracker_pending_after_requests": client.tracker.pending_count,
    }


def measure_qt(
    latest_event: MarketEvent,
    client: ClientProtocolClient,
    *,
    market_events: int,
    market_rate_hz: float,
    low_samples: int,
    ui_refresh_ms: int,
) -> dict[str, Any]:
    try:
        from PySide6.QtWidgets import QApplication
        from vnpy_oxapp.ui.market_page import MarketPage
        from vnpy_oxapp.ui.qt_bridge import AsyncQtBridge
        from vnpy_oxapp.ui.table_page import ServiceTablePage, TableSpec
    except ImportError as error:
        raise RuntimeError("Qt benchmark requires PySide6 from client/requirements.txt") from error

    app = QApplication.instance() or QApplication([])
    logger = ClientLogger(stdout=StringIO())

    # 高频页面使用真实 MarketPage、Qt Signal、200ms QTimer、Service 和 QTableWidget，只把外部网络改成可控事件生产器。
    market_hub = MarketHub(ManualFeed(), ui_refresh_ms=ui_refresh_ms)
    seed = replace(latest_event, sequence=0)
    if not market_hub.publish(seed):
        raise AssertionError("cannot seed the market Hub before constructing MarketPage")
    market_bridge = AsyncQtBridge()
    market_page = MarketPage(UnusedClient(), market_bridge, logger, market_hub=market_hub)
    market_panel = market_page._tables._panels[0][1]
    wait_until(lambda: not market_page._tables._pending, 2.0, pump=app.processEvents)

    market_delays_from_first_ms: list[float] = []
    market_delays_from_latest_ms: list[float] = []
    timing_lock = Lock()
    batch_first_ns: int | None = None
    batch_latest_ns: int | None = None
    original_market_set_rows = market_panel.set_rows

    def measured_market_set_rows(items, total, page, page_size) -> None:
        nonlocal batch_first_ns, batch_latest_ns
        original_market_set_rows(items, total, page, page_size)  # 计时终点放在真实单元格写入、列宽计算和分页状态更新之后。
        finished_ns = time.perf_counter_ns()
        with timing_lock:
            if batch_first_ns is not None and batch_latest_ns is not None:
                market_delays_from_first_ms.append((finished_ns - batch_first_ns) / 1_000_000)
                market_delays_from_latest_ms.append((finished_ns - batch_latest_ns) / 1_000_000)
                batch_first_ns = None  # 一次表格完成消费掉当前合并批次，下一条 Tick 会建立新的批次起点。
                batch_latest_ns = None

    market_panel.set_rows = measured_market_set_rows
    producer_done = Event()
    interval_seconds = 1.0 / market_rate_hz

    def produce_market() -> None:
        nonlocal batch_first_ns, batch_latest_ns
        base_source_time = seed.source_time + timedelta(seconds=1)
        for index in range(market_events):
            event = replace(
                seed,
                sequence=0,
                source_time=base_source_time + timedelta(milliseconds=index + 1),
                received_at=datetime.now(),
                last_price=seed.last_price + (index + 1) / 10_000,
                volume=seed.volume + index + 1,
                turnover=seed.turnover + index + 1,
            )
            published_ns = time.perf_counter_ns()
            with timing_lock:
                if batch_first_ns is None:
                    batch_first_ns = published_ns
                batch_latest_ns = published_ns
            if not market_hub.publish(event):
                raise AssertionError(f"controlled market event {index} was unexpectedly rejected")
            time.sleep(interval_seconds)
        producer_done.set()

    producer = Thread(target=produce_market, name="benchmark-market-producer", daemon=True)
    producer.start()
    expected_duration = market_events / market_rate_hz
    if not wait_until(producer_done.is_set, expected_duration + 3.0, pump=app.processEvents):
        raise RuntimeError("controlled market producer did not finish")
    producer.join(timeout=1.0)
    wait_until(lambda: not market_page._refresh_timer.isActive(), ui_refresh_ms / 1_000 + 1.0, pump=app.processEvents)
    app.processEvents()
    market_metrics = market_hub.metrics
    market_page.close()
    app.processEvents()
    if not market_delays_from_first_ms or not market_delays_from_latest_ms:
        raise AssertionError("MarketPage did not complete any coalesced table refresh")

    # 低频页面使用真实 ClientProtocolClient -> C++ 后端 -> Qt queued signal -> ServiceTablePage -> set_rows。
    low_bridge = AsyncQtBridge()
    user_service = UserService(client)
    low_page = ServiceTablePage(
        client,
        low_bridge,
        logger,
        (TableSpec("用户管理", (("user_id", "用户编号"), ("user_name", "用户名")), lambda filters: user_service.list_users(**filters)),),
    )
    low_panel = low_page._panels[0][1]
    if not wait_until(lambda: bool(low_panel._rows) and not low_page._pending, 3.0, pump=app.processEvents):
        raise RuntimeError("low-frequency Qt page warm-up did not finish")
    low_qt_ms: list[float] = []
    low_started_ns = 0
    original_low_set_rows = low_panel.set_rows

    def measured_low_set_rows(items, total, page, page_size) -> None:
        original_low_set_rows(items, total, page, page_size)  # 包含真实 QTableWidgetItem 创建和列宽调整，结束点与用户看到表格一致。
        if low_started_ns:
            low_qt_ms.append((time.perf_counter_ns() - low_started_ns) / 1_000_000)

    low_panel.set_rows = measured_low_set_rows
    for expected in range(1, low_samples + 1):
        low_started_ns = time.perf_counter_ns()
        low_page._load(0)
        if not wait_until(lambda: len(low_qt_ms) >= expected, 3.0, pump=app.processEvents):
            raise RuntimeError(f"low-frequency Qt sample {expected}/{low_samples} timed out")
    low_page.close()
    app.processEvents()

    return {
        "market_publish_to_table_from_first_event": asdict(distribution(market_delays_from_first_ms)),
        "market_publish_to_table_from_latest_event": asdict(distribution(market_delays_from_latest_ms)),
        "market_input_events": market_events,
        "market_input_rate_hz": market_rate_hz,
        "market_table_refreshes": len(market_delays_from_first_ms),
        "market_hub_metrics": market_metrics,
        "low_request_to_table": asdict(distribution(low_qt_ms)),
    }


def format_distribution(name: str, result: dict[str, Any]) -> str:
    return (
        f"{name:<42} n={result['samples']:>4}  min={result['minimum_ms']:8.3f}  "
        f"mean={result['mean_ms']:8.3f}  P50={result['p50_ms']:8.3f}  "
        f"P95={result['p95_ms']:8.3f}  P99={result['p99_ms']:8.3f}  max={result['maximum_ms']:8.3f} ms"
    )


def print_report(result: dict[str, Any]) -> None:
    print("\n=== 高频行情 ===")
    market = result["market"]
    print(format_distribution("Bridge 时间 -> Feed received_at", market["bridge_to_feed"]))
    print(format_distribution("完整 JSON 行 -> 标准 MarketEvent", market["json_decode_and_normalize"]))
    print(format_distribution("MarketEvent -> Hub publish 完成", market["hub_publish"]))
    print(format_distribution("MarketService -> Hub 快照 Future", market["service_snapshot"]))
    print(f"行情异常闸门: {market['anomaly_checks']}")

    print("\n=== 低频查询 ===")
    low = result["low_frequency"]
    print(format_distribution("Python request -> C++/IPC/LBM -> Future", low["serial_round_trip"]))
    print(format_distribution(f"同链路 {low['concurrency']} 并发请求", low["concurrent_round_trip"]))
    print(f"并发吞吐: {low['throughput_requests_per_second']:.1f} requests/s")

    print("\n=== Qt 页面完成点 ===")
    qt = result["qt"]
    print(format_distribution("行情批次首 Tick -> 表格 set_rows 完成", qt["market_publish_to_table_from_first_event"]))
    print(format_distribution("行情批次最新 Tick -> 表格 set_rows 完成", qt["market_publish_to_table_from_latest_event"]))
    print(format_distribution("低频请求 -> 表格 set_rows 完成", qt["low_request_to_table"]))
    print(
        f"行情输入 {qt['market_input_events']} 条 @ {qt['market_input_rate_hz']:.1f}Hz，"
        f"Qt 实际重绘 {qt['market_table_refreshes']} 次（QTimer 合并刷新）。",
    )

    print("\n=== 资源清理 ===")
    resources = result["resources"]
    print(f"测试后新增残留队列: {resources['remaining_new_queue_ids']}")
    print(f"测试后仍存活的本次后端 PID: {resources['remaining_backend_pids']}")
    print(f"行情 Feed 已停止: {resources['market_feed_stopped']}")
    print("说明: Bridge 时间由本机 Bridge 在 PyTdx 拉取后生成，不是交易所时间；Qt 行情耗时包含 200ms 合并策略。")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-host", default="127.0.0.1")
    parser.add_argument("--bridge-port", type=int, default=19001)
    parser.add_argument("--bridge-samples", type=int, default=24)
    parser.add_argument("--bridge-warmup", type=int, default=8)
    parser.add_argument("--bridge-timeout", type=float, default=15.0)
    parser.add_argument("--serial-samples", type=int, default=100)
    parser.add_argument("--concurrent-samples", type=int, default=400)
    parser.add_argument("--concurrency", type=int, default=8)
    parser.add_argument("--market-events", type=int, default=200)
    parser.add_argument("--market-rate-hz", type=float, default=200.0)
    parser.add_argument("--qt-low-samples", type=int, default=30)
    parser.add_argument("--ui-refresh-ms", type=int, default=200)
    parser.add_argument("--backend", type=Path, default=PROJECT_ROOT / "build" / "bin" / "oxMbsMockBackend")
    parser.add_argument("--json-output", type=Path)
    args = parser.parse_args()
    positive = {
        "bridge_samples": args.bridge_samples,
        "bridge_timeout": args.bridge_timeout,
        "serial_samples": args.serial_samples,
        "concurrent_samples": args.concurrent_samples,
        "concurrency": args.concurrency,
        "market_events": args.market_events,
        "market_rate_hz": args.market_rate_hz,
        "qt_low_samples": args.qt_low_samples,
        "ui_refresh_ms": args.ui_refresh_ms,
    }
    invalid = [name for name, value in positive.items() if value <= 0]
    if invalid:
        parser.error(f"these values must be positive: {', '.join(invalid)}")
    return args


def main() -> int:
    args = parse_args()
    backend = args.backend.resolve()
    if not backend.is_file() or not os.access(backend, os.X_OK):
        raise RuntimeError(f"C++ Mock backend is not executable: {backend}")

    queues_before = queue_ids()
    transport = MockBackendTransport(MockBackendTransportConfig(backend_path=backend, shutdown_timeout_seconds=2.0))
    client = ClientProtocolClient(transport)
    tracked_backend_pids: set[int] = set()
    market_feed_stopped = False
    try:
        print("[1/4] 连接真实 PyTdx Bridge，采集行情分段样本...")
        market_result, latest_event = measure_real_market(
            args.bridge_host, args.bridge_port, args.bridge_samples, args.bridge_warmup, args.bridge_timeout,
        )
        market_feed_stopped = True  # measure_real_market 的 finally 已完成 feed.stop()，成功返回即证明清理路径执行。

        print("[2/4] 启动独立 C++ Mock 后端，执行预热、串行和并发查询...")
        if not client.connect():
            raise RuntimeError("cannot start the independent C++ Mock backend")
        if transport._process is not None:
            tracked_backend_pids.add(transport._process.pid)
            time.sleep(0.05)
            tracked_backend_pids.update(descendants(transport._process.pid))  # 同时跟踪 Supervisor/Worker 子进程，关闭后逐个确认不存在。
        low_result = measure_low_frequency(client, args.serial_samples, args.concurrent_samples, args.concurrency)

        print("[3/4] 运行真实 Qt Signal/QTimer/QTableWidget 页面完成点测试...")
        qt_result = measure_qt(
            latest_event,
            client,
            market_events=args.market_events,
            market_rate_hz=args.market_rate_hz,
            low_samples=args.qt_low_samples,
            ui_refresh_ms=args.ui_refresh_ms,
        )
    finally:
        client.disconnect()  # 关闭 stdin 触发 C++ 后端正常回收 Worker 和它拥有的双消息队列。

    print("[4/4] 比较测试前后进程和 System V 消息队列...")
    time.sleep(0.1)
    queues_after = queue_ids()
    remaining_pids = sorted(pid for pid in tracked_backend_pids if Path(f"/proc/{pid}").exists())
    remaining_queues = sorted(queues_after - queues_before)
    resources = {
        "baseline_queue_ids": sorted(queues_before),
        "remaining_new_queue_ids": remaining_queues,
        "tracked_backend_pids": sorted(tracked_backend_pids),
        "remaining_backend_pids": remaining_pids,
        "market_feed_stopped": market_feed_stopped,
    }
    if remaining_queues or remaining_pids or not market_feed_stopped:
        raise RuntimeError(f"benchmark left resources behind: {resources}")

    result = {
        "metadata": {
            "generated_at": datetime.now().isoformat(timespec="seconds"),
            "branch": subprocess.run(
                ["git", "branch", "--show-current"], cwd=PROJECT_ROOT, capture_output=True, text=True, check=True,
            ).stdout.strip(),
            "bridge": f"{args.bridge_host}:{args.bridge_port}",
            "backend": str(backend),
            "warning": "Bridge source_time is generated after PyTdx fetch; it is not an exchange timestamp.",
        },
        "market": market_result,
        "low_frequency": low_result,
        "qt": qt_result,
        "resources": resources,
    }
    print_report(result)
    if args.json_output is not None:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        args.json_output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"\nJSON 结果: {args.json_output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
