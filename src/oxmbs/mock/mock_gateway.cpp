#include "oxmbs/mock/mock_gateway.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace oxmbs::mock {
namespace {

using Record = std::map<std::string, std::string>;
using Records = std::vector<Record>;

std::string value(const MockForm& form, std::string_view key) {
    const auto found = form.find(std::string(key));
    return found == form.end() ? std::string{} : found->second;
}

std::string lower_copy(std::string text) {
    for (char& character : text) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

std::string json_escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (const char character : text) {
        switch (character) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(character);
            break;
        }
    }
    return escaped;
}

std::string json_record(const Record& record) {
    std::string output{"{"};
    bool first = true;
    for (const auto& [key, item] : record) {
        if (!first) {
            output += ",";
        }
        first = false;
        output += "\"" + json_escape(key) + "\":\"" + json_escape(item) + "\"";
    }
    output += "}";
    return output;
}

std::string json_records(const Records& records) {
    std::string output{"["};
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (index != 0) {
            output += ",";
        }
        output += json_record(records[index]);
    }
    output += "]";
    return output;
}

std::string number(int value) {
    return std::to_string(value);
}

std::string fixed(double value, int precision = 2) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(precision) << value;
    return output.str();
}

int integer(const MockForm& form, std::string_view key, int fallback = 0) {
    try {
        return std::stoi(value(form, key));
    } catch (...) {
        return fallback;
    }
}

double decimal(const MockForm& form, std::string_view key, double fallback = 0.0) {
    try {
        return std::stod(value(form, key));
    } catch (...) {
        return fallback;
    }
}

std::string padded(int value, int width = 4) {
    std::ostringstream output;
    output << std::setw(width) << std::setfill('0') << value;
    return output.str();
}

bool is_future_symbol(std::string_view symbol) {
    return symbol.find(".CFX") != std::string_view::npos;
}

double record_decimal(const Record& record, std::string_view key, double fallback = 0.0) {
    const auto found = record.find(std::string(key));
    if (found == record.end()) {
        return fallback;
    }
    try {
        return std::stod(found->second);
    } catch (...) {
        return fallback;
    }
}

int record_integer(const Record& record, std::string_view key, int fallback = 0) {
    const auto found = record.find(std::string(key));
    if (found == record.end()) {
        return fallback;
    }
    try {
        return std::stoi(found->second);
    } catch (...) {
        return fallback;
    }
}

Record* find_record(Records& records, std::string_view key, std::string_view expected) {
    const auto found = std::find_if(records.begin(), records.end(), [key, expected](const Record& item) {
        const auto value = item.find(std::string(key));
        return value != item.end() && value->second == expected;
    });
    return found == records.end() ? nullptr : &*found;
}

const Record* find_record(const Records& records, std::string_view key, std::string_view expected) {
    const auto found = std::find_if(records.begin(), records.end(), [key, expected](const Record& item) {
        const auto value = item.find(std::string(key));
        return value != item.end() && value->second == expected;
    });
    return found == records.end() ? nullptr : &*found;
}

double quote_last(const Records& quotes, std::string_view symbol, double fallback) {
    const Record* quote = find_record(quotes, "symbol", symbol);
    return quote == nullptr ? fallback : record_decimal(*quote, "last", fallback);
}

std::string quote_name(const Records& quotes, std::string_view symbol, const std::string& fallback) {
    const Record* quote = find_record(quotes, "symbol", symbol);
    if (quote == nullptr) {
        return fallback;
    }
    const auto name = quote->find("name");
    return name == quote->end() ? fallback : name->second;
}

MockGatewayResult failure(std::string code, std::string message) {
    MockGatewayResult result;
    result.success = false;
    result.app_status = "failed";
    result.error_code = std::move(code);
    result.error_message = std::move(message);
    return result;
}

MockGatewayResult page_result(const Records& rows, int page, int page_size) {
    const int safe_page = std::max(1, page);
    const int safe_size = std::min(20, std::max(1, page_size));
    const std::size_t start = std::min(rows.size(), static_cast<std::size_t>((safe_page - 1) * safe_size));
    const std::size_t end = std::min(rows.size(), start + static_cast<std::size_t>(safe_size));
    Records selected(rows.begin() + static_cast<std::ptrdiff_t>(start), rows.begin() + static_cast<std::ptrdiff_t>(end));
    MockGatewayResult result;
    result.payload_json = "{\"items\":" + json_records(selected) +
                          ",\"page\":" + number(safe_page) +
                          ",\"page_size\":" + number(safe_size) +
                          ",\"total\":" + number(static_cast<int>(rows.size())) +
                          ",\"source\":\"mock-gateway\"}";
    return result;
}

bool matches(const Record& row, const MockForm& form) {
    const std::string status = value(form, "status");
    if (!status.empty()) {
        const auto found = row.find("status");
        if (found == row.end() || found->second != status) {
            return false;
        }
    }
    const std::string symbol = value(form, "symbol");
    if (!symbol.empty() && row.find("symbol") != row.end() && row.at("symbol") != symbol) {
        return false;
    }
    const std::string query = lower_copy(value(form, "query"));
    if (!query.empty()) {
        std::string haystack;
        for (const auto& [key, item] : row) {
            haystack += key + "=" + item + ";";
        }
        if (lower_copy(haystack).find(query) == std::string::npos) {
            return false;
        }
    }
    return true;
}

}  // namespace

MockGateway::MockGateway() {
    const std::vector<std::pair<std::string, std::string>> instruments{
        {"000001.SZ", "平安银行"}, {"600000.SH", "浦发银行"}, {"600519.SH", "贵州茅台"},
        {"000300.SH", "沪深300"}, {"510300.SH", "300ETF"}, {"IF2609.CFX", "IF2609"},
        {"IC2609.CFX", "IC2609"}, {"159915.SZ", "创业板ETF"},
    };
    for (std::size_t index = 0; index < instruments.size(); ++index) {
        const double pre_close = 10.0 + static_cast<double>(index) * 8.35;
        const double last = pre_close + (static_cast<int>(index % 5) - 2) * 0.17;
        resources_["quotes"].push_back({
            {"symbol", instruments[index].first}, {"name", instruments[index].second},
            {"market", instruments[index].first.find(".CFX") != std::string::npos ? "期货" : "股票/ETF"},
            {"last", fixed(last)}, {"pre_close", fixed(pre_close)}, {"open", fixed(pre_close + 0.08)},
            {"high", fixed(last + 0.42)}, {"low", fixed(last - 0.38)},
            {"change", fixed(last - pre_close)}, {"change_percent", fixed((last - pre_close) / pre_close * 100.0)},
            {"volume", number(120000 + static_cast<int>(index) * 18300)}, {"amount", fixed(2300000 + index * 810000, 0)},
            {"bid1", fixed(last - 0.02)}, {"bid1_volume", number(1200 + static_cast<int>(index) * 100)},
            {"ask1", fixed(last + 0.02)}, {"ask1_volume", number(980 + static_cast<int>(index) * 80)},
            {"update_time", "2026-08-24 10:30:" + padded(static_cast<int>(index + 10), 2)},
        });
        for (int bar = 0; bar < 60; ++bar) {
            const double close = last + std::sin(static_cast<double>(bar) / 5.0) * 0.35;
            const int minute = 30 + bar;
            resources_["bars"].push_back({
                {"symbol", instruments[index].first}, {"datetime", "2026-08-24 " + padded(9 + minute / 60, 2) + ":" + padded(minute % 60, 2) + ":00"},
                {"open", fixed(close - 0.11)}, {"high", fixed(close + 0.24)}, {"low", fixed(close - 0.28)},
                {"close", fixed(close)}, {"volume", number(8000 + bar * 127)}, {"amount", fixed(close * (8000 + bar * 127), 0)},
            });
        }
        resources_["depth"].push_back({
            {"symbol", instruments[index].first}, {"name", instruments[index].second},
            {"bid1_price", fixed(last - 0.02)}, {"bid1_volume", number(1200 + static_cast<int>(index) * 100)},
            {"bid2_price", fixed(last - 0.04)}, {"bid2_volume", number(1400 + static_cast<int>(index) * 100)},
            {"bid3_price", fixed(last - 0.06)}, {"bid3_volume", number(1700 + static_cast<int>(index) * 100)},
            {"ask1_price", fixed(last + 0.02)}, {"ask1_volume", number(980 + static_cast<int>(index) * 80)},
            {"ask2_price", fixed(last + 0.04)}, {"ask2_volume", number(1300 + static_cast<int>(index) * 80)},
            {"ask3_price", fixed(last + 0.06)}, {"ask3_volume", number(1600 + static_cast<int>(index) * 80)},
        });
        for (int tick = 0; tick < 80; ++tick) {
            const double tick_price = last + std::sin(static_cast<double>(tick) / 4.0) * 0.08;
            resources_["ticks"].push_back({
                {"symbol", instruments[index].first}, {"name", instruments[index].second},
                {"datetime", "2026-08-24 10:" + padded(20 + tick / 12, 2) + ":" + padded((tick * 5) % 60, 2)},
                {"last", fixed(tick_price)}, {"volume", number(100 + tick * 7)}, {"turnover", fixed(tick_price * (100 + tick * 7), 0)},
                {"bid1", fixed(tick_price - 0.02)}, {"ask1", fixed(tick_price + 0.02)}, {"open_interest", number(12000 + tick * 3)},
            });
        }
    }

    resources_["accounts"] = {
        {{"account_id", "MOCK-ACCT-001"}, {"account_name", "量化测试账户"}, {"currency", "CNY"}, {"total_asset", "12500000.00"}, {"available", "8235000.00"}, {"frozen", "125000.00"}, {"market_value", "4140000.00"}, {"daily_pnl", "38600.00"}, {"status", "active"}},
        {{"account_id", "MOCK-ACCT-002"}, {"account_name", "期货模拟账户"}, {"currency", "CNY"}, {"total_asset", "3200000.00"}, {"available", "2860000.00"}, {"frozen", "86000.00"}, {"market_value", "254000.00"}, {"daily_pnl", "-8200.00"}, {"status", "active"}},
    };
    resources_["positions"] = {
        {{"position_id", "POS-0001"}, {"account_id", "MOCK-ACCT-001"}, {"symbol", "000001.SZ"}, {"name", "平安银行"}, {"side", "多"}, {"total_volume", "12000"}, {"available_volume", "12000"}, {"today_volume", "0"}, {"yesterday_volume", "12000"}, {"frozen_volume", "0"}, {"cost_price", "10.12"}, {"last_price", "10.28"}, {"market_value", "123360.00"}, {"pnl", "1920.00"}, {"pnl_percent", "1.58%"}},
        {{"position_id", "POS-0002"}, {"account_id", "MOCK-ACCT-001"}, {"symbol", "600519.SH"}, {"name", "贵州茅台"}, {"side", "多"}, {"total_volume", "500"}, {"available_volume", "500"}, {"today_volume", "0"}, {"yesterday_volume", "500"}, {"frozen_volume", "0"}, {"cost_price", "1450.00"}, {"last_price", "1462.50"}, {"market_value", "731250.00"}, {"pnl", "6250.00"}, {"pnl_percent", "0.86%"}},
        {{"position_id", "POS-0003"}, {"account_id", "MOCK-ACCT-002"}, {"symbol", "IF2609.CFX"}, {"name", "IF2609"}, {"side", "多"}, {"total_volume", "3"}, {"available_volume", "2"}, {"today_volume", "1"}, {"yesterday_volume", "2"}, {"frozen_volume", "1"}, {"cost_price", "3842.20"}, {"last_price", "3850.40"}, {"market_value", "254000.00"}, {"pnl", "24600.00"}, {"pnl_percent", "2.13%"}},
    };
    resources_["orders"] = {
        {{"order_id", "MO-0001"}, {"account_id", "MOCK-ACCT-001"}, {"symbol", "000001.SZ"}, {"name", "平安银行"}, {"side", "买入"}, {"order_type", "限价"}, {"price", "10.20"}, {"volume", "5000"}, {"traded_volume", "5000"}, {"frozen_cash", "0.00"}, {"status", "filled"}, {"order_time", "2026-08-24 09:35:12"}, {"gateway", "MockGateway"}},
        {{"order_id", "MO-0002"}, {"account_id", "MOCK-ACCT-001"}, {"symbol", "600519.SH"}, {"name", "贵州茅台"}, {"side", "卖出"}, {"order_type", "限价"}, {"price", "1465.00"}, {"volume", "100"}, {"traded_volume", "0"}, {"frozen_cash", "0.00"}, {"status", "working"}, {"order_time", "2026-08-24 10:12:08"}, {"gateway", "MockGateway"}},
        {{"order_id", "MO-0003"}, {"account_id", "MOCK-ACCT-002"}, {"symbol", "IF2609.CFX"}, {"name", "IF2609"}, {"side", "买入"}, {"order_type", "限价"}, {"price", "3848.00"}, {"volume", "2"}, {"traded_volume", "1"}, {"frozen_cash", "0.00"}, {"status", "partial"}, {"order_time", "2026-08-24 10:20:43"}, {"gateway", "MockGateway"}},
    };
    next_order_id_ = 4;
    resources_["trades"] = {
        {{"trade_id", "MT-0001"}, {"order_id", "MO-0001"}, {"symbol", "000001.SZ"}, {"name", "平安银行"}, {"side", "买入"}, {"price", "10.20"}, {"volume", "5000"}, {"trade_time", "2026-08-24 09:35:14"}, {"gateway", "MockGateway"}},
        {{"trade_id", "MT-0002"}, {"order_id", "MO-0003"}, {"symbol", "IF2609.CFX"}, {"name", "IF2609"}, {"side", "买入"}, {"price", "3848.00"}, {"volume", "1"}, {"trade_time", "2026-08-24 10:20:45"}, {"gateway", "MockGateway"}},
    };
    next_trade_id_ = 3;
}

MockGatewayResult MockGateway::handle(const std::string& service,
                                      const std::string& operation,
                                      const MockForm& form) {
    if (value(form, "force_error") == "true") {
        return failure("service_error", "mock gateway was asked to fail");
    }
    if (service == "market") {
        if (operation == "subscribe") {
            return subscribe(form);
        }
        if (operation == "quotes") {
            return page("quotes", form);
        }
        if (operation == "ticks") {
            return page("ticks", form);
        }
        if (operation == "bars") {
            return page("bars", form);
        }
        if (operation == "depth") {
            return page("depth", form);
        }
    }
    if (service == "query" || service == "trade") {
        if (operation == "page") {
            return page(value(form, "resource").empty() ? "orders" : value(form, "resource"), form);
        }
        if (service == "trade" && operation == "place_order") {
            return place_order(form);
        }
        if (service == "trade" && operation == "cancel_order") {
            return cancel_order(form);
        }
        if (service == "trade" && operation == "fill_order") {
            return fill_order(form);
        }
    }
    return failure("not_supported", "unsupported mock gateway operation");
}

MockGatewayResult MockGateway::page(const std::string& resource, const MockForm& form) const {
    const auto found = resources_.find(resource);
    if (found == resources_.end()) {
        return failure("not_found", "mock gateway resource was not found");
    }
    Records filtered;
    for (const Record& row : found->second) {
        if (matches(row, form)) {
            filtered.push_back(row);
        }
    }
    return page_result(filtered, integer(form, "page", 1), integer(form, "page_size", 10));
}

MockGatewayResult MockGateway::subscribe(const MockForm& form) const {
    const std::string symbols = value(form, "symbols");
    MockGatewayResult result;
    result.payload_json = "{\"status\":\"subscribed\",\"symbols\":\"" + json_escape(symbols.empty() ? "all" : symbols) +
                          "\",\"stream\":\"MockMarketGateway\",\"reconnect\":\"enabled\"}";
    return result;
}

MockGatewayResult MockGateway::place_order(const MockForm& form) {
    const std::string symbol = value(form, "symbol");
    const std::string side = value(form, "side");
    const int volume = integer(form, "volume");
    const double price = decimal(form, "price");
    if (symbol.empty() || (side != "buy" && side != "sell") || price <= 0.0 || volume <= 0) {
        return failure("validation_error", "symbol, side, positive price and volume are required");
    }
    const bool is_future = is_future_symbol(symbol);
    if (find_record(resources_["quotes"], "symbol", symbol) == nullptr) {
        return failure("validation_error", "mock symbol is not subscribed by the market gateway");
    }
    if (volume < 100 && !is_future) {
        return failure("validation_error", "A-share mock volume must be at least 100");
    }
    if (!is_future && volume % 100 != 0) {
        return failure("validation_error", "A-share mock volume must be an integer multiple of 100");
    }
    if (volume > 500000) {
        return failure("risk_rejected", "mock risk rejected order: single-order volume limit");
    }
    const std::string account_id = value(form, "account_id").empty()
        ? (is_future ? "MOCK-ACCT-002" : "MOCK-ACCT-001") : value(form, "account_id");
    Record* account = find_record(resources_["accounts"], "account_id", account_id);
    if (account == nullptr || account->at("status") != "active") {
        return failure("risk_rejected", "mock risk rejected order: account is unavailable");
    }
    const double notional = price * static_cast<double>(volume);
    const double freeze_cash = is_future ? notional * 300.0 * 0.12 : notional;
    if (side == "buy" && record_decimal(*account, "available") + 1e-9 < freeze_cash) {
        return failure("risk_rejected", "mock risk rejected order: insufficient available cash or margin");
    }
    Record* position = find_record(resources_["positions"], "symbol", symbol);
    if (side == "sell" && (position == nullptr || position->at("account_id") != account_id ||
                            record_integer(*position, "available_volume") < volume)) {
        return failure("risk_rejected", "mock risk rejected order: insufficient sellable position");
    }
    if (side == "buy") {
        (*account)["available"] = fixed(record_decimal(*account, "available") - freeze_cash);
        (*account)["frozen"] = fixed(record_decimal(*account, "frozen") + freeze_cash);  // 限价买入先冻结现金/保证金，成交或撤单才释放，避免 Mock 资金被重复使用。
    } else {
        (*position)["available_volume"] = number(record_integer(*position, "available_volume") - volume);
        (*position)["frozen_volume"] = number(record_integer(*position, "frozen_volume") + volume);  // 卖出委托冻结可卖仓位，保留 A 股 T+1 的可卖数量语义。
    }
    Record order{
        {"order_id", "MO-" + padded(next_order_id_++)}, {"account_id", account_id}, {"symbol", symbol}, {"name", quote_name(resources_["quotes"], symbol, value(form, "name").empty() ? "Mock Instrument" : value(form, "name"))},
        {"side", side == "buy" ? "买入" : "卖出"}, {"order_type", value(form, "order_type").empty() ? "限价" : value(form, "order_type")},
        {"price", fixed(price)}, {"volume", number(volume)}, {"traded_volume", "0"}, {"frozen_cash", fixed(side == "buy" ? freeze_cash : 0.0)}, {"status", "working"},
        {"order_time", "2026-08-24 10:31:00"}, {"gateway", "MockTradeGateway"},
    };
    resources_["orders"].insert(resources_["orders"].begin(), order);
    refresh_account_totals(account_id);
    MockGatewayResult result;
    result.payload_json = "{\"order\":" + json_record(order) + ",\"risk_status\":\"passed\",\"risk_checks\":[\"instrument\",\"volume\",\"cash_or_position\",\"single_order_limit\"],\"route\":\"MockTradeGateway\",\"message\":\"mock order accepted\"}";
    return result;
}

MockGatewayResult MockGateway::cancel_order(const MockForm& form) {
    const std::string order_id = value(form, "order_id");
    for (Record& order : resources_["orders"]) {
        if (order["order_id"] == order_id) {
            if (order["status"] == "filled" || order["status"] == "cancelled") {
                return failure("validation_error", "filled or cancelled order cannot be cancelled");
            }
            const int remaining = record_integer(order, "volume") - record_integer(order, "traded_volume");
            Record* account = find_record(resources_["accounts"], "account_id", order["account_id"]);
            if (account == nullptr) {
                return failure("service_error", "mock order account was not found");
            }
            if (order["side"] == "买入") {
                const double release = record_decimal(order, "frozen_cash");
                (*account)["available"] = fixed(record_decimal(*account, "available") + release);
                (*account)["frozen"] = fixed(std::max(0.0, record_decimal(*account, "frozen") - release));
                order["frozen_cash"] = "0.00";  // frozen_cash 始终代表剩余未成交部分，撤单要一次性返还，不能再次按原总量折算。
            } else {
                Record* position = find_record(resources_["positions"], "symbol", order["symbol"]);
                if (position != nullptr) {
                    (*position)["available_volume"] = number(record_integer(*position, "available_volume") + remaining);
                    (*position)["frozen_volume"] = number(std::max(0, record_integer(*position, "frozen_volume") - remaining));
                }
            }
            order["status"] = "cancelled";
            refresh_account_totals(order["account_id"]);
            MockGatewayResult result;
            result.payload_json = "{\"order\":" + json_record(order) + ",\"message\":\"mock order cancelled\"}";
            return result;
        }
    }
    return failure("not_found", "mock order was not found");
}

MockGatewayResult MockGateway::fill_order(const MockForm& form) {
    const std::string order_id = value(form, "order_id");
    for (Record& order : resources_["orders"]) {
        if (order["order_id"] != order_id) {
            continue;
        }
        if (order["status"] == "cancelled" || order["status"] == "filled") {
            return failure("validation_error", "order cannot be filled in its current state");
        }
        const int volume = record_integer(order, "volume");
        const int traded = record_integer(order, "traded_volume");
        const int remaining = volume - traded;
        const int requested = integer(form, "fill_volume", remaining);
        const int fill_volume = std::min(remaining, std::max(1, requested));
        if (fill_volume <= 0) {
            return failure("validation_error", "mock fill volume must be positive");
        }
        const int after_trade = traded + fill_volume;
        order["traded_volume"] = number(after_trade);
        order["status"] = after_trade == volume ? "filled" : "partial";
        Record* account = find_record(resources_["accounts"], "account_id", order["account_id"]);
        if (account == nullptr) {
            return failure("service_error", "mock order account was not found");
        }
        const double order_price = record_decimal(order, "price");
        const double last_price = quote_last(resources_["quotes"], order["symbol"], order_price);
        const bool is_future = is_future_symbol(order["symbol"]);
        if (order["side"] == "买入") {
            const double frozen_release = record_decimal(order, "frozen_cash") * static_cast<double>(fill_volume) /
                static_cast<double>(std::max(1, remaining));
            (*account)["frozen"] = fixed(std::max(0.0, record_decimal(*account, "frozen") - frozen_release));
            order["frozen_cash"] = fixed(std::max(0.0, record_decimal(order, "frozen_cash") - frozen_release));
            Record* position = find_record(resources_["positions"], "symbol", order["symbol"]);
            if (position == nullptr) {
                Record new_position{
                    {"position_id", "POS-" + padded(static_cast<int>(resources_["positions"].size() + 1))}, {"account_id", order["account_id"]},
                    {"symbol", order["symbol"]}, {"name", order["name"]}, {"side", "多"}, {"total_volume", "0"},
                    {"available_volume", "0"}, {"today_volume", "0"}, {"yesterday_volume", "0"}, {"frozen_volume", "0"},
                    {"cost_price", fixed(order_price)}, {"last_price", fixed(last_price)}, {"market_value", "0.00"}, {"pnl", "0.00"}, {"pnl_percent", "0.00%"},
                };
                resources_["positions"].push_back(std::move(new_position));
                position = &resources_["positions"].back();
            }
            const int prior_total = record_integer(*position, "total_volume");
            const int next_total = prior_total + fill_volume;
            const double weighted_cost = (record_decimal(*position, "cost_price") * static_cast<double>(prior_total) +
                                          order_price * static_cast<double>(fill_volume)) / static_cast<double>(next_total);
            (*position)["total_volume"] = number(next_total);
            (*position)["today_volume"] = number(record_integer(*position, "today_volume") + fill_volume);
            if (is_future) {
                (*position)["available_volume"] = number(record_integer(*position, "available_volume") + fill_volume);  // 期货采用当日可平的 Mock 语义；A 股新增仓位仍受 T+1 限制。
            }
            (*position)["cost_price"] = fixed(weighted_cost);
            (*position)["last_price"] = fixed(last_price);
            const double multiplier = is_future ? 300.0 : 1.0;
            (*account)["market_value"] = fixed(record_decimal(*account, "market_value") + last_price * fill_volume * multiplier);
            (*account)["daily_pnl"] = fixed(record_decimal(*account, "daily_pnl") + (last_price - order_price) * fill_volume * multiplier);
        } else {
            Record* position = find_record(resources_["positions"], "symbol", order["symbol"]);
            if (position == nullptr || record_integer(*position, "total_volume") < fill_volume) {
                return failure("service_error", "mock frozen position was not found");
            }
            (*position)["total_volume"] = number(record_integer(*position, "total_volume") - fill_volume);
            (*position)["frozen_volume"] = number(std::max(0, record_integer(*position, "frozen_volume") - fill_volume));
            const double multiplier = is_future ? 300.0 : 1.0;
            const double realized_pnl = (order_price - record_decimal(*position, "cost_price")) * fill_volume * multiplier;
            (*position)["market_value"] = fixed(std::max(0.0, record_decimal(*position, "market_value") - last_price * fill_volume * multiplier));
            (*account)["available"] = fixed(record_decimal(*account, "available") + order_price * fill_volume * multiplier);
            (*account)["market_value"] = fixed(std::max(0.0, record_decimal(*account, "market_value") - last_price * fill_volume * multiplier));
            (*account)["daily_pnl"] = fixed(record_decimal(*account, "daily_pnl") + realized_pnl);
        }
        Record* position = find_record(resources_["positions"], "symbol", order["symbol"]);
        if (position != nullptr) {
            const double multiplier = is_future ? 300.0 : 1.0;
            const int total_volume = record_integer(*position, "total_volume");
            const double market_value = last_price * total_volume * multiplier;
            const double pnl = (last_price - record_decimal(*position, "cost_price")) * total_volume * multiplier;
            (*position)["last_price"] = fixed(last_price);
            (*position)["market_value"] = fixed(market_value);
            (*position)["pnl"] = fixed(pnl);
            (*position)["pnl_percent"] = fixed(record_decimal(*position, "cost_price") == 0.0 ? 0.0 : (last_price / record_decimal(*position, "cost_price") - 1.0) * 100.0) + "%";
        }
        refresh_account_totals(order["account_id"]);
        Record trade{
            {"trade_id", "MT-" + padded(next_trade_id_++)}, {"order_id", order_id}, {"symbol", order["symbol"]},
            {"name", order["name"]}, {"side", order["side"]}, {"price", order["price"]}, {"volume", number(fill_volume)},
            {"trade_time", "2026-08-24 10:31:02"}, {"gateway", "MockTradeGateway"},
        };
        resources_["trades"].insert(resources_["trades"].begin(), trade);
        MockGatewayResult result;
        result.payload_json = "{\"order\":" + json_record(order) + ",\"trade\":" + json_record(trade) + ",\"message\":\"mock fill completed\"}";
        return result;
    }
    return failure("not_found", "mock order was not found");
}

void MockGateway::refresh_account_totals(const std::string& account_id) {
    Record* account = find_record(resources_["accounts"], "account_id", account_id);
    if (account == nullptr) {
        return;
    }
    (*account)["total_asset"] = fixed(
        record_decimal(*account, "available") + record_decimal(*account, "frozen") + record_decimal(*account, "market_value"));
    // 初始 Mock 账户包含未在演示页展开的组合资产；这里只按本次订单增量更新市值，再重算总资产，避免首笔交易把初始快照错误重置。
}

}  // namespace oxmbs::mock
