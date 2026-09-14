from __future__ import annotations

from pathlib import Path
from tempfile import TemporaryDirectory
import os
import unittest
from unittest.mock import patch

from vnpy_oxapp.hpsocket_transport import HpSocketTransport
from vnpy_oxapp.runtime_config import ClientRuntimeConfig, create_client_transport, load_client_runtime_config


class RuntimeConfigTest(unittest.TestCase):
    def test_missing_config_keeps_mock_default(self) -> None:
        with TemporaryDirectory() as directory:
            config = load_client_runtime_config(Path(directory) / "missing.ini")
        self.assertEqual("mock", config.transport_kind)
        self.assertEqual("mock-session", config.session_id)

    def test_environment_selects_formal_hpsocket_transport(self) -> None:
        with patch.dict(os.environ, {
            "OXAPP_TRANSPORT": "hpsocket",
            "OXAPP_HPSOCKET_HOST": "172.26.112.1",
            "OXAPP_HPSOCKET_PORT": "18080",
            "OXAPP_SESSION_ID": "desktop-readonly",
        }, clear=False):
            config = load_client_runtime_config(Path("/tmp/oxapp-runtime-config-missing.ini"))
        self.assertEqual("hpsocket", config.transport_kind)
        self.assertEqual("172.26.112.1", config.host)
        self.assertEqual(18080, config.port)
        self.assertIsInstance(create_client_transport(config), HpSocketTransport)

    def test_invalid_transport_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            ClientRuntimeConfig(transport_kind="unknown").validate()


if __name__ == "__main__":
    unittest.main()
