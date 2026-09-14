"""oxApp 客户端运行模式配置。

默认仍保持 Mock，只有显式设置 ``OXAPP_TRANSPORT=hpsocket`` 或配置文件中的
``[transport] kind=hpsocket`` 才连接 oxMBS。这样开发演示不会误连服务端，
而正式运行的地址、端口和会话标识可以由部署环境注入，不进入源码。
"""

from __future__ import annotations

from configparser import ConfigParser
from dataclasses import dataclass
import os
from pathlib import Path

from .hpsocket_transport import HpSocketTransport, HpSocketTransportConfig
from .mock.mock_backend_transport import MockBackendTransport
from .transport import IClientTransport


@dataclass(frozen=True, slots=True)
class ClientRuntimeConfig:
    """客户端传输和会话边界；密码、Token 等认证秘密不属于本配置。"""

    transport_kind: str = "mock"
    host: str = "127.0.0.1"
    port: int = 18080
    session_id: str = "mock-session"
    connect_timeout_seconds: float = 3.0
    read_timeout_seconds: float = 0.5
    reconnect_enabled: bool = True

    def validate(self) -> None:
        if self.transport_kind not in {"mock", "hpsocket"}:
            raise ValueError("transport.kind must be mock or hpsocket")
        if not self.host or not 1 <= self.port <= 65535:
            raise ValueError("transport host and port are invalid")
        if self.connect_timeout_seconds <= 0 or self.read_timeout_seconds <= 0:
            raise ValueError("transport timeouts must be positive")
        if not self.session_id.strip():
            raise ValueError("transport.session_id must not be empty")


def _project_root() -> Path:
    return Path(__file__).resolve().parents[2]


def load_client_runtime_config(path: str | Path | None = None) -> ClientRuntimeConfig:
    """读取本机 INI，并让 ``OXAPP_*`` 环境变量覆盖部署相关字段。"""

    config_path = Path(path) if path is not None else _project_root() / "config" / "oxapp.ini"
    parser = ConfigParser()
    parser.read(config_path, encoding="utf-8")
    section = parser["transport"] if parser.has_section("transport") else {}

    def value(name: str, default: str) -> str:
        return os.environ.get(f"OXAPP_{name.upper()}", str(section.get(name, default))).strip()

    config = ClientRuntimeConfig(
        transport_kind=value("transport", "mock").lower(),
        host=value("hpsocket_host", "127.0.0.1"),
        port=int(value("hpsocket_port", "18080")),
        session_id=value("session_id", "mock-session"),
        connect_timeout_seconds=float(value("connect_timeout_seconds", "3")),
        read_timeout_seconds=float(value("read_timeout_seconds", "0.5")),
        reconnect_enabled=value("reconnect_enabled", "true").lower() not in {"0", "false", "no"},
    )
    config.validate()
    return config


def create_client_transport(config: ClientRuntimeConfig) -> IClientTransport:
    """根据显式运行模式创建 Transport；页面不直接决定 Mock 或 Socket。"""

    config.validate()
    if config.transport_kind == "hpsocket":
        return HpSocketTransport(HpSocketTransportConfig(
            host=config.host,
            port=config.port,
            connect_timeout_seconds=config.connect_timeout_seconds,
            read_timeout_seconds=config.read_timeout_seconds,
            reconnect_enabled=config.reconnect_enabled,
        ))
    return MockBackendTransport()


__all__ = ["ClientRuntimeConfig", "create_client_transport", "load_client_runtime_config"]
