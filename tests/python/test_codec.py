from __future__ import annotations

import unittest

from vnpy_oxapp.codec import CodecError, JsonMessageCodec
from vnpy_oxapp.message import ClientRequest, ClientResponse, MessageStatus, unix_time_ms


class JsonMessageCodecTest(unittest.TestCase):
    def test_request_and_response_round_trip(self) -> None:
        request = ClientRequest("r-1", "s-1", "users", "page", {"query": "yangjh"}, unix_time_ms() + 1_000)  # 构造含关联、过滤和 deadline 的完整请求。
        decoded_request = JsonMessageCodec.decode_request(JsonMessageCodec.encode_request(request))  # 验证对象 -> bytes -> 对象不丢字段。
        self.assertEqual(request, decoded_request)  # 任一字段变化都说明协议布局不稳定。
        response = ClientResponse("r-1", "s-1", "users", "page", MessageStatus.SUCCEEDED, {"total": 4})  # 构造成功应答。
        decoded_response = JsonMessageCodec.decode_response(JsonMessageCodec.encode_response(response))  # 同样验证应答方向的往返。
        self.assertEqual(response, decoded_response)  # 成功状态、payload 和关联字段必须完全保留。

    def test_rejects_wrong_message_kind_and_bad_deadline(self) -> None:
        with self.assertRaises(CodecError):  # 故意把 request 当 response，验证方向闸门会阻止错包进入业务。
            JsonMessageCodec.decode_response(b'{"kind":"request"}')
        with self.assertRaises(CodecError):  # 故意使用 0 deadline，验证无期限请求不会进入传输层。
            JsonMessageCodec.decode_request(b'{"kind":"request","request_id":"r","session_id":"s","msg_type":"m","func_id":"f","deadline_unix_ms":0}')
