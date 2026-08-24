from __future__ import annotations

import queue
import unittest

from vnpy_oxapp.gateway import EVENT_OXAPP_CONNECTION, EVENT_OXAPP_RESPONSE, OxAppGateway, VNPY_AVAILABLE
from vnpy_oxapp.mock.mock_transport import MockTransport


@unittest.skipUnless(VNPY_AVAILABLE, "vn.py is optional for the core communication package")
class VnpyGatewayTest(unittest.TestCase):
    def test_gateway_publishes_transport_state_and_response_events(self) -> None:
        from vnpy.event import EventEngine

        event_engine = EventEngine()  # vn.py 事件总线是 Gateway 与 Monitor 的唯一交付边界。
        gateway = OxAppGateway(event_engine, transport=MockTransport())  # 注入 Mock，隔离真实网络依赖。
        gateway.connect({})  # Gateway 把连接状态转成 eOxAppConnection。
        response = gateway.client.request(session_id="mock-session", msg_type="users", func_id="page").result(timeout=0.2)  # 发起一条可关联查询。
        self.assertTrue(response.is_success)  # 先验收客户端响应，再检查事件总线。
        queued_events = []
        while True:
            try:
                queued_events.append(event_engine._queue.get_nowait())  # 验证 Gateway 写入 EventEngine，而非直接访问 UI。
            except queue.Empty:
                break
        event_types = [event.type for event in queued_events]
        self.assertIn(EVENT_OXAPP_CONNECTION, event_types)  # 连接状态事件必须可被 vn.py 订阅。
        self.assertIn(EVENT_OXAPP_RESPONSE, event_types)  # 完整响应事件必须进入同一事件队列。
