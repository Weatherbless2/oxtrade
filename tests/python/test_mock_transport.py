from __future__ import annotations

import time
import unittest

from vnpy_oxapp.connection import ClientProtocolClient
from vnpy_oxapp.message import ErrorCode, MessageStatus
from vnpy_oxapp.mock.mock_transport import MockScenario, MockTransport, MockTransportConfig
from vnpy_oxapp.transport import ConnectionState


class MockTransportTest(unittest.TestCase):
    def test_normal_page_response_contains_realistic_mock_records(self) -> None:
        client = ClientProtocolClient(MockTransport())  # 使用默认内存服务，覆盖完整正常链路。
        self.assertTrue(client.connect())  # 只有 CONNECTED 才允许发送请求。
        response = client.request(session_id="mock-session", msg_type="users", func_id="page", payload={"page_size": 2}).result(timeout=0.2)  # 等待异步 Timer 返回。
        self.assertTrue(response.is_success)  # 正常业务必须明确为 succeeded。
        self.assertEqual(2, len(response.payload["items"]))  # 当前页按 page_size 返回两条。
        self.assertEqual(4, response.payload["total"])  # total 验证底层快照总数未被分页截断。

    def test_empty_permission_and_service_error_scenarios(self) -> None:
        checks = (
            (MockScenario.EMPTY, MessageStatus.SUCCEEDED, ErrorCode.NONE),
            (MockScenario.UNAUTHORIZED, MessageStatus.UNAUTHORIZED, ErrorCode.UNAUTHORIZED),
            (MockScenario.SERVICE_ERROR, MessageStatus.FAILED, ErrorCode.SERVICE_ERROR),
        )
        for scenario, status, code in checks:
            with self.subTest(scenario=scenario):
                client = ClientProtocolClient(MockTransport(MockTransportConfig(scenario=scenario)))  # 每个场景隔离 Transport 状态。
                client.connect()  # 统一走连接成功后再注入业务异常。
                response = client.request(session_id="s", msg_type="risk", func_id="page").result(timeout=0.2)  # 等待异步结果收口。
                self.assertEqual(status, response.status)  # 空数据/无权限/服务错误必须落到不同终态。
                self.assertEqual(code, response.error_code)  # 错误分类不能只依赖可读文本。

    def test_connect_failure_and_disconnect_change_state(self) -> None:
        states = []
        failed = MockTransport(MockTransportConfig(scenario=MockScenario.CONNECT_FAILURE))
        failed.set_state_callback(lambda state, reason: states.append(state))  # 记录状态序列，验证连接失败边界。
        self.assertFalse(failed.connect())  # 故意注入拒绝，connect 必须返回 False。
        self.assertEqual([ConnectionState.CONNECTING, ConnectionState.FAILED], states)  # 状态不可跳过 connecting 或伪造 connected。
        disconnected = MockTransport(MockTransportConfig(scenario=MockScenario.DISCONNECT))
        client = ClientProtocolClient(disconnected)
        self.assertTrue(client.connect())  # 先建立连接，再模拟 send 期间断开。
        result = client.request(session_id="s", msg_type="users", func_id="page").result(timeout=0.1)  # False 发送结果必须完成 Future。
        self.assertEqual(ErrorCode.DISCONNECTED, result.error_code)  # 页面看到稳定断线分类而不是永久等待。
        self.assertEqual(ConnectionState.DISCONNECTED, disconnected.state)  # 断开场景必须释放传输状态。

    def test_duplicate_response_is_delivered_once(self) -> None:
        client = ClientProtocolClient(MockTransport(MockTransportConfig(scenario=MockScenario.DUPLICATE)))
        observed = []  # 收集页面回调次数，直接验收“只交付一次”。
        client.set_response_callback(observed.append)  # 回调代表 UI/Gateway 可见结果。
        client.connect()  # 启动 Timer 传输。
        response = client.request(session_id="s", msg_type="users", func_id="page").result(timeout=0.2)  # 第一包完成 Future。
        self.assertTrue(response.is_success)  # 第一包应为成功。
        time.sleep(0.05)  # 留出重复 Timer 触发时间，故意制造真实网络重试窗口。
        self.assertEqual(1, len(observed))  # 第二包应被 tracker 拒绝，页面不能重复刷新。
