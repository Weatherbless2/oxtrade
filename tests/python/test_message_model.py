from __future__ import annotations

import unittest

from vnpy_oxapp.message import ClientRequest, ClientResponse, ErrorCode, MessageStatus, unix_time_ms


class MessageModelTest(unittest.TestCase):
    def test_request_requires_correlation_and_deadline(self) -> None:
        with self.assertRaises(ValueError):  # 故意缺 request_id，验证回包无法关联时在模型入口就被拒绝。
            ClientRequest("", "session", "users", "page", {}, unix_time_ms() + 1)
        with self.assertRaises(ValueError):  # 故意给 0 deadline，验证请求必须有可回收的有效期。
            ClientRequest("request", "session", "users", "page", {}, 0)

    def test_failure_copies_request_correlation(self) -> None:
        request = ClientRequest("request-1", "session-1", "users", "page", {}, unix_time_ms() + 1_000)  # 准备一个可关联的原请求。
        response = ClientResponse.failure(request, ErrorCode.TIMEOUT, "expired", status=MessageStatus.TIMED_OUT)  # 模拟客户端主动生成超时结果。
        self.assertEqual("request-1", response.request_id)  # 超时不能丢失 request_id，否则 Future 无法完成。
        self.assertEqual("session-1", response.session_id)  # 会话边界必须跟随错误应答。
        self.assertEqual(MessageStatus.TIMED_OUT, response.status)  # 页面应看到超时终态，而不是普通成功。
