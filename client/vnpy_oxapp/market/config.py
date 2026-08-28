"""行情开发配置：本地配置可覆盖示例文件，默认始终可以用 Mock 启动。"""

from __future__ import annotations

from configparser import ConfigParser
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True, slots=True)
class MarketConfig:
    source: str = "mock"  # 未提供本机配置时只启动离线行情，避免开发机误连外部行情地址。
    pytdx_host: str = "127.0.0.1"  # Bridge 只按本机开发进程使用，不把第三方行情服务器地址交给 oxApp。
    pytdx_port: int = 19001  # QuantFabric Bridge 的 JSON-lines 监听端口，不是通达信私有协议端口。
    reconnect_seconds: float = 2.0  # TCP 失败后的退避时间，限制失败循环频率，避免空转占满 CPU。
    fallback_after_seconds: float = 5.0  # 连上但迟迟没有有效 Tick 时启用 Mock，不能只依赖 socket connected 状态。
    ui_refresh_ms: int = 200  # 页面最短刷新间隔；行情仍逐条进入 Hub，但多个 Tick 合并为一次表格重绘。
    tick_buffer_size: int = 500  # 每个标的最多保留的 Tick 数，deque 到上限会自动淘汰最旧记录。
    bar_buffer_size: int = 480  # 每个标的最多保留的分钟线数，防止桌面客户端长期运行后内存无界增长。
    mock_interval_ms: int = 800  # 离线源生成快照的间隔，只服务开发演示，不代表生产行情频率。


def load_market_config(path: str | Path | None = None) -> MarketConfig:
    """读取 ``config/oxapp.ini``；不存在本地配置时使用安全的 Mock 默认值。"""

    project_root = Path(__file__).resolve().parents[3]  # 从已安装模块位置反推仓库根目录，启动命令不必依赖当前工作目录。
    config_path = Path(path) if path is not None else project_root / "config" / "oxapp.ini"  # 测试可注入临时文件，正式启动读取不提交的本机配置。
    parser = ConfigParser()  # 使用标准 INI 解析器保留 section/key 语义，避免手工切字符串误读注释或空格。
    parser.read(config_path, encoding="utf-8")  # 文件不存在时 ConfigParser 返回空结果，随后安全回落到 Mock 默认值。
    section = parser["market"] if parser.has_section("market") else {}  # 缺少整个行情段也可离线启动，不让可选行情阻断客户端。
    source = str(section.get("source", "mock")).strip().lower()  # 大小写和首尾空格规范化后再执行白名单校验。
    if source not in {"mock", "pytdx"}:
        raise ValueError("market.source must be mock or pytdx")
    return MarketConfig(
        source=source,
        pytdx_host=str(section.get("pytdx_host", "127.0.0.1")).strip() or "127.0.0.1",
        pytdx_port=max(1, int(section.get("pytdx_port", 19001))),  # 拒绝 0/负端口落入含糊的系统行为，类型错误则直接报告配置失败。
        reconnect_seconds=max(0.05, float(section.get("reconnect_seconds", 2.0))),  # 至少等待 50ms，错误配置也不能形成紧密重连循环。
        fallback_after_seconds=max(0.2, float(section.get("fallback_after_seconds", 5.0))),  # 至少观察 200ms，避免线程刚启动就误判主源无行情。
        ui_refresh_ms=max(50, int(section.get("ui_refresh_ms", 200))),  # Qt 最快 20 次/秒刷新，保护主线程不被 Tick 风暴拖死。
        tick_buffer_size=max(10, int(section.get("tick_buffer_size", 500))),  # 保留最少十条便于页面验收，同时始终有明确容量上限。
        bar_buffer_size=max(10, int(section.get("bar_buffer_size", 480))),  # 分钟线同样使用有界缓存，调用方拿到的是快照而非无限历史。
        mock_interval_ms=max(50, int(section.get("mock_interval_ms", 800))),  # Mock 生成线程也限制最短周期，避免错误配置制造高 CPU。
    )
