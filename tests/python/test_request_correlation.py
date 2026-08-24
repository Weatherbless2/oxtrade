from __future__ import annotations

import time
import unittest

from vnpy_oxapp.connection import ClientProtocolClient, RequestResolution, RequestTracker
from vnpy_oxapp.message import ClientResponse, ErrorCode, MessageStatus
from vnpy_oxapp.mock.mock_transport import MockScenario, MockTransport, MockTransportConfig


class RequestCorrelationTest(unittest.TestCase):
    def test_response_requires_same_session_and_rejects_duplicate(self) -> None:
        tracker = RequestTracker()
        request = tracker.create_request(session_id="s-1", msg_type="users", func_id="page")  # 建立 pending 和单调 deadline。
        wrong_session = ClientResponse(request.request_id, "s-2", "users", "page", MessageStatus.SUCCEEDED)  # request_id 正确但会话错误，模拟串会话回包。
        self.assertEqual(RequestResolution.SESSION_MISMATCH, tracker.resolve(wrong_session))  # 错包不能删除 pending。
        valid = ClientResponse(request.request_id, "s-1", "users", "page", MessageStatus.SUCCEEDED)  # 构造可信回包。
        self.assertEqual(RequestResolution.ACCEPTED, tracker.resolve(valid))  # 首次可信回包才消费 pending。
        self.assertEqual(RequestResolution.DUPLICATE, tracker.resolve(valid))  # 同一对象再次到达必须被识别为重复。

    def test_timeout_response_and_late_response_do_not_update_twice(self) -> None:
        transport = MockTransport(MockTransportConfig(scenario=MockScenario.LATE, response_delay_ms=150))
        client = ClientProtocolClient(transport)
        delivered = []  # 记录页面可见结果，超时和迟到包合计只能交付一次。
        client.set_response_callback(delivered.append)  # 观察 tracker 最终输出而不是 Transport 内部 Timer。
        self.assertTrue(client.connect())  # 先建立连接，确保后面的等待来自延迟而非断线。
        future = client.request(session_id="s-1", msg_type="users", func_id="page", timeout_ms=10)  # 10ms deadline 小于 Mock 150ms 延迟。
        time.sleep(0.03)  # 等过本地 deadline，模拟页面定时任务发现过期。
        timeout_responses = client.expire_requests()  # 主动扫描并生成超时应答。
        self.assertEqual(1, len(timeout_responses))  # 恰好回收一个 pending。
        self.assertEqual(MessageStatus.TIMED_OUT, timeout_responses[0].status)  # 超时结果必须是专门终态。
        self.assertEqual(MessageStatus.TIMED_OUT, future.result(timeout=0.1).status)  # 原 Future 被超时结果唤醒。
        time.sleep(0.2)  # 等迟到 Timer 真正触发，验证异常不是靠提前结束测试掩盖。
        self.assertEqual(1, len(delivered))  # 迟到包被 tracker 拒绝，不二次刷新页面。

    def test_disconnected_request_finishes_with_error(self) -> None:
        client = ClientProtocolClient(MockTransport())  # 不 connect，直接模拟调用方忘记建立链路。
        response = client.request(session_id="s-1", msg_type="users", func_id="page").result(timeout=0.1)  # send=False 仍必须完成 Future。
        self.assertEqual(ErrorCode.DISCONNECTED, response.error_code)  # 断线是明确错误，不允许调用方永久阻塞。
