from __future__ import annotations

from datetime import datetime
import unittest

from vnpy_oxapp.market.event import MarketEvent, MarketPayloadError, split_symbol


def quote_payload(**changes):
    payload = {  # 构造一条字段形状与 QuantFabric Bridge 一致的股票快照，测试只覆盖本机开发协议边界。
        "type": "stock_quote", "ticker": "600519", "name": "贵州茅台", "exchange": "SSE",
        "update_time": "09:30:01", "millisec": 123, "last_price": 1462.5, "volume": 120000,
        "turnover": 175500000, "pre_close": 1450, "open": 1451, "high": 1465, "low": 1448,
        "bid_prices": [1462.49, 1462.48], "bid_volumes": [100, 200],
        "ask_prices": [1462.50, 1462.51], "ask_volumes": [300, 400], "source": "pytdx",
    }
    payload.update(changes)  # 每个异常用例只替换目标字段，其他合法字段保持不变，失败原因才能精确定位。
    return payload


class MarketEventTest(unittest.TestCase):
    def test_bridge_payload_is_normalized_to_internal_symbol_and_five_levels(self) -> None:
        event = MarketEvent.from_bridge_payload(quote_payload(), received_at=datetime(2026, 8, 27, 10, 0, 0))  # 固定接收日，避免仅含时分秒的源时间随测试日期变化。
        self.assertEqual("600519.SH", event.symbol)  # SSE+ticker 必须转换成页面统一代码，否则订阅和快照无法按同一键匹配。
        self.assertEqual("09:30:01.123", event.update_time)  # 验证 Bridge 的独立 millisec 字段确实进入源时间。
        self.assertEqual((1462.49, 1462.48, 0.0, 0.0, 0.0), event.bid_prices)  # 两档输入补成固定五档，页面下标访问不会越界。
        self.assertEqual((300, 400, 0, 0, 0), event.ask_volumes)  # 数量必须保持整数，缺失三档明确补零。

    def test_invalid_type_and_negative_values_are_rejected_before_reaching_hub(self) -> None:
        with self.assertRaises(MarketPayloadError):
            MarketEvent.from_bridge_payload(quote_payload(type="subscription_added"))  # 故意注入订阅确认，证明控制消息不会被误当价格 Tick。
        with self.assertRaises(MarketPayloadError):
            MarketEvent.from_bridge_payload(quote_payload(volume=-1))  # 故意注入负累计量，避免错误数据污染 K 线差分和风控展示。

    def test_bridge_subscription_symbol_conversion_only_accepts_supported_a_shares(self) -> None:
        self.assertEqual(("000001", "SZSE"), split_symbol("000001.SZ"))
        self.assertEqual(("600519", "SSE"), split_symbol("600519.SH"))
        with self.assertRaises(ValueError):
            split_symbol("IF2609.CFX")  # 期货不在 PyTdx 股票 Bridge 范围，应留给既有 Mock/未来正式 SDK，而不是猜错交易所。
