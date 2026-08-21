from __future__ import annotations

import unittest

from vnpy_oxapp.codec import CodecError, JsonMessageCodec
from vnpy_oxapp.message import ClientRequest, ClientResponse, MessageStatus, unix_time_ms


class JsonMessageCodecTest(unittest.TestCase):
    def test_request_and_response_round_trip(self) -> None:
        request = ClientRequest("r-1", "s-1", "users", "page", {"query": "yangjh"}, unix_time_ms() + 1_000)
        decoded_request = JsonMessageCodec.decode_request(JsonMessageCodec.encode_request(request))
        self.assertEqual(request, decoded_request)
        response = ClientResponse("r-1", "s-1", "users", "page", MessageStatus.SUCCEEDED, {"total": 4})
        decoded_response = JsonMessageCodec.decode_response(JsonMessageCodec.encode_response(response))
        self.assertEqual(response, decoded_response)

    def test_rejects_wrong_message_kind_and_bad_deadline(self) -> None:
        with self.assertRaises(CodecError):
            JsonMessageCodec.decode_response(b'{"kind":"request"}')
        with self.assertRaises(CodecError):
            JsonMessageCodec.decode_request(b'{"kind":"request","request_id":"r","session_id":"s","msg_type":"m","func_id":"f","deadline_unix_ms":0}')
