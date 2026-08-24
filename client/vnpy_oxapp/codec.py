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
        document = {  # 显式字段布局让 Mock、日志和未来正式 Codec 都能检查同一份契约。
            "kind": "request",  # 顶层类型先做路由闸门，避免把应答误当成请求。
            "request_id": request.request_id,  # 关联键必须原样传到对端并回传。
            "session_id": request.session_id,  # 会话键用于跨会话隔离。
            "msg_type": request.msg_type,  # 业务服务分类。
            "func_id": request.func_id,  # 服务内操作分类。
            "payload": dict(request.payload),  # Mapping 转普通 dict，保证 JSON 序列化不依赖调用方容器。
            "deadline_unix_ms": request.deadline_unix_ms,  # 传递绝对截止时间，让对端可理解请求有效期。
        }
        return JsonMessageCodec._encode(document)  # 统一在这里转 UTF-8 bytes，传输层只处理字节。

    @staticmethod
    def decode_request(data: bytes) -> ClientRequest:
        document = JsonMessageCodec._decode(data, "request")  # 先校验 JSON 根对象和 kind，再读取业务字段。
        try:
            return ClientRequest(  # 构造模型再次触发字段校验，坏请求在进入服务前失败。
                request_id=JsonMessageCodec._text(document, "request_id"),  # 必须是非空文本关联键。
                session_id=JsonMessageCodec._text(document, "session_id"),  # 必须是非空会话标识。
                msg_type=JsonMessageCodec._text(document, "msg_type"),  # 必须指定业务模块。
                func_id=JsonMessageCodec._text(document, "func_id"),  # 必须指定业务操作。
                payload=JsonMessageCodec._payload(document),  # 只接受对象，拒绝数组/字符串污染业务参数。
                deadline_unix_ms=JsonMessageCodec._positive_int(document, "deadline_unix_ms"),  # 拒绝 0、负数和 bool 截止时间。
            )
        except (TypeError, ValueError) as error:
            raise CodecError(f"invalid request fields: {error}") from error  # 统一包装模型错误，调用方只处理 CodecError。

    @staticmethod
    def encode_response(response: ClientResponse) -> bytes:
        document = {  # 应答布局与请求分开，保证 status/error 字段不会被误读成请求参数。
            "kind": "response",
            "request_id": response.request_id,
            "session_id": response.session_id,
            "msg_type": response.msg_type,
            "func_id": response.func_id,
            "status": response.status.value,
            "payload": dict(response.payload),  # 复制 Mapping，避免编码期间调用方继续修改原对象。
            "error_code": response.error_code.value,
            "error_message": response.error_message,
        }
        return JsonMessageCodec._encode(document)  # 编码失败统一转 CodecError，不把 json 异常泄漏到页面。

    @staticmethod
    def decode_response(data: bytes) -> ClientResponse:
        document = JsonMessageCodec._decode(data, "response")  # 先挡住错误 kind、非法 UTF-8 和非对象根节点。
        try:
            return ClientResponse(  # 枚举构造会拒绝未知状态/错误码，防止页面收到不可判断的字符串。
                request_id=JsonMessageCodec._text(document, "request_id"),
                session_id=JsonMessageCodec._text(document, "session_id"),
                msg_type=JsonMessageCodec._text(document, "msg_type"),
                func_id=JsonMessageCodec._text(document, "func_id"),
                status=MessageStatus(JsonMessageCodec._text(document, "status")),
                payload=JsonMessageCodec._payload(document),  # 应答数据必须保持对象形状。
                error_code=ErrorCode(JsonMessageCodec._text(document, "error_code")),
                error_message=JsonMessageCodec._text(document, "error_message", required=False),
            )
        except (TypeError, ValueError) as error:
            raise CodecError(f"invalid response fields: {error}") from error  # 调用方按协议错误走失败分支。

    @staticmethod
    def _encode(document: Mapping[str, Any]) -> bytes:
        try:
            # sort_keys 让同一输入产生稳定字节，便于日志比对和单元测试。
            return json.dumps(document, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")  # 稳定键序和 UTF-8 字节便于重放、比对和测试。
        except (TypeError, ValueError) as error:
            raise CodecError(f"message is not JSON serializable: {error}") from error  # 不可序列化输入在发送前终止，避免半包进入传输层。

    @staticmethod
    def _decode(data: bytes, expected_kind: str) -> Mapping[str, Any]:
        try:
            document = json.loads(data.decode("utf-8"))  # 先按协议约定解码字节；乱码或坏 JSON 在这里被截断。
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise CodecError(f"invalid JSON message: {error}") from error  # 对外只暴露稳定的协议错误类型。
        if not isinstance(document, dict):  # 根节点不是对象时无法安全读取字段，不能继续路由。
            raise CodecError("message root must be an object")
        if document.get("kind") != expected_kind:  # 请求/应答方向错位时立即拒绝，避免错误状态污染 pending。
            raise CodecError(f"expected {expected_kind} message")
        return document

    @staticmethod
    def _text(document: Mapping[str, Any], key: str, *, required: bool = True) -> str:
        value = document.get(key, "")  # 缺失字段按空值处理，再由统一校验给出字段名。
        if not isinstance(value, str) or (required and not value):
            raise CodecError(f"{key} must be a non-empty string")
        return value

    @staticmethod
    def _payload(document: Mapping[str, Any]) -> Mapping[str, Any]:
        value = document.get("payload", {})  # 允许省略 payload 时使用空对象，页面仍得到统一数据形状。
        if not isinstance(value, dict):
            raise CodecError("payload must be an object")
        return value

    @staticmethod
    def _positive_int(document: Mapping[str, Any], key: str) -> int:
        value = document.get(key)  # 不给默认值，缺失 deadline 必须明确报协议错误。
        if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
            raise CodecError(f"{key} must be a positive integer")
        return value
