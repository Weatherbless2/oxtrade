from __future__ import annotations

import queue
import unittest

from vnpy_oxapp.gateway import EVENT_OXAPP_CONNECTION, EVENT_OXAPP_RESPONSE, OxAppGateway, VNPY_AVAILABLE
from vnpy_oxapp.mock.mock_transport import MockTransport


@unittest.skipUnless(VNPY_AVAILABLE, "vn.py is optional for the core communication package")
class VnpyGatewayTest(unittest.TestCase):
    def test_gateway_publishes_transport_state_and_response_events(self) -> None:
        from vnpy.event import EventEngine

        event_engine = EventEngine()
        gateway = OxAppGateway(event_engine, transport=MockTransport())
        gateway.connect({})
        response = gateway.client.request(session_id="mock-session", msg_type="users", func_id="page").result(timeout=0.2)
        self.assertTrue(response.is_success)
        queued_events = []
        while True:
            try:
                queued_events.append(event_engine._queue.get_nowait())  # 验证 Gateway 写入的是 vn.py EventEngine，而非直接访问 UI。
            except queue.Empty:
                break
        event_types = [event.type for event in queued_events]
        self.assertIn(EVENT_OXAPP_CONNECTION, event_types)
        self.assertIn(EVENT_OXAPP_RESPONSE, event_types)
