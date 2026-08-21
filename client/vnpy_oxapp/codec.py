"""JSON Mock Codec。

真实 PackMessage 字段和二进制布局未确认，本工单只提供可审计、可替换的临时编码。
"""

from __future__ import annotations

import json
from typing import Any, Mapping

from .message import ClientRequest, ClientResponse, ErrorCode, MessageStatus


class CodecError(ValueError):
    """入站数据无法解释为 #16 临时消息模型。"""


class JsonMessageCodec:
    """显式字段名的 JSON 编解码器，测试可准确覆盖协议边界。"""

    @staticmethod
    def encode_request(request: ClientRequest) -> bytes:
        document = {
            "kind": "request",
            "request_id": request.request_id,
            "session_id": request.session_id,
            "msg_type": request.msg_type,
            "func_id": request.func_id,
            "payload": dict(request.payload),
            "deadline_unix_ms": request.deadline_unix_ms,
        }
        return JsonMessageCodec._encode(document)

    @staticmethod
    def decode_request(data: bytes) -> ClientRequest:
        document = JsonMessageCodec._decode(data, "request")
        try:
            return ClientRequest(
                request_id=JsonMessageCodec._text(document, "request_id"),
                session_id=JsonMessageCodec._text(document, "session_id"),
                msg_type=JsonMessageCodec._text(document, "msg_type"),
                func_id=JsonMessageCodec._text(document, "func_id"),
                payload=JsonMessageCodec._payload(document),
                deadline_unix_ms=JsonMessageCodec._positive_int(document, "deadline_unix_ms"),
            )
        except (TypeError, ValueError) as error:
            raise CodecError(f"invalid request fields: {error}") from error

    @staticmethod
    def encode_response(response: ClientResponse) -> bytes:
        document = {
            "kind": "response",
            "request_id": response.request_id,
            "session_id": response.session_id,
            "msg_type": response.msg_type,
            "func_id": response.func_id,
            "status": response.status.value,
            "payload": dict(response.payload),
            "error_code": response.error_code.value,
            "error_message": response.error_message,
        }
        return JsonMessageCodec._encode(document)

    @staticmethod
    def decode_response(data: bytes) -> ClientResponse:
        document = JsonMessageCodec._decode(data, "response")
        try:
            return ClientResponse(
                request_id=JsonMessageCodec._text(document, "request_id"),
                session_id=JsonMessageCodec._text(document, "session_id"),
                msg_type=JsonMessageCodec._text(document, "msg_type"),
                func_id=JsonMessageCodec._text(document, "func_id"),
                status=MessageStatus(JsonMessageCodec._text(document, "status")),
                payload=JsonMessageCodec._payload(document),
                error_code=ErrorCode(JsonMessageCodec._text(document, "error_code")),
                error_message=JsonMessageCodec._text(document, "error_message", required=False),
            )
        except (TypeError, ValueError) as error:
            raise CodecError(f"invalid response fields: {error}") from error

    @staticmethod
    def _encode(document: Mapping[str, Any]) -> bytes:
        try:
            # sort_keys 让同一输入产生稳定字节，便于日志比对和单元测试。
            return json.dumps(document, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
        except (TypeError, ValueError) as error:
            raise CodecError(f"message is not JSON serializable: {error}") from error

    @staticmethod
    def _decode(data: bytes, expected_kind: str) -> Mapping[str, Any]:
        try:
            document = json.loads(data.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise CodecError(f"invalid JSON message: {error}") from error
        if not isinstance(document, dict):
            raise CodecError("message root must be an object")
        if document.get("kind") != expected_kind:
            raise CodecError(f"expected {expected_kind} message")
        return document

    @staticmethod
    def _text(document: Mapping[str, Any], key: str, *, required: bool = True) -> str:
        value = document.get(key, "")
        if not isinstance(value, str) or (required and not value):
            raise CodecError(f"{key} must be a non-empty string")
        return value

    @staticmethod
    def _payload(document: Mapping[str, Any]) -> Mapping[str, Any]:
        value = document.get("payload", {})
        if not isinstance(value, dict):
            raise CodecError("payload must be an object")
        return value

    @staticmethod
    def _positive_int(document: Mapping[str, Any], key: str) -> int:
        value = document.get(key)
        if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
            raise CodecError(f"{key} must be a positive integer")
        return value
