from __future__ import annotations

import re
import tempfile
import unittest
from io import StringIO
from pathlib import Path

from vnpy_oxapp.logging import ClientLogger, LogLevel


class ClientLoggerTest(unittest.TestCase):
    def test_open_creates_required_filename_and_format(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = StringIO()
            logger = ClientLogger(stdout=output)
            path = logger.open(directory)
            logger.info("client_started")
            logger.close()
            self.assertRegex(path.name, r"^oxTrader_\d{8}_\d{6}\.log$")
            line = path.read_text(encoding="utf-8").strip()
            self.assertRegex(line, r"^\d{8} \d{6}\d{3}\|\[\d{5}\]\[\d{6,}\]\[oxApp\]\[INFO\]: client_started$")
            self.assertIn(line, output.getvalue())

    def test_level_filter_and_load_timing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = StringIO()
            logger = ClientLogger(minimum_level=LogLevel.INFO, stdout=output)
            logger.open(directory)
            timings = []
            with logger.measure("control_a", sink=timings):
                pass
            logger.debug("hidden")
            logger.close()
            self.assertEqual(1, len(timings))
            self.assertEqual("control_a", timings[0].component)
            self.assertGreaterEqual(timings[0].elapsed_ms, 0)
            self.assertNotIn("hidden", output.getvalue())
            self.assertIn("component_load_end component=control_a", output.getvalue())
