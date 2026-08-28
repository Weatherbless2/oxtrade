#!/usr/bin/env python3
"""测量当前 Mock 后端高频直通与低频 Worker 路线的完整客户端往返。"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time
from typing import Sequence


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "client"))  # 直接运行时固定使用当前工作区源码，避免旧安装包改变协议实现。

from vnpy_oxapp.connection import ClientProtocolClient  # noqa: E402
from vnpy_oxapp.mock.mock_backend_transport import MockBackendTransport, MockBackendTransportConfig  # noqa: E402


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
    ordered = sorted(values)
    if not ordered:
        raise ValueError("cannot summarize empty samples")

    def percentile(ratio: float) -> float:
        return ordered[max(1, math.ceil(ratio * len(ordered))) - 1]  # 最近秩分位数直接对应一次真实业务请求。

    return Distribution(
        samples=len(ordered),
        minimum_ms=ordered[0],
        mean_ms=statistics.fmean(ordered),
        p50_ms=percentile(0.50),
        p95_ms=percentile(0.95),
        p99_ms=percentile(0.99),
        maximum_ms=ordered[-1],
    )


def queue_ids() -> set[int]:
    result = subprocess.run(["ipcs", "-q"], check=True, capture_output=True, text=True)
    ids: set[int] = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].startswith("0x") and fields[1].isdigit():
            ids.add(int(fields[1]))  # 以运行前的所有现存队列为基线，只检查本次独立后端是否留下新队列。
    return ids


def request(client: ClientProtocolClient, *, route: str):
    if route == "high":
        return client.request(
            session_id="benchmark-session",
            msg_type="market",
            func_id="quotes",
            payload={"page": 1, "page_size": 1},
            timeout_ms=3_000,
        )  # 当前 C++ 后端把 market 直接交给 MockGateway，不创建 IPC Task。
    return client.request(
        session_id="benchmark-session",
        msg_type="service_status",
        func_id="page",
        payload={"page": 1, "page_size": 1},
        timeout_ms=3_000,
    )  # 服务状态命中低频白名单，且单行字段量更接近行情快照，减少响应大小差异但仍不是同一种业务。


def measure(client: ClientProtocolClient, samples: int, warmup: int) -> dict:
    for _ in range(warmup):
        for route in ("high", "low"):
            response = request(client, route=route).result(timeout=3)
            if not response.is_success:
                raise AssertionError(f"{route} warm-up failed: {response.error_code.value}")

    high_ms: list[float] = []
    low_ms: list[float] = []
    high_payload_bytes = 0
    low_payload_bytes = 0
    pair_started_ns = time.perf_counter_ns()
    for index in range(samples):
        # 每轮交换先后顺序，降低系统负载随时间变化对某一条路线的固定偏差。
        routes = ("high", "low") if index % 2 == 0 else ("low", "high")
        for route in routes:
            started_ns = time.perf_counter_ns()
            response = request(client, route=route).result(timeout=3)
            elapsed_ms = (time.perf_counter_ns() - started_ns) / 1_000_000
            if not response.is_success:
                raise AssertionError(f"{route} request failed: {response.error_code.value}")
            encoded_bytes = len(json.dumps(response.payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
            if route == "high":
                high_ms.append(elapsed_ms)
                high_payload_bytes = encoded_bytes
                if response.payload.get("source") != "mock-gateway":
                    raise AssertionError("high request did not use MockGateway")
            else:
                low_ms.append(elapsed_ms)
                low_payload_bytes = encoded_bytes
                if response.payload.get("source") != "mock-lbm":
                    raise AssertionError("low request did not use Mock LBM")
    wall_seconds = (time.perf_counter_ns() - pair_started_ns) / 1_000_000_000
    return {
        "high_direct_gateway": asdict(distribution(high_ms)),
        "low_systemv_worker_lbm": asdict(distribution(low_ms)),
        "high_response_payload_bytes": high_payload_bytes,
        "low_response_payload_bytes": low_payload_bytes,
        "combined_requests_per_second": (samples * 2) / wall_seconds,
        "tracker_pending_after_requests": client.tracker.pending_count,
    }


def print_distribution(name: str, value: dict) -> None:
    print(
        f"{name:<44} n={value['samples']:>5} min={value['minimum_ms']:8.3f} "
        f"mean={value['mean_ms']:8.3f} P50={value['p50_ms']:8.3f} "
        f"P95={value['p95_ms']:8.3f} P99={value['p99_ms']:8.3f} max={value['maximum_ms']:8.3f} ms",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument("--samples", type=int, default=1000)
    parser.add_argument("--backend", type=Path, default=PROJECT_ROOT / "build" / "bin" / "oxMbsMockBackend")
    parser.add_argument("--json-output", type=Path)
    args = parser.parse_args()
    if args.warmup <= 0 or args.samples <= 0:
        parser.error("warmup and samples must be positive")
    backend = args.backend.resolve()
    if not backend.is_file() or not os.access(backend, os.X_OK):
        raise RuntimeError(f"backend is not executable: {backend}")

    queues_before = queue_ids()
    transport = MockBackendTransport(MockBackendTransportConfig(backend_path=backend, shutdown_timeout_seconds=2.0))
    client = ClientProtocolClient(transport)
    backend_pid = 0
    try:
        if not client.connect():
            raise RuntimeError("cannot start independent Mock backend")
        if transport._process is not None:
            backend_pid = transport._process.pid
        result = measure(client, args.samples, args.warmup)
    finally:
        client.disconnect()  # 关闭 stdin 触发 C++ 父进程依次回收 Worker、ReplyCollector 和双消息队列。

    time.sleep(0.1)
    remaining_queues = sorted(queue_ids() - queues_before)
    backend_alive = backend_pid > 0 and Path(f"/proc/{backend_pid}").exists()
    result["remaining_new_queue_ids"] = remaining_queues
    result["backend_process_alive"] = backend_alive
    if remaining_queues or backend_alive or result["tracker_pending_after_requests"] != 0:
        raise RuntimeError(f"benchmark resource cleanup failed: {result}")

    high = result["high_direct_gateway"]
    low = result["low_systemv_worker_lbm"]
    print_distribution("high: Python -> C++ direct Gateway -> Python", high)
    print_distribution("low: Python -> SysV/Worker/LBM -> Python", low)
    print(
        f"response_payload_bytes high={result['high_response_payload_bytes']} low={result['low_response_payload_bytes']} "
        f"mean_ratio={low['mean_ms'] / high['mean_ms']:.2f} "
        f"direct_reduction={(1.0 - high['mean_ms'] / low['mean_ms']) * 100.0:.2f}% "
        f"combined_throughput={result['combined_requests_per_second']:.1f} req/s",
    )
    print(f"cleanup queues={remaining_queues} backend_alive={backend_alive} pending={result['tracker_pending_after_requests']}")
    if args.json_output is not None:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        args.json_output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"JSON result: {args.json_output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
