from __future__ import annotations

import unittest

from vnpy_oxapp.message import ClientRequest, ClientResponse, ErrorCode, MessageStatus, unix_time_ms


class MessageModelTest(unittest.TestCase):
    def test_request_requires_correlation_and_deadline(self) -> None:
        with self.assertRaises(ValueError):
            ClientRequest("", "session", "users", "page", {}, unix_time_ms() + 1)
        with self.assertRaises(ValueError):
            ClientRequest("request", "session", "users", "page", {}, 0)

    def test_failure_copies_request_correlation(self) -> None:
        request = ClientRequest("request-1", "session-1", "users", "page", {}, unix_time_ms() + 1_000)
        response = ClientResponse.failure(request, ErrorCode.TIMEOUT, "expired", status=MessageStatus.TIMED_OUT)
        self.assertEqual("request-1", response.request_id)
        self.assertEqual("session-1", response.session_id)
        self.assertEqual(MessageStatus.TIMED_OUT, response.status)
