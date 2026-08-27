"""行情开发配置：本地配置可覆盖示例文件，默认始终可以用 Mock 启动。"""

from __future__ import annotations

from configparser import ConfigParser
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True, slots=True)
class MarketConfig:
    source: str = "mock"
    pytdx_host: str = "127.0.0.1"
    pytdx_port: int = 19001
    reconnect_seconds: float = 2.0
    fallback_after_seconds: float = 5.0
    ui_refresh_ms: int = 200
    tick_buffer_size: int = 500
    bar_buffer_size: int = 480
    mock_interval_ms: int = 800


def load_market_config(path: str | Path | None = None) -> MarketConfig:
    """读取 ``config/oxapp.ini``；不存在本地配置时使用安全的 Mock 默认值。"""

    project_root = Path(__file__).resolve().parents[3]
    config_path = Path(path) if path is not None else project_root / "config" / "oxapp.ini"
    parser = ConfigParser()
    parser.read(config_path, encoding="utf-8")
    section = parser["market"] if parser.has_section("market") else {}
    source = str(section.get("source", "mock")).strip().lower()
    if source not in {"mock", "pytdx"}:
        raise ValueError("market.source must be mock or pytdx")
    return MarketConfig(
        source=source,
        pytdx_host=str(section.get("pytdx_host", "127.0.0.1")).strip() or "127.0.0.1",
        pytdx_port=max(1, int(section.get("pytdx_port", 19001))),
        reconnect_seconds=max(0.05, float(section.get("reconnect_seconds", 2.0))),
        fallback_after_seconds=max(0.2, float(section.get("fallback_after_seconds", 5.0))),
        ui_refresh_ms=max(50, int(section.get("ui_refresh_ms", 200))),
        tick_buffer_size=max(10, int(section.get("tick_buffer_size", 500))),
        bar_buffer_size=max(10, int(section.get("bar_buffer_size", 480))),
        mock_interval_ms=max(50, int(section.get("mock_interval_ms", 800))),
    )
