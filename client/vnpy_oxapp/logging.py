"""oxApp 客户端运行日志和控件加载耗时统计。

日志格式与 Mentor 要求保持一致：
YYYYMMDD HHMMSSmmm|[5d-pid][6d-tid][module][level]: message
"""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime
from enum import Enum
from pathlib import Path
from threading import RLock, get_ident
from time import perf_counter
from typing import Iterator, MutableSequence
import os
import sys


class LogLevel(str, Enum):
    DEBUG = "DEBUG"
    INFO = "INFO"
    WARN = "WARN"
    ERROR = "ERROR"


@dataclass(frozen=True, slots=True)
class LoadTiming:
    """一次组件或页面加载耗时，供日志和性能面板共同使用。"""

    component: str
    elapsed_ms: float


class ClientLogger:
    """线程安全的单次启动文件日志器。"""

    _level_order = {LogLevel.DEBUG: 10, LogLevel.INFO: 20, LogLevel.WARN: 30, LogLevel.ERROR: 40}

    def __init__(self, module: str = "oxApp", minimum_level: LogLevel = LogLevel.INFO, stdout=None) -> None:
        self.module = module
        self.minimum_level = minimum_level
        self._stdout = stdout if stdout is not None else sys.stdout
        self._lock = RLock()  # Qt 主线程和 Mock/网络线程可能同时写日志，整行必须原子落盘。
        self._file = None
        self.path: Path | None = None

    def open(self, directory: str | Path, prefix: str = "oxTrader") -> Path:
        """创建本次启动独立日志文件并返回路径。"""

        log_directory = Path(directory)
        log_directory.mkdir(parents=True, exist_ok=True)  # 日志目录由客户端负责创建，启动不依赖手工建目录。
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        path = log_directory / f"{prefix}_{timestamp}.log"
        with self._lock:
            if self._file is not None:
                self._file.close()  # 重复 open 先关闭旧文件，避免句柄和日志流泄漏。
            self._file = path.open("a", encoding="utf-8", newline="")
            self.path = path
        return path

    def close(self) -> None:
        with self._lock:
            if self._file is not None:
                self._file.flush()
                self._file.close()
                self._file = None

    def log(self, level: LogLevel, message: str, *, module: str | None = None) -> None:
        if self._level_order[level] < self._level_order[self.minimum_level]:
            return
        timestamp = datetime.now().strftime("%Y%m%d %H%M%S%f")[:-3]
        pid = os.getpid()
        tid = get_ident()
        line = f"{timestamp}|[{pid:05d}][{tid:06d}][{module or self.module}][{level.value}]: {message}"
        with self._lock:
            if self._file is not None:
                self._file.write(line + "\n")
                self._file.flush()  # 客户端异常退出时，关键启动/错误行也尽量保留下来。
            if self._stdout is not None:
                print(line, file=self._stdout, flush=True)  # Docker/终端可直接收集同一格式的标准输出。

    def debug(self, message: str, *, module: str | None = None) -> None:
        self.log(LogLevel.DEBUG, message, module=module)

    def info(self, message: str, *, module: str | None = None) -> None:
        self.log(LogLevel.INFO, message, module=module)

    def warn(self, message: str, *, module: str | None = None) -> None:
        self.log(LogLevel.WARN, message, module=module)

    def error(self, message: str, *, module: str | None = None) -> None:
        self.log(LogLevel.ERROR, message, module=module)

    @contextmanager
    def measure(
        self,
        component: str,
        *,
        sink: MutableSequence[LoadTiming] | None = None,
        module: str | None = None,
    ) -> Iterator[None]:
        """测量代码块耗时；异常仍向上抛出，但结束日志会标记 failed。"""

        started = perf_counter()
        self.debug(f"component_load_start component={component}", module=module)
        try:
            yield
        except Exception:
            elapsed_ms = (perf_counter() - started) * 1_000
            self.error(f"component_load_end component={component} elapsed_ms={elapsed_ms:.3f} status=failed", module=module)
            if sink is not None:
                sink.append(LoadTiming(component, elapsed_ms))
            raise
        else:
            elapsed_ms = (perf_counter() - started) * 1_000
            self.info(f"component_load_end component={component} elapsed_ms={elapsed_ms:.3f} status=ok", module=module)
            if sink is not None:
                sink.append(LoadTiming(component, elapsed_ms))


def create_startup_logger(log_directory: str | Path = "logs") -> ClientLogger:
    """构造并打开默认 oxApp 启动日志。"""

    logger = ClientLogger()
    logger.open(log_directory)
    return logger
