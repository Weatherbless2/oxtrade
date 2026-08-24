"""#24 的本地 Mock 后端传输适配。

该模块只通过 stdin/stdout 启动本地 ``oxMbsMockBackend``，用于验证已完成的 Router、
System V IPC、Worker、LBM 和 ReplyCollector 链路。它不是正式 HP-Socket 协议实现。
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
from subprocess import PIPE, Popen, TimeoutExpired
from threading import Lock, Thread, current_thread
from typing import Any, Mapping
from urllib.parse import quote, unquote, urlencode

from ..message import ClientRequest, ClientResponse, ErrorCode, MessageStatus
from ..transport import ConnectionState, ErrorCallback, IClientTransport, ResponseCallback, StateCallback


@dataclass(frozen=True, slots=True)
class MockBackendTransportConfig:
    """本地子进程配置；可执行文件路径可注入，测试不需要猜测 IDE 构建目录。"""

    backend_path: str | Path | None = None
    shutdown_timeout_seconds: float = 1.0


class MockBackendTransport(IClientTransport):
    """把客户端适配请求交给 Mock-only C++ 后端，并在后台线程读取回复。"""

    def __init__(self, config: MockBackendTransportConfig | None = None) -> None:
        self.config = config or MockBackendTransportConfig()
        self._state = ConnectionState.DISCONNECTED
        self._lock = Lock()  # 进程句柄、状态和 stdin 写入必须串行，避免两条请求字节交叉。
        self._process: Popen[bytes] | None = None
        self._reader: Thread | None = None
        self._closing = False
        self._response_callback: ResponseCallback | None = None
        self._state_callback: StateCallback | None = None
        self._error_callback: ErrorCallback | None = None

    @property
    def state(self) -> ConnectionState:
        with self._lock:
            return self._state

    def set_response_callback(self, callback: ResponseCallback) -> None:
        self._response_callback = callback

    def set_state_callback(self, callback: StateCallback) -> None:
        self._state_callback = callback

    def set_error_callback(self, callback: ErrorCallback) -> None:
        self._error_callback = callback

    def connect(self) -> bool:
        with self._lock:
            if self._state is ConnectionState.CONNECTED and self._process is not None and self._process.poll() is None:
                return True
        self._set_state(ConnectionState.CONNECTING, "starting local Mock backend")
        executable = self._resolve_backend_path()
        if not executable.is_file():
            message = f"Mock backend executable was not found: {executable}"
            self._set_state(ConnectionState.FAILED, message)
            self._emit_error(ErrorCode.CONNECTION_FAILED, message)
            return False
        try:
            process = Popen(  # noqa: S603 - 路径来自显式配置/本地构建产物，不经过 shell 字符串拼接。
                [str(executable)], stdin=PIPE, stdout=PIPE, stderr=PIPE, bufsize=0,
            )
        except OSError as error:
            message = f"cannot start Mock backend: {error}"
            self._set_state(ConnectionState.FAILED, message)
            self._emit_error(ErrorCode.CONNECTION_FAILED, message)
            return False
        with self._lock:
            self._process = process
            self._closing = False
        reader = Thread(target=self._read_responses, args=(process,), name="oxapp-mock-backend-reader", daemon=True)
        self._reader = reader
        reader.start()  # 子线程只解析协议和回调 ClientProtocolClient，Qt 线程切换仍由 AsyncQtBridge 负责。
        self._set_state(ConnectionState.CONNECTED, "local Mock backend started")
        return True

    def disconnect(self) -> None:
        with self._lock:
            process, self._process = self._process, None
            reader = self._reader
            self._closing = True
        if process is not None:
            if process.stdin is not None:
                try:
                    process.stdin.close()  # EOF 要求 C++ 后端走既有 Supervisor/Queue 清理路径，而不是让 Python 直接杀 Worker。
                except OSError:
                    pass
            try:
                process.wait(timeout=self.config.shutdown_timeout_seconds)
            except TimeoutExpired:
                process.terminate()  # 正常 EOF 清理失败时才终止父进程；不单独操作任何 System V Queue。
                try:
                    process.wait(timeout=self.config.shutdown_timeout_seconds)
                except TimeoutExpired:
                    process.kill()
                    process.wait(timeout=self.config.shutdown_timeout_seconds)
        if reader is not None and reader is not current_thread():
            reader.join(timeout=self.config.shutdown_timeout_seconds)
        if process is not None:
            for stream in (process.stdout, process.stderr):
                if stream is not None:
                    try:
                        stream.close()  # 正常断开也显式关闭 stdout/stderr 管道，避免长期运行和测试留下文件描述符。
                    except OSError:
                        pass
        with self._lock:
            self._reader = None
            self._closing = False
        self._set_state(ConnectionState.DISCONNECTED, "local Mock backend stopped")

    def send(self, request: ClientRequest) -> bool:
        line = self._encode_request(request)
        with self._lock:
            process = self._process
            if self._state is not ConnectionState.CONNECTED or process is None or process.poll() is not None or process.stdin is None:
                self._emit_error(ErrorCode.DISCONNECTED, "Mock backend is not connected")
                return False
            try:
                process.stdin.write(line.encode("ascii") + b"\n")  # 外层百分号编码后只剩 ASCII，避免本地管道的编码歧义。
                process.stdin.flush()
                return True
            except OSError as error:
                self._emit_error(ErrorCode.DISCONNECTED, f"Mock backend write failed: {error}")
                return False

    def _resolve_backend_path(self) -> Path:
        configured = self.config.backend_path or os.environ.get("OXAPP_MOCK_BACKEND_PATH")
        if configured:
            return Path(configured).expanduser().resolve()
        project_root = Path(__file__).resolve().parents[3]
        return project_root / "build" / "bin" / "oxMbsMockBackend"  # WSL 默认 CMake 产物路径；真实服务替换时不复用这个默认值。

    @staticmethod
    def _encode_request(request: ClientRequest) -> str:
        payload = urlencode(_flatten_payload(request.payload), doseq=False)
        fields = (
            "Q", quote(request.request_id, safe=""), quote(request.session_id, safe=""), quote(request.msg_type, safe=""),
            quote(request.func_id, safe=""), quote(payload, safe=""), str(request.deadline_unix_ms),
        )
        return "\t".join(fields)  # Q 行是 #24 临时本地协议，禁止拿去替代未确认的正式 HP-Socket 报文。

    def _read_responses(self, process: Popen[bytes]) -> None:
        try:
            if process.stdout is None:
                return
            for raw_line in process.stdout:
                response = self._decode_response(raw_line)
                if response is None:
                    continue
                callback = self._response_callback
                if callback is not None:
                    callback(response)
        finally:
            unexpected = False
            with self._lock:
                if self._process is process:
                    self._process = None
                    unexpected = not self._closing
            if unexpected:
                detail = _read_stderr(process)
                message = "Mock backend exited unexpectedly" + (f": {detail}" if detail else "")
                self._set_state(ConnectionState.DISCONNECTED, message)
                self._emit_error(ErrorCode.DISCONNECTED, message)

    def _decode_response(self, raw_line: bytes) -> ClientResponse | None:
        try:
            fields = raw_line.decode("ascii").rstrip("\r\n").split("\t")
            if len(fields) != 9 or fields[0] != "R":
                raise ValueError("expected Mock-only R line with nine tab-separated fields")
            request_id, session_id, msg_type, func_id, status_text, code_text, message, payload_text = (
                unquote(field) for field in fields[1:]
            )
            status = MessageStatus(status_text)
            error_code = ErrorCode(code_text)
            payload = json.loads(payload_text) if payload_text else {}
            if not isinstance(payload, dict):
                raise ValueError("Mock backend payload must be a JSON object")
            return ClientResponse(
                request_id=request_id, session_id=session_id, msg_type=msg_type, func_id=func_id,
                status=status, error_code=error_code, error_message=message, payload=payload,
            )
        except (UnicodeDecodeError, ValueError, json.JSONDecodeError) as error:
            self._emit_error(ErrorCode.PROTOCOL_ERROR, f"invalid Mock backend response: {error}")
            return None  # 无法验证关联字段的坏行不应送入 RequestTracker，避免污染任何页面的 Future。

    def _set_state(self, state: ConnectionState, reason: str) -> None:
        with self._lock:
            self._state = state
        callback = self._state_callback
        if callback is not None:
            callback(state, reason)

    def _emit_error(self, code: ErrorCode, message: str) -> None:
        callback = self._error_callback
        if callback is not None:
            callback(code, message)


def _flatten_payload(payload: Mapping[str, Any], prefix: str = "") -> dict[str, str]:
    """把页面对象编码为 Mock LBM 可解析的扁平表单，真实 HP-Socket 替换时页面层不依赖本规则。"""

    flat: dict[str, str] = {}
    for key, value in payload.items():
        name = f"{prefix}.{key}" if prefix else str(key)
        if isinstance(value, Mapping):
            flat.update(_flatten_payload(value, name))
        elif isinstance(value, bool):
            flat[name] = "true" if value else "false"
        elif isinstance(value, (list, tuple, set)):
            flat[name] = ",".join(str(item) for item in value)
        elif value is None:
            flat[name] = ""
        else:
            flat[name] = str(value)
    return flat


def _read_stderr(process: Popen[bytes]) -> str:
    if process.stderr is None:
        return ""
    try:
        return process.stderr.read().decode("utf-8", errors="replace").strip()
    except OSError:
        return ""
