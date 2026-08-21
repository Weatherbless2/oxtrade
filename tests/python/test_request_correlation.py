from __future__ import annotations

import time
import unittest

from vnpy_oxapp.connection import ClientProtocolClient, RequestResolution, RequestTracker
from vnpy_oxapp.message import ClientResponse, ErrorCode, MessageStatus
from vnpy_oxapp.mock.mock_transport import MockScenario, MockTransport, MockTransportConfig


class RequestCorrelationTest(unittest.TestCase):
    def test_response_requires_same_session_and_rejects_duplicate(self) -> None:
        tracker = RequestTracker()
        request = tracker.create_request(session_id="s-1", msg_type="users", func_id="page")
        wrong_session = ClientResponse(request.request_id, "s-2", "users", "page", MessageStatus.SUCCEEDED)
        self.assertEqual(RequestResolution.SESSION_MISMATCH, tracker.resolve(wrong_session))
        valid = ClientResponse(request.request_id, "s-1", "users", "page", MessageStatus.SUCCEEDED)
        self.assertEqual(RequestResolution.ACCEPTED, tracker.resolve(valid))
        self.assertEqual(RequestResolution.DUPLICATE, tracker.resolve(valid))

    def test_timeout_response_and_late_response_do_not_update_twice(self) -> None:
        transport = MockTransport(MockTransportConfig(scenario=MockScenario.LATE, response_delay_ms=150))
        client = ClientProtocolClient(transport)
        delivered = []
        client.set_response_callback(delivered.append)
        self.assertTrue(client.connect())
        future = client.request(session_id="s-1", msg_type="users", func_id="page", timeout_ms=10)
        time.sleep(0.03)
        timeout_responses = client.expire_requests()
        self.assertEqual(1, len(timeout_responses))
        self.assertEqual(MessageStatus.TIMED_OUT, timeout_responses[0].status)
        self.assertEqual(MessageStatus.TIMED_OUT, future.result(timeout=0.1).status)
        time.sleep(0.2)
        self.assertEqual(1, len(delivered))  # 迟到 Mock 回包被 tracker 拒绝，不二次刷新页面。

    def test_disconnected_request_finishes_with_error(self) -> None:
        client = ClientProtocolClient(MockTransport())
        response = client.request(session_id="s-1", msg_type="users", func_id="page").result(timeout=0.1)
        self.assertEqual(ErrorCode.DISCONNECTED, response.error_code)
