#include "oxmbs/service/qmt_service_router.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

namespace oxmbs::service {
namespace {

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Object, Array };
    Kind kind{Kind::Null};
    bool boolean{false};
    std::string scalar;
    std::map<std::string, JsonValue> object;
    std::vector<JsonValue> array;

    const JsonValue* find(std::string_view key) const noexcept {
        if (kind != Kind::Object) return nullptr;
        const auto it = object.find(std::string(key));
        return it == object.end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool parse(JsonValue& value, std::string& error) {
        skip_space();
        if (!parse_value(value, 0, error)) return false;
        skip_space();
        if (position_ != input_.size()) {
            error = "JSON has trailing characters";
            return false;
        }
        return true;
    }

private:
    bool parse_value(JsonValue& value, std::size_t depth, std::string& error) {
        if (depth > 32) {
            error = "JSON nesting exceeds 32 levels";
            return false;
        }
        skip_space();
        if (position_ >= input_.size()) {
            error = "JSON value is incomplete";
            return false;
        }
        switch (input_[position_]) {
        case '{': return parse_object(value, depth, error);
        case '[': return parse_array(value, depth, error);
        case '"':
            value = {};
            value.kind = JsonValue::Kind::String;
            return parse_string(value.scalar, error);
        case 't': return parse_literal("true", value, true, error);
        case 'f': return parse_literal("false", value, false, error);
        case 'n':
            if (!consume_literal("null")) {
                error = "invalid JSON literal";
                return false;
            }
            value = {};
            return true;
        default:
            if (input_[position_] == '-' || std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
                value = {};
                value.kind = JsonValue::Kind::Number;
                return parse_number(value.scalar, error);
            }
            error = "invalid JSON value";
            return false;
        }
    }

    bool parse_object(JsonValue& value, std::size_t depth, std::string& error) {
        value = {};
        value.kind = JsonValue::Kind::Object;
        ++position_;
        skip_space();
        if (consume('}')) return true;
        while (position_ < input_.size()) {
            if (input_[position_] != '"') {
                error = "JSON object key must be a string";
                return false;
            }
            std::string key;
            if (!parse_string(key, error)) return false;
            skip_space();
            if (!consume(':')) {
                error = "JSON object is missing ':'";
                return false;
            }
            JsonValue child;
            if (!parse_value(child, depth + 1, error)) return false;
            if (!value.object.emplace(std::move(key), std::move(child)).second) {
                error = "JSON object contains a duplicate key";
                return false;
            }
            skip_space();
            if (consume('}')) return true;
            if (!consume(',')) {
                error = "JSON object is missing ',' or '}'";
                return false;
            }
            skip_space();
        }
        error = "JSON object is incomplete";
        return false;
    }

    bool parse_array(JsonValue& value, std::size_t depth, std::string& error) {
        value = {};
        value.kind = JsonValue::Kind::Array;
        ++position_;
        skip_space();
        if (consume(']')) return true;
        while (position_ < input_.size()) {
            JsonValue child;
            if (!parse_value(child, depth + 1, error)) return false;
            value.array.push_back(std::move(child));
            skip_space();
            if (consume(']')) return true;
            if (!consume(',')) {
                error = "JSON array is missing ',' or ']'";
                return false;
            }
            skip_space();
        }
        error = "JSON array is incomplete";
        return false;
    }

    bool parse_string(std::string& output, std::string& error) {
        if (!consume('"')) {
            error = "JSON string must start with a quote";
            return false;
        }
        output.clear();
        while (position_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') return true;
            if (character < 0x20U) {
                error = "JSON string contains an unescaped control character";
                return false;
            }
            if (character != '\\') {
                output.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= input_.size()) {
                error = "JSON escape is incomplete";
                return false;
            }
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u':
                if (!append_unicode(output, error)) return false;
                break;
            default:
                error = "JSON string contains an unknown escape";
                return false;
            }
        }
        error = "JSON string is incomplete";
        return false;
    }

    bool append_unicode(std::string& output, std::string& error) {
        if (position_ + 4 > input_.size()) {
            error = "JSON unicode escape is incomplete";
            return false;
        }
        unsigned int codepoint = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const int digit = hex_digit(input_[position_++]);
            if (digit < 0) {
                error = "JSON unicode escape contains a non-hex digit";
                return false;
            }
            codepoint = (codepoint << 4U) | static_cast<unsigned int>(digit);
        }
        if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
            error = "JSON surrogate escapes are not supported";
            return false;
        }
        if (codepoint <= 0x7FU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
        return true;
    }

    bool parse_number(std::string& output, std::string& error) {
        const std::size_t begin = position_;
        if (consume('-') && position_ >= input_.size()) {
            error = "JSON number is incomplete";
            return false;
        }
        if (consume('0')) {
            if (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
                error = "JSON number has a leading zero";
                return false;
            }
        } else {
            if (position_ >= input_.size() || input_[position_] < '1' || input_[position_] > '9') {
                error = "JSON number has no integer digits";
                return false;
            }
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) ++position_;
        }
        if (consume('.')) {
            const std::size_t fraction_begin = position_;
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) ++position_;
            if (position_ == fraction_begin) {
                error = "JSON number has an empty fraction";
                return false;
            }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            const std::size_t exponent_begin = position_;
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) ++position_;
            if (position_ == exponent_begin) {
                error = "JSON number has an empty exponent";
                return false;
            }
        }
        output = std::string(input_.substr(begin, position_ - begin));
        return true;
    }

    bool parse_literal(std::string_view literal, JsonValue& value, bool boolean, std::string& error) {
        if (!consume_literal(literal)) {
            error = "invalid JSON literal";
            return false;
        }
        value = {};
        value.kind = JsonValue::Kind::Bool;
        value.boolean = boolean;
        return true;
    }

    bool consume_literal(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) return false;
        position_ += literal.size();
        return true;
    }
    bool consume(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }
    void skip_space() {
        while (position_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[position_])) != 0) ++position_;
    }
    static int hex_digit(char character) noexcept {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    }
    std::string_view input_;
    std::size_t position_{0};
};

bool unsigned_number(std::string_view text, std::uint64_t& output) {
    if (text.empty() || text.front() == '-') return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool required_string(const JsonValue& object, std::string_view key, std::string& output, std::string& error) {
    const auto* value = object.find(key);
    if (value == nullptr || value->kind != JsonValue::Kind::String || value->scalar.empty()) {
        error = "field '" + std::string(key) + "' must be a non-empty string";
        return false;
    }
    output = value->scalar;
    return true;
}

bool required_identifier(const JsonValue& object, std::string_view key, std::string& output, std::string& error) {
    const auto* value = object.find(key);
    if (value == nullptr || (value->kind != JsonValue::Kind::String && value->kind != JsonValue::Kind::Number)) {
        error = "field '" + std::string(key) + "' must be a string or unsigned integer";
        return false;
    }
    if (value->kind == JsonValue::Kind::String && value->scalar.empty()) {
        error = "field '" + std::string(key) + "' must not be empty";
        return false;
    }
    if (value->kind == JsonValue::Kind::Number) {
        std::uint64_t ignored = 0;
        if (!unsigned_number(value->scalar, ignored)) {
            error = "field '" + std::string(key) + "' must be an unsigned integer";
            return false;
        }
    }
    output = value->scalar;
    return true;
}

bool optional_string(const JsonValue& object, std::string_view key, std::string& output, std::string& error) {
    const auto* value = object.find(key);
    if (value == nullptr || value->kind == JsonValue::Kind::Null) {
        output.clear();
        return true;
    }
    if (value->kind != JsonValue::Kind::String) {
        error = "field '" + std::string(key) + "' must be a string or null";
        return false;
    }
    output = value->scalar;
    return true;
}

bool optional_bool(const JsonValue& object, std::string_view key, bool& output, std::string& error) {
    const auto* value = object.find(key);
    if (value == nullptr) return true;
    if (value->kind != JsonValue::Kind::Bool) {
        error = "field '" + std::string(key) + "' must be boolean";
        return false;
    }
    output = value->boolean;
    return true;
}

bool optional_unsigned(const JsonValue& object, std::string_view key, std::uint64_t& output, std::string& error) {
    const auto* value = object.find(key);
    if (value == nullptr) return true;
    if (value->kind != JsonValue::Kind::Number || !unsigned_number(value->scalar, output)) {
        error = "field '" + std::string(key) + "' must be an unsigned integer";
        return false;
    }
    return true;
}

bool parse_page(const JsonValue& root, const QmtServiceRouterOptions& options,
                storage::PageRequest& page, std::string& error) {
    const auto* page_value = root.find("page");
    if (page_value != nullptr && page_value->kind != JsonValue::Kind::Object) {
        error = "field 'page' must be an object";
        return false;
    }
    page = {};
    page.limit = std::min<std::size_t>(50, options.max_page_size);  // 未携带 page 时仍使用稳定默认值，但不能超过服务端门禁。
    const JsonValue empty_object;
    const auto& object = page_value == nullptr ? empty_object : *page_value;
    std::uint64_t page_size = page.limit;
    if (!optional_unsigned(object, "page_size", page_size, error)) return false;
    if (page_size == 0 || page_size > options.max_page_size || page_size > 200) {
        error = "page.page_size must be between 1 and " + std::to_string(std::min<std::size_t>(options.max_page_size, 200));
        return false;
    }
    page.limit = static_cast<std::size_t>(page_size);
    std::string token;
    if (!optional_string(object, "page_token", token, error)) return false;
    if (!token.empty()) {
        std::uint64_t offset = 0;
        if (token.rfind("v1:", 0) != 0 || !unsigned_number(token.substr(3), offset) || offset > 10'000'000U) {
            error = "page.page_token is not a valid v1 cursor";
            return false;
        }
        page.offset = static_cast<std::size_t>(offset);
    }
    return optional_string(object, "sort", page.sort_field, error) &&
           optional_bool(object, "descending", page.descending, error);
}

std::string json_escape(std::string_view value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20U) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned int>(character) << std::dec;
            } else output << static_cast<char>(character);
        }
    }
    return output.str();
}

std::string json_string(std::string_view value) { return "\"" + json_escape(value) + "\""; }

bool json_number(std::string_view value) {
    if (value.empty()) return false;
    char* end = nullptr;
    const std::string copy(value);
    std::strtod(copy.c_str(), &end);
    return end == copy.c_str() + copy.size();
}

std::string json_scalar(std::string_view value) {
    if (value.empty()) return "null";
    return json_number(value) ? std::string(value) : json_string(value);
}

QmtServiceResult result_with_payload(ipc::IpcResponseStatus status,
                                     network::PackMessageErrorCode code,
                                     std::string message,
                                     std::string data) {
    QmtServiceResult result;
    result.ipc_status = status;
    result.error_code = code;
    result.message = std::move(message);
    const std::string document = "{\"schema_version\":1,\"data\":" + data +
                                 ",\"error_code\":" + std::to_string(static_cast<std::uint32_t>(code)) +
                                 ",\"message\":" + json_string(result.message) + ",\"warnings\":[]}";
    result.payload.assign(document.begin(), document.end());
    return result;
}

QmtServiceResult success_result(std::string data) {
    return result_with_payload(ipc::IpcResponseStatus::Ok, network::PackMessageErrorCode::Ok, {}, std::move(data));
}

QmtServiceResult invalid_result(std::string message) {
    return result_with_payload(ipc::IpcResponseStatus::Rejected, network::PackMessageErrorCode::InvalidArgument,
                               std::move(message), "{}");
}

QmtServiceResult unavailable_result(std::string message) {
    return result_with_payload(ipc::IpcResponseStatus::Rejected, network::PackMessageErrorCode::FunctionNotAvailable,
                               std::move(message), "{}");
}

QmtServiceResult empty_result(std::string data) {
    return result_with_payload(ipc::IpcResponseStatus::Rejected, network::PackMessageErrorCode::EmptyResult,
                               "query returned no rows", std::move(data));
}

QmtServiceResult repository_result(const storage::RepositoryFailure& failure) {
    auto code = network::PackMessageErrorCode::InternalError;
    auto status = ipc::IpcResponseStatus::Failed;
    switch (failure.status) {
    case storage::RepositoryStatus::InvalidArgument:
        code = network::PackMessageErrorCode::InvalidArgument;
        status = ipc::IpcResponseStatus::Rejected;
        break;
    case storage::RepositoryStatus::ConnectionUnavailable: code = network::PackMessageErrorCode::PgUnavailable; break;
    case storage::RepositoryStatus::Timeout: code = network::PackMessageErrorCode::PgTimeout; break;
    case storage::RepositoryStatus::EmptyResult:
        code = network::PackMessageErrorCode::EmptyResult;
        status = ipc::IpcResponseStatus::Rejected;
        break;
    default: break;
    }
    const std::string public_message = failure.status == storage::RepositoryStatus::InvalidArgument
                                           ? (failure.message.empty() ? "invalid repository argument" : failure.message)
                                           : (failure.status == storage::RepositoryStatus::ConnectionUnavailable
                                                  ? "PostgreSQL is unavailable"
                                                  : (failure.status == storage::RepositoryStatus::Timeout
                                                         ? "PostgreSQL query timed out"
                                                         : "PostgreSQL query failed"));
    return result_with_payload(status, code, public_message, "{}");
}

std::string page_data(std::string items, std::size_t total, const storage::PageRequest& page) {
    const bool has_more = page.offset <= std::numeric_limits<std::size_t>::max() - page.limit && page.offset + page.limit < total;
    const auto next_offset = page.offset + page.limit;
    const std::string token = has_more ? json_string("v1:" + std::to_string(next_offset)) : "null";
    const auto page_number = page.limit == 0 ? 1U : (page.offset / page.limit) + 1U;
    return "{\"items\":" + std::move(items) + ",\"total\":" + std::to_string(total) +
           ",\"page\":" + std::to_string(page_number) + ",\"page_size\":" + std::to_string(page.limit) +
           ",\"has_more\":" + (has_more ? "true" : "false") + ",\"next_page_token\":" + token + "}";
}

std::string instrument_json(const storage::InstrumentRecord& row) {
    const bool tradable = row.status == "active" || row.status == "trading" || row.status == "1";
    return "{\"instrument_id\":" + json_string(row.market + ":" + row.stkcode) +
           ",\"market\":" + json_string(row.market) + ",\"stkcode\":" + json_string(row.stkcode) +
           ",\"qmt_code\":" + json_string(row.qmt_code) + ",\"name\":" + json_string(row.stkname) +
           ",\"instrument_type\":" + json_string(row.instrument_type) + ",\"qmt_type\":" + json_string(row.qmt_type) +
           ",\"status\":" + json_string(row.status) + ",\"listing_date\":" + json_string(row.listing_date) +
           ",\"expire_date\":" + json_string(row.expire_date) + ",\"source\":\"QMT/PG\",\"is_tradable\":" +
           (tradable ? "true" : "false") + "}";
}

std::string futures_instrument_json(const storage::FuturesInstrumentRecord& row) {
    return "{\"instrument_id\":" + json_string(row.market + ":" + row.stkcode) +
           ",\"market\":" + json_string(row.market) + ",\"stkcode\":" + json_string(row.stkcode) +
           ",\"qmt_code\":" + json_string(row.qmt_code) + ",\"product_id\":" + json_string(row.product_id) +
           ",\"name\":" + json_string(row.stkname) + ",\"instrument_type\":" + json_string(row.instrument_type) +
           ",\"status\":" + json_string(row.status) + ",\"listing_date\":" + json_string(row.listing_date) +
           ",\"expire_date\":" + json_string(row.expire_date) + ",\"contract_unit\":" + json_scalar(row.contract_unit) +
           ",\"price_tick\":" + json_scalar(row.price_tick) + ",\"main_level\":" + json_string(row.main_level) +
           ",\"source\":\"QMT/PG\"}";
}

std::string calendar_json(const storage::CalendarRecord& row) {
    std::string markets = "[";
    for (std::size_t index = 0; index < row.markets.size(); ++index) {
        if (index != 0) markets += ',';
        markets += json_string(row.markets[index]);
    }
    markets += ']';
    const bool y = std::find(row.markets.begin(), row.markets.end(), "Y") != row.markets.end();
    const bool zero = std::find(row.markets.begin(), row.markets.end(), "0") != row.markets.end();
    const bool one = std::find(row.markets.begin(), row.markets.end(), "1") != row.markets.end();
    const bool two = std::find(row.markets.begin(), row.markets.end(), "2") != row.markets.end();
    return "{\"phydate\":" + json_string(row.phydate) + ",\"markets\":" + markets +
           ",\"is_y_derived\":" + (y && zero && one && two ? "true" : "false") +
           ",\"source\":" + json_string(row.source) + ",\"updated_at\":" + json_string(row.updated_at) + "}";
}

std::string stock_quote_json(const storage::StockQuoteRecord& row) {
    return "{\"instrument_id\":" + json_string(row.market + ":" + row.stkcode) +
           ",\"market\":" + json_string(row.market) + ",\"stkcode\":" + json_string(row.stkcode) +
           ",\"name\":" + json_string(row.stkname) + ",\"last_price\":" + json_scalar(row.last_price) +
           ",\"asset_class\":\"stock\"" +
           ",\"open_price\":" + json_scalar(row.open_price) + ",\"high_price\":" + json_scalar(row.high_price) +
           ",\"low_price\":" + json_scalar(row.low_price) + ",\"pre_close\":" + json_scalar(row.pre_close) +
           ",\"volume\":" + json_scalar(row.volume) + ",\"amount\":" + json_scalar(row.amount) +
           ",\"bid_price_1\":" + json_scalar(row.bid_price_1) + ",\"bid_volume_1\":" + json_scalar(row.bid_volume_1) +
           ",\"ask_price_1\":" + json_scalar(row.ask_price_1) + ",\"ask_volume_1\":" + json_scalar(row.ask_volume_1) +
           ",\"quote_time\":" + json_string(row.quote_time) + ",\"receive_time\":" + json_string(row.receive_time) +
           ",\"freshness_status\":\"UNASSESSED\",\"source\":\"QMT/PG\"}";
}

std::string futures_quote_json(const storage::FuturesQuoteRecord& row) {
    return "{\"instrument_id\":" + json_string(row.market + ":" + row.stkcode) +
           ",\"market\":" + json_string(row.market) + ",\"stkcode\":" + json_string(row.stkcode) +
           ",\"name\":" + json_string(row.stkname) + ",\"last_price\":" + json_scalar(row.last_price) +
           ",\"asset_class\":\"futures\"" +
           ",\"open_price\":" + json_scalar(row.open_price) + ",\"high_price\":" + json_scalar(row.high_price) +
           ",\"low_price\":" + json_scalar(row.low_price) + ",\"pre_close\":" + json_scalar(row.pre_close) +
           ",\"settlement\":" + json_scalar(row.settlement) + ",\"volume\":" + json_scalar(row.volume) +
           ",\"amount\":" + json_scalar(row.amount) + ",\"open_interest\":" + json_scalar(row.open_interest) +
           ",\"bid_price_1\":" + json_scalar(row.bid_price_1) + ",\"bid_volume_1\":" + json_scalar(row.bid_volume_1) +
           ",\"ask_price_1\":" + json_scalar(row.ask_price_1) + ",\"ask_volume_1\":" + json_scalar(row.ask_volume_1) +
           ",\"quote_time\":" + json_string(row.quote_time) + ",\"receive_time\":" + json_string(row.receive_time) +
           ",\"freshness_status\":\"UNASSESSED\",\"source\":\"QMT/PG\"}";
}

bool futures_market(std::string_view market) noexcept {
    return std::string_view("DFHZIG").find(market) != std::string_view::npos;
}

}  // namespace

const char* qmt_operation_name(QmtOperation operation) noexcept {
    switch (operation) {
    case QmtOperation::InstrumentSearch: return "instrument.search";
    case QmtOperation::CalendarPage: return "calendar.page";
    case QmtOperation::QuoteLatest: return "quote.latest";
    case QmtOperation::BarPage: return "bar.page";
    case QmtOperation::TickPage: return "tick.page";
    case QmtOperation::DepthLatest: return "depth.latest";
    }
    return "unknown";
}

QmtServiceRouter::QmtServiceRouter(IQmtRepository& repository, QmtServiceRouterOptions options)
    : repository_(&repository), options_(std::move(options)) {
}

QmtServiceResult QmtServiceRouter::execute(const ipc::IpcTaskMsg& task) const {
    const auto frame_limit = ipc::TaskProtocolLimits{}.max_frame_bytes;
    if (repository_ == nullptr || options_.max_page_size == 0 || options_.max_page_size > 200 ||
        options_.max_instrument_count == 0 || options_.max_response_payload_bytes == 0 ||
        options_.max_response_payload_bytes > frame_limit - ipc::IpcWireLayout::kHeaderBytes) {
        return result_with_payload(ipc::IpcResponseStatus::Failed, network::PackMessageErrorCode::InternalError,
                                   "QMT service router options are invalid", "{}");
    }
    if (task.task_seq == 0 || (options_.require_authenticated_session && task.session_id == 0)) {
        return result_with_payload(ipc::IpcResponseStatus::Rejected, network::PackMessageErrorCode::NotAuthenticated,
                                   "authenticated session is required", "{}");
    }
    const auto* route = resolve(task.msg_type, task.func_id);
    if (route == nullptr) return invalid_result("no QMT service route for message type/function");
    const auto* bytes = task.payload.empty() ? reinterpret_cast<const std::uint8_t*>("") : task.payload.data();
    auto result = execute_operation(route->operation, task, std::string_view(reinterpret_cast<const char*>(bytes), task.payload.size()));
    if (result.payload.size() > options_.max_response_payload_bytes) {
        // 响应在 Service 层截断前被拒绝，调用方可以据此改小 page_size，而不会收到不可编码的半包。
        return result_with_payload(ipc::IpcResponseStatus::Rejected, network::PackMessageErrorCode::FrameTooLarge,
                                   "response payload exceeds configured limit; reduce page_size", "{}");
    }
    return result;
}

worker::QueryExecutionResult QmtServiceRouter::handle(const ipc::IpcTaskMsg& task, void* context) {
    worker::QueryExecutionResult output;
    if (context == nullptr) {
        output.status = ipc::IpcResponseStatus::Failed;
        output.payload = result_with_payload(ipc::IpcResponseStatus::Failed, network::PackMessageErrorCode::InternalError,
                                             "QMT service router context is null", "{}").payload;
        return output;
    }
    auto result = static_cast<QmtServiceRouter*>(context)->execute(task);
    output.status = result.ipc_status;
    output.payload = std::move(result.payload);
    return output;
}

const QmtRouteBinding* QmtServiceRouter::resolve(std::uint32_t message_type, std::uint32_t function_id) const noexcept {
    for (const auto& route : options_.routes) {
        if (route.message_type == message_type && route.function_id == function_id) return &route;
    }
    return nullptr;
}

QmtServiceResult QmtServiceRouter::execute_operation(QmtOperation operation, const ipc::IpcTaskMsg& task,
                                                     std::string_view payload) const {
    (void)task;
    JsonValue root;
    std::string error;
    JsonParser parser(payload);
    if (!parser.parse(root, error) || root.kind != JsonValue::Kind::Object) {
        return invalid_result(error.empty() ? "request payload must be a JSON object" : error);
    }
    const auto* schema = root.find("schema_version");
    std::uint64_t schema_version = 0;
    if (schema == nullptr || schema->kind != JsonValue::Kind::Number || !unsigned_number(schema->scalar, schema_version) || schema_version != 1) {
        return invalid_result("schema_version must be 1");
    }
    std::string request_id;
    if (!required_identifier(root, "request_id", request_id, error)) return invalid_result(error);
    (void)request_id;  // 网络层已用 request_id 关联；Worker 只校验其存在，不以它替代 task_seq。
    const auto* data_value = root.find("data");
    if (data_value == nullptr || data_value->kind != JsonValue::Kind::Object) return invalid_result("data must be a JSON object");
    const auto& data = *data_value;
    if (operation == QmtOperation::BarPage || operation == QmtOperation::TickPage || operation == QmtOperation::DepthLatest) {
        return unavailable_result(std::string(qmt_operation_name(operation)) + " is not available in current PG schema");
    }
    storage::PageRequest page;
    if (!parse_page(root, options_, page, error)) return invalid_result(error);

    if (operation == QmtOperation::InstrumentSearch) {
        std::string market, keyword, asset_class, instrument_type, status;
        if (!optional_string(data, "market", market, error) || !optional_string(data, "keyword", keyword, error) ||
            !optional_string(data, "asset_class", asset_class, error) || !optional_string(data, "instrument_type", instrument_type, error) ||
            !optional_string(data, "status", status, error)) return invalid_result(error);
        if (!instrument_type.empty() || !status.empty()) return invalid_result("instrument_type/status filters are not in the audited Repository query contract");
        if (asset_class != "" && asset_class != "stock" && asset_class != "futures") return invalid_result("asset_class must be stock or futures");
        if (asset_class == "futures" || futures_market(market)) {
            storage::PageResult<storage::FuturesInstrumentRecord> result;
            storage::RepositoryFailure failure;
            if (!repository_->list_futures_instruments(market, keyword, page, result, failure)) return repository_result(failure);
            std::string items = "[";
            for (std::size_t index = 0; index < result.rows.size(); ++index) { if (index != 0) items += ','; items += futures_instrument_json(result.rows[index]); }
            items += ']';
            return result.rows.empty() ? empty_result(page_data(items, result.total, page)) : success_result(page_data(items, result.total, page));
        }
        std::vector<storage::InstrumentRecord> rows;
        std::size_t total = 0;
        const auto markets = market == "Y" ? std::vector<std::string>{"0", "1", "2"} : std::vector<std::string>{market};
        for (const auto& selected_market : markets) {
            storage::PageResult<storage::InstrumentRecord> result;
            storage::RepositoryFailure failure;
            if (!repository_->list_stock_instruments(selected_market, keyword, page, result, failure)) return repository_result(failure);
            total += result.total;
            rows.insert(rows.end(), result.rows.begin(), result.rows.end());
        }
        std::string items = "[";
        for (std::size_t index = 0; index < rows.size(); ++index) { if (index != 0) items += ','; items += instrument_json(rows[index]); }
        items += ']';
        return rows.empty() ? empty_result(page_data(items, total, page)) : success_result(page_data(items, total, page));
    }

    if (operation == QmtOperation::CalendarPage) {
        std::string start_date, end_date, market;
        bool include_detail = false;
        if (!optional_string(data, "start_date", start_date, error) || !optional_string(data, "end_date", end_date, error) ||
            !optional_string(data, "market", market, error) || !optional_bool(data, "include_detail", include_detail, error)) return invalid_result(error);
        if (include_detail) return invalid_result("include_detail requires a detail Repository method and is not enabled yet");
        storage::PageResult<storage::CalendarRecord> result;
        storage::RepositoryFailure failure;
        if (!repository_->list_calendar(start_date, end_date, market, page, result, failure)) return repository_result(failure);
        std::string items = "[";
        for (std::size_t index = 0; index < result.rows.size(); ++index) { if (index != 0) items += ','; items += calendar_json(result.rows[index]); }
        items += ']';
        return result.rows.empty() ? empty_result(page_data(items, result.total, page)) : success_result(page_data(items, result.total, page));
    }

    if (operation == QmtOperation::QuoteLatest) {
        std::uint64_t freshness_max_age_ms = 300'000;
        if (!optional_unsigned(data, "freshness_max_age_ms", freshness_max_age_ms, error) || freshness_max_age_ms > 86'400'000U) return invalid_result("freshness_max_age_ms must be an unsigned integer no greater than one day");
        const auto* instruments = data.find("instruments");
        std::vector<std::pair<std::string, std::string>> selections;
        if (instruments != nullptr) {
            if (instruments->kind != JsonValue::Kind::Array || instruments->array.size() > options_.max_instrument_count) return invalid_result("instruments must be an array within the configured limit");
            for (const auto& item : instruments->array) {
                if (item.kind != JsonValue::Kind::Object) return invalid_result("each instruments item must be an object");
                std::string market, stkcode;
                if (!required_string(item, "market", market, error) || !required_string(item, "stkcode", stkcode, error)) return invalid_result(error);
                selections.emplace_back(std::move(market), std::move(stkcode));
            }
        } else {
            std::string market, stkcode;
            if (!optional_string(data, "market", market, error) || !optional_string(data, "stkcode", stkcode, error)) return invalid_result(error);
            if (market == "Y") return invalid_result("quote.market cannot be the Y collection; choose a security market");
            selections.emplace_back(std::move(market), std::move(stkcode));
        }
        (void)freshness_max_age_ms;  // PG 返回原始时间；MarketHub 在 Push 路径按交易时段计算最终新鲜度。
        std::string items = "[";
        std::size_t total = 0;
        bool first = true;
        for (const auto& [market, stkcode] : selections) {
            storage::PageRequest quote_page = page;
            // 指定 instruments 时每个标的最多返回一条最新快照；全市场请求则完整使用 page/page_token。
            if (instruments != nullptr) {
                quote_page.limit = 1;
                quote_page.offset = 0;
            }
            if (market.empty()) {
                // 空市场由 Repository 在 PostgreSQL 中 UNION ALL 两张核心行情表后再分页，
                // 保证 page_token 是整个 QMT 数据集的游标，不会因分别 OFFSET 两张表而漏行或重复。
                storage::PageResult<storage::LatestQuoteRecord> result;
                storage::RepositoryFailure failure;
                if (!repository_->list_all_latest_quotes(stkcode, quote_page, result, failure)) return repository_result(failure);
                total = result.total;
                for (const auto& row : result.rows) {
                    if (!first) items += ',';
                    first = false;
                    items += row.is_futures ? futures_quote_json(row.futures) : stock_quote_json(row.stock);
                }
            } else if (futures_market(market)) {
                storage::PageResult<storage::FuturesQuoteRecord> result;
                storage::RepositoryFailure failure;
                if (!repository_->list_futures_quotes(market, stkcode, quote_page, result, failure)) return repository_result(failure);
                total += result.total;
                for (const auto& row : result.rows) { if (!first) items += ','; first = false; items += futures_quote_json(row); }
            } else {
                storage::PageResult<storage::StockQuoteRecord> result;
                storage::RepositoryFailure failure;
                if (!repository_->list_stock_quotes(market, stkcode, quote_page, result, failure)) return repository_result(failure);
                total += result.total;
                for (const auto& row : result.rows) { if (!first) items += ','; first = false; items += stock_quote_json(row); }
            }
        }
        items += ']';
        return total == 0 ? empty_result(page_data(items, 0, page)) : success_result(page_data(items, total, page));
    }
    return invalid_result("unsupported QMT operation");
}

}  // namespace oxmbs::service
