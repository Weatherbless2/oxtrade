"""正式 PackMessage 二进制协议的 Python 实现。

该模块只负责 OXPK 外部网络帧，不复用内部 System V IPC v2 结构；所有字段
使用显式网络字节序，供 ``HpSocketTransport`` 和跨语言协议测试使用。
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
import json
import struct
from typing import Any, Mapping
import zlib


class PackProtocolError(ValueError):
    """网络帧无法通过固定头、长度、CRC 或 JSON 校验。"""


class PackMessageKind(IntEnum):
    REQUEST = 1
    RESPONSE = 2
    PUSH = 3
    HEARTBEAT = 4


@dataclass(frozen=True, slots=True)
class PackFrame:
    """已完成协议校验的一帧；payload 保留原始 UTF-8 字节。"""

    kind: PackMessageKind
    request_id: int
    session_id: int
    message_type: int
    function_id: int
    sequence: int = 0
    timestamp_ms: int = 0
    error_code: int = 0
    payload: bytes = b""


class PackMessageCodec:
    """与 C++ ``PackMessage`` 保持完全一致的固定头编解码器。"""

    MAGIC = b"OXPK"
    VERSION = 1
    HEADER_LENGTH = 64
    MAX_FRAME_BYTES = 4 * 1024 * 1024
    _HEADER = struct.Struct("!4sHHIBBHQQHHQQIII")

    @classmethod
    def encode(cls, frame: PackFrame) -> bytes:
        payload = bytes(frame.payload)
        cls._validate_fields(frame, len(payload))
        checksum = zlib.crc32(payload) & 0xFFFFFFFF if payload else 0
        header = cls._HEADER.pack(
            cls.MAGIC,
            cls.VERSION,
            cls.HEADER_LENGTH,
            len(payload),
            int(frame.kind),
            0,
            0,
            frame.request_id,
            frame.session_id,
            frame.message_type,
            frame.function_id,
            frame.sequence,
            frame.timestamp_ms,
            frame.error_code,
            checksum,
            0,
        )
        return header + payload

    @classmethod
    def decode(cls, data: bytes) -> PackFrame:
        if len(data) < cls.HEADER_LENGTH:
            raise PackProtocolError("frame is shorter than the fixed 64-byte header")
        if len(data) != cls.HEADER_LENGTH + cls._HEADER.unpack_from(data, 0)[3]:
            raise PackProtocolError("frame size does not match body length")
        (
            magic,
            version,
            header_length,
            body_length,
            raw_kind,
            flags,
            reserved,
            request_id,
            session_id,
            message_type,
            function_id,
            sequence,
            timestamp_ms,
            error_code,
            payload_crc32,
            reserved2,
        ) = cls._HEADER.unpack_from(data, 0)
        if magic != cls.MAGIC:
            raise PackProtocolError("magic is not OXPK")
        if version != cls.VERSION:
            raise PackProtocolError("unsupported PackMessage version")
        if header_length != cls.HEADER_LENGTH:
            raise PackProtocolError("header length must be exactly 64 bytes")
        if body_length > cls.MAX_FRAME_BYTES - cls.HEADER_LENGTH:
            raise PackProtocolError("body length exceeds configured frame limit")
        if flags != 0 or reserved != 0 or reserved2 != 0:
            raise PackProtocolError("compression flags or reserved fields are non-zero")
        try:
            kind = PackMessageKind(raw_kind)
        except ValueError as error:
            raise PackProtocolError("message kind is invalid") from error
        if kind is PackMessageKind.REQUEST and error_code != 0:
            raise PackProtocolError("Request error_code must be zero")
        if kind is PackMessageKind.HEARTBEAT and function_id != 0:
            raise PackProtocolError("Heartbeat function_id must be zero")
        payload = data[cls.HEADER_LENGTH:]
        expected_crc = zlib.crc32(payload) & 0xFFFFFFFF if payload else 0
        if payload_crc32 != expected_crc:
            raise PackProtocolError("payload CRC32 does not match")
        return PackFrame(
            kind=kind,
            request_id=request_id,
            session_id=session_id,
            message_type=message_type,
            function_id=function_id,
            sequence=sequence,
            timestamp_ms=timestamp_ms,
            error_code=error_code,
            payload=payload,
        )

    @classmethod
    def decode_json(cls, payload: bytes) -> dict[str, Any]:
        if not payload:
            return {}
        try:
            document = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise PackProtocolError(f"invalid JSON payload: {error}") from error
        if not isinstance(document, dict):
            raise PackProtocolError("JSON payload must be an object")
        return document

    @classmethod
    def encode_json(cls, document: Mapping[str, Any]) -> bytes:
        try:
            # 稳定键序和紧凑分隔符便于跨语言固定向量比对，不改变中文 UTF-8 字节。
            return json.dumps(document, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
        except (TypeError, ValueError) as error:
            raise PackProtocolError(f"JSON payload is not serializable: {error}") from error

    @classmethod
    def _validate_fields(cls, frame: PackFrame, body_length: int) -> None:
        if body_length > cls.MAX_FRAME_BYTES - cls.HEADER_LENGTH:
            raise PackProtocolError("payload exceeds configured frame limit")
        try:
            PackMessageKind(frame.kind)
        except ValueError as error:
            raise PackProtocolError("message kind is invalid") from error
        if not 0 <= frame.request_id <= 0xFFFFFFFFFFFFFFFF:
            raise PackProtocolError("request_id is outside uint64 range")
        if not 0 <= frame.session_id <= 0xFFFFFFFFFFFFFFFF:
            raise PackProtocolError("session_id is outside uint64 range")
        if not 0 <= frame.message_type <= 0xFFFF or not 0 <= frame.function_id <= 0xFFFF:
            raise PackProtocolError("message/function id is outside uint16 range")
        if not 0 <= frame.sequence <= 0xFFFFFFFFFFFFFFFF or not 0 <= frame.timestamp_ms <= 0xFFFFFFFFFFFFFFFF:
            raise PackProtocolError("sequence/timestamp is outside uint64 range")
        if not 0 <= frame.error_code <= 0xFFFFFFFF:
            raise PackProtocolError("error_code is outside uint32 range")


class PackMessageStreamDecoder:
    """按 TCP 字节流还原完整 PackMessage，支持半包与粘包。"""

    def __init__(self, max_frame_bytes: int = PackMessageCodec.MAX_FRAME_BYTES) -> None:
        if not PackMessageCodec.HEADER_LENGTH <= max_frame_bytes <= PackMessageCodec.MAX_FRAME_BYTES:
            raise ValueError("max_frame_bytes must be between 64 and 4 MiB")
        self.max_frame_bytes = max_frame_bytes
        self._buffer = bytearray()

    def feed(self, data: bytes) -> list[PackFrame]:
        if not data:
            return []
        if len(self._buffer) + len(data) > self.max_frame_bytes:
            self._buffer.clear()
            raise PackProtocolError("receive buffer exceeds configured frame limit")
        self._buffer.extend(data)
        frames: list[PackFrame] = []
        while len(self._buffer) >= PackMessageCodec.HEADER_LENGTH:
            body_length = struct.unpack_from("!I", self._buffer, 8)[0]
            if body_length > self.max_frame_bytes - PackMessageCodec.HEADER_LENGTH:
                self._buffer.clear()
                raise PackProtocolError("body length exceeds configured frame limit")
            frame_length = PackMessageCodec.HEADER_LENGTH + body_length
            if len(self._buffer) < frame_length:
                break
            raw_frame = bytes(self._buffer[:frame_length])
            del self._buffer[:frame_length]
            frames.append(PackMessageCodec.decode(raw_frame))
        return frames

    @property
    def buffered_bytes(self) -> int:
        return len(self._buffer)

    def clear(self) -> None:
        self._buffer.clear()
