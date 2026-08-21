from __future__ import annotations

import time
import unittest

from vnpy_oxapp.connection import ClientProtocolClient
from vnpy_oxapp.message import ErrorCode, MessageStatus
from vnpy_oxapp.mock.mock_transport import MockScenario, MockTransport, MockTransportConfig
from vnpy_oxapp.transport import ConnectionState


class MockTransportTest(unittest.TestCase):
    def test_normal_page_response_contains_realistic_mock_records(self) -> None:
        client = ClientProtocolClient(MockTransport())
        self.assertTrue(client.connect())
        response = client.request(session_id="mock-session", msg_type="users", func_id="page", payload={"page_size": 2}).result(timeout=0.2)
        self.assertTrue(response.is_success)
        self.assertEqual(2, len(response.payload["items"]))
        self.assertEqual(4, response.payload["total"])

    def test_empty_permission_and_service_error_scenarios(self) -> None:
        checks = (
            (MockScenario.EMPTY, MessageStatus.SUCCEEDED, ErrorCode.NONE),
            (MockScenario.UNAUTHORIZED, MessageStatus.UNAUTHORIZED, ErrorCode.UNAUTHORIZED),
            (MockScenario.SERVICE_ERROR, MessageStatus.FAILED, ErrorCode.SERVICE_ERROR),
        )
        for scenario, status, code in checks:
            with self.subTest(scenario=scenario):
                client = ClientProtocolClient(MockTransport(MockTransportConfig(scenario=scenario)))
                client.connect()
                response = client.request(session_id="s", msg_type="risk", func_id="page").result(timeout=0.2)
                self.assertEqual(status, response.status)
                self.assertEqual(code, response.error_code)

    def test_connect_failure_and_disconnect_change_state(self) -> None:
        states = []
        failed = MockTransport(MockTransportConfig(scenario=MockScenario.CONNECT_FAILURE))
        failed.set_state_callback(lambda state, reason: states.append(state))
        self.assertFalse(failed.connect())
        self.assertEqual([ConnectionState.CONNECTING, ConnectionState.FAILED], states)
        disconnected = MockTransport(MockTransportConfig(scenario=MockScenario.DISCONNECT))
        client = ClientProtocolClient(disconnected)
        self.assertTrue(client.connect())
        result = client.request(session_id="s", msg_type="users", func_id="page").result(timeout=0.1)
        self.assertEqual(ErrorCode.DISCONNECTED, result.error_code)
        self.assertEqual(ConnectionState.DISCONNECTED, disconnected.state)

    def test_duplicate_response_is_delivered_once(self) -> None:
        client = ClientProtocolClient(MockTransport(MockTransportConfig(scenario=MockScenario.DUPLICATE)))
        observed = []
        client.set_response_callback(observed.append)
        client.connect()
        response = client.request(session_id="s", msg_type="users", func_id="page").result(timeout=0.2)
        self.assertTrue(response.is_success)
        time.sleep(0.05)
        self.assertEqual(1, len(observed))
