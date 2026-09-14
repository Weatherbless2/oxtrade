#include "oxmbs/storage/qmt_repository.h"

#include <charconv>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <utility>

namespace oxmbs::storage {
namespace {

constexpr std::size_t kMaxPageSize = 1000;
constexpr std::size_t kMaxOffset = 10'000'000;

bool valid_market(std::string_view market) noexcept {
    if (market.empty()) {
        return true;
    }
    if (market.size() != 1) {
        return false;
    }
    return std::string_view("Y0129DFHZIG").find(market.front()) != std::string_view::npos;
}

bool valid_date(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
        return false;
    }
    for (std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U}) {
        if (!std::isdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

std::string direction(bool descending) {
    return descending ? " DESC" : " ASC";
}

bool order_for(const PageRequest& page,
               std::string_view default_expression,
               std::initializer_list<std::pair<std::string_view, std::string_view>> allowed,
               std::string& order,
               std::string& error) {
    const auto requested = page.sort_field.empty() ? default_expression : std::string_view(page.sort_field);
    std::string_view expression;
    for (const auto& [name, candidate] : allowed) {
        if (requested == name) {
            expression = candidate;
            break;
        }
    }
    if (expression.empty()) {
        error = "unsupported sort field: " + std::string(requested);
        return false;
    }
    order = " ORDER BY " + std::string(expression) + direction(page.descending) + " NULLS LAST";
    return true;
}

std::string total_as_string(const PostgresResult& rows, int row) {
    return rows.value(row, rows.column_count() - 1);
}

std::size_t parse_total(const PostgresResult& rows, int row) {
    const auto text = total_as_string(rows, row);
    std::size_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} ? result : 0;
}

std::vector<std::string> split_markets(const std::string& value) {
    std::vector<std::string> markets;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(',', begin);
        const auto part_end = end == std::string::npos ? value.size() : end;
        if (part_end > begin) {
            markets.emplace_back(value.substr(begin, part_end - begin));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return markets;
}

std::string text_param(std::size_t value) {
    return std::to_string(value);
}

}  // namespace

bool validate_page_request(const PageRequest& request, std::string& error) noexcept {
    if (request.limit == 0 || request.limit > kMaxPageSize) {
        error = "page limit must be between 1 and 1000";
        return false;
    }
    if (request.offset > kMaxOffset) {
        error = "page offset is too large";
        return false;
    }
    if (request.sort_field.size() > 64) {
        error = "sort field is too long";
        return false;
    }
    error.clear();
    return true;
}

const char* repository_status_name(RepositoryStatus status) noexcept {
    switch (status) {
    case RepositoryStatus::Ok: return "Ok";
    case RepositoryStatus::InvalidArgument: return "InvalidArgument";
    case RepositoryStatus::ConnectionUnavailable: return "ConnectionUnavailable";
    case RepositoryStatus::QueryFailed: return "QueryFailed";
    case RepositoryStatus::Timeout: return "Timeout";
    case RepositoryStatus::EmptyResult: return "EmptyResult";
    case RepositoryStatus::FunctionNotAvailable: return "FunctionNotAvailable";
    }
    return "Unknown";
}

RepositoryStatus QmtRepository::classify_failure(const PostgresFailure& failure) noexcept {
    switch (failure.status) {
    case PostgresStatus::InvalidArgument: return RepositoryStatus::InvalidArgument;
    case PostgresStatus::ConnectionUnavailable:
    case PostgresStatus::PoolClosed: return RepositoryStatus::ConnectionUnavailable;
    case PostgresStatus::Timeout: return RepositoryStatus::Timeout;
    case PostgresStatus::QueryFailed:
    case PostgresStatus::TransactionFailed: return RepositoryStatus::QueryFailed;
    case PostgresStatus::Ok: return RepositoryStatus::Ok;
    }
    return RepositoryStatus::QueryFailed;
}

void QmtRepository::copy_failure(const PostgresFailure& source,
                                 RepositoryFailure& target) noexcept {
    target.status = classify_failure(source);
    target.message = source.message;
    target.sqlstate = source.sqlstate;
}

bool QmtRepository::acquire(std::unique_ptr<PostgresConnectionPool::Lease>& lease,
                            RepositoryFailure& failure) const {
    if (pool_ == nullptr) {
        failure = {RepositoryStatus::ConnectionUnavailable, "PostgreSQL pool is not configured", {}};
        return false;
    }
    PostgresFailure pool_failure;
    lease = pool_->acquire(pool_failure);
    if (!lease) {
        copy_failure(pool_failure, failure);
        return false;
    }
    failure = {RepositoryStatus::Ok, {}, {}};
    return true;
}

bool QmtRepository::list_stock_instruments(const std::string& market,
                                           const std::string& keyword,
                                           const PageRequest& page,
                                           PageResult<InstrumentRecord>& result,
                                           RepositoryFailure& failure) const {
    result = {};
    std::string error;
    if (!validate_page_request(page, error) || !valid_market(market) || keyword.size() > 128) {
        failure = {RepositoryStatus::InvalidArgument,
                   error.empty() ? "invalid stock instrument filter" : error,
                   {}};
        return false;
    }
    std::string order;
    if (!order_for(page, "updated_at", {
                       {"market", "btrim(market)"},
                       {"stkcode", "stkcode"},
                       {"stkname", "stkname"},
                       {"listing_date", "listing_date"},
                       {"updated_at", "updated_at"},
                   }, order, error)) {
        failure = {RepositoryStatus::InvalidArgument, error, {}};
        return false;
    }

    std::unique_ptr<PostgresConnectionPool::Lease> lease;
    if (!acquire(lease, failure)) {
        return false;
    }
    const std::string sql =
        "SELECT btrim(market), stkcode, qmt_code, COALESCE(stkname, ''), instrument_type, "
        "COALESCE(qmt_type, ''), COALESCE(status, ''), COALESCE(listing_date::text, ''), "
        "COALESCE(expire_date::text, ''), updated_at::text, COUNT(*) OVER() "
        "FROM ta_data.stkinfo "
        "WHERE ($1 = '' OR btrim(market) = $1) "
        "AND ($2 = '' OR stkcode ILIKE '%' || $2 || '%' OR COALESCE(stkname, '') ILIKE '%' || $2 || '%')" +
        order + " , btrim(market), stkcode LIMIT $3::bigint OFFSET $4::bigint";
    const std::vector<std::optional<std::string>> parameters{
        market, keyword, text_param(page.limit), text_param(page.offset)};
    PostgresFailure pg_failure;
    auto rows = lease->connection().exec_prepared("oxapp_stkinfo_page", sql, parameters, pg_failure);
    if (!rows.ok()) {
        copy_failure(pg_failure, failure);
        return false;
    }
    result.total = rows.row_count() == 0 ? 0 : parse_total(rows, 0);
    result.rows.reserve(static_cast<std::size_t>(rows.row_count()));
    for (int row = 0; row < rows.row_count(); ++row) {
        result.rows.push_back({rows.value(row, 0), rows.value(row, 1), rows.value(row, 2),
                               rows.value(row, 3), rows.value(row, 4), rows.value(row, 5),
                               rows.value(row, 6), rows.value(row, 7), rows.value(row, 8),
                               rows.value(row, 9)});
    }
    failure = {RepositoryStatus::Ok, {}, {}};
    return true;
}

bool QmtRepository::list_futures_instruments(const std::string& market,
                                             const std::string& keyword,
                                             const PageRequest& page,
                                             PageResult<FuturesInstrumentRecord>& result,
                                             RepositoryFailure& failure) const {
    result = {};
    std::string error;
    if (!validate_page_request(page, error) || !valid_market(market) || keyword.size() > 128) {
        failure = {RepositoryStatus::InvalidArgument,
                   error.empty() ? "invalid futures instrument filter" : error,
                   {}};
        return false;
    }
    std::string order;
    if (!order_for(page, "updated_at", {
                       {"market", "btrim(market)"},
                       {"stkcode", "stkcode"},
                       {"product_id", "product_id"},
                       {"stkname", "stkname"},
                       {"expire_date", "expire_date"},
                       {"updated_at", "updated_at"},
                   }, order, error)) {
        failure = {RepositoryStatus::InvalidArgument, error, {}};
        return false;
    }
    std::unique_ptr<PostgresConnectionPool::Lease> lease;
    if (!acquire(lease, failure)) {
        return false;
    }
    const std::string sql =
        "SELECT btrim(market), stkcode, qmt_code, COALESCE(product_id, ''), COALESCE(stkname, ''), "
        "instrument_type, COALESCE(status, ''), COALESCE(listing_date::text, ''), "
        "COALESCE(expire_date::text, ''), COALESCE(contract_unit::text, ''), "
        "COALESCE(price_tick::text, ''), COALESCE(main_level, ''), updated_at::text, COUNT(*) OVER() "
        "FROM ta_data.ft_stkinfo "
        "WHERE ($1 = '' OR btrim(market) = $1) "
        "AND ($2 = '' OR stkcode ILIKE '%' || $2 || '%' OR COALESCE(stkname, '') ILIKE '%' || $2 || '%')" +
        order + " , btrim(market), stkcode LIMIT $3::bigint OFFSET $4::bigint";
    const std::vector<std::optional<std::string>> parameters{
        market, keyword, text_param(page.limit), text_param(page.offset)};
    PostgresFailure pg_failure;
    auto rows = lease->connection().exec_prepared("oxapp_ftinfo_page", sql, parameters, pg_failure);
    if (!rows.ok()) {
        copy_failure(pg_failure, failure);
        return false;
    }
    result.total = rows.row_count() == 0 ? 0 : parse_total(rows, 0);
    result.rows.reserve(static_cast<std::size_t>(rows.row_count()));
    for (int row = 0; row < rows.row_count(); ++row) {
        result.rows.push_back({rows.value(row, 0), rows.value(row, 1), rows.value(row, 2),
                               rows.value(row, 3), rows.value(row, 4), rows.value(row, 5),
                               rows.value(row, 6), rows.value(row, 7), rows.value(row, 8),
                               rows.value(row, 9), rows.value(row, 10), rows.value(row, 11),
                               rows.value(row, 12)});
    }
    failure = {RepositoryStatus::Ok, {}, {}};
    return true;
}

bool QmtRepository::list_calendar(const std::string& start_date,
                                   const std::string& end_date,
                                   const std::string& market,
                                   const PageRequest& page,
                                   PageResult<CalendarRecord>& result,
                                   RepositoryFailure& failure) const {
    result = {};
    std::string error;
    if (!validate_page_request(page, error) || !valid_date(start_date) || !valid_date(end_date) ||
        !valid_market(market)) {
        failure = {RepositoryStatus::InvalidArgument,
                   error.empty() ? "invalid calendar filter" : error,
                   {}};
        return false;
    }
    if (!start_date.empty() && !end_date.empty() && start_date > end_date) {
        failure = {RepositoryStatus::InvalidArgument, "calendar start_date is after end_date", {}};
        return false;
    }
    std::string order;
    if (!order_for(page, "phydate", {
                       {"phydate", "phydate"},
                       {"updated_at", "updated_at"},
                   }, order, error)) {
        failure = {RepositoryStatus::InvalidArgument, error, {}};
        return false;
    }
    std::unique_ptr<PostgresConnectionPool::Lease> lease;
    if (!acquire(lease, failure)) {
        return false;
    }
    const std::string sql =
        "SELECT phydate::text, array_to_string(markets, ','), source, updated_at::text, COUNT(*) OVER() "
        "FROM ta_data.trading_calendar "
        "WHERE ($1 = '' OR phydate >= NULLIF($1, '')::date) "
        "AND ($2 = '' OR phydate <= NULLIF($2, '')::date) "
        "AND ($3 = '' OR $3 = ANY(markets))" + order +
        " , phydate LIMIT $4::bigint OFFSET $5::bigint";
    const std::vector<std::optional<std::string>> parameters{
        start_date, end_date, market, text_param(page.limit), text_param(page.offset)};
    PostgresFailure pg_failure;
    auto rows = lease->connection().exec_prepared("oxapp_calendar_page", sql, parameters, pg_failure);
    if (!rows.ok()) {
        copy_failure(pg_failure, failure);
        return false;
    }
    result.total = rows.row_count() == 0 ? 0 : parse_total(rows, 0);
    result.rows.reserve(static_cast<std::size_t>(rows.row_count()));
    for (int row = 0; row < rows.row_count(); ++row) {
        result.rows.push_back({rows.value(row, 0), split_markets(rows.value(row, 1)),
                               rows.value(row, 2), rows.value(row, 3)});
    }
    failure = {RepositoryStatus::Ok, {}, {}};
    return true;
}

bool QmtRepository::list_stock_quotes(const std::string& market,
                                      const std::string& stkcode,
                                      const PageRequest& page,
                                      PageResult<StockQuoteRecord>& result,
                                      RepositoryFailure& failure) const {
    result = {};
    std::string error;
    if (!validate_page_request(page, error) || !valid_market(market) || stkcode.size() > 64) {
        failure = {RepositoryStatus::InvalidArgument,
                   error.empty() ? "invalid stock quote filter" : error,
                   {}};
        return false;
    }
    std::string order;
    if (!order_for(page, "receive_time", {
                       {"market", "btrim(p.market)"},
                       {"stkcode", "p.stkcode"},
                       {"quote_time", "p.quote_time"},
                       {"receive_time", "p.receive_time"},
                       {"last_price", "p.last_price"},
                   }, order, error)) {
        failure = {RepositoryStatus::InvalidArgument, error, {}};
        return false;
    }
    std::unique_ptr<PostgresConnectionPool::Lease> lease;
    if (!acquire(lease, failure)) {
        return false;
    }
    const std::string sql =
        "SELECT btrim(p.market), p.stkcode, p.quote_time::text, p.receive_time::text, "
        "COALESCE(p.last_price::text, ''), COALESCE(p.open_price::text, ''), "
        "COALESCE(p.high_price::text, ''), COALESCE(p.low_price::text, ''), "
        "COALESCE(p.pre_close::text, ''), COALESCE(p.volume::text, ''), COALESCE(p.amount::text, ''), "
        "COALESCE(p.bid_price_1::text, ''), COALESCE(p.bid_volume_1::text, ''), "
        "COALESCE(p.ask_price_1::text, ''), COALESCE(p.ask_volume_1::text, ''), "
        "COALESCE(i.stkname, ''), COUNT(*) OVER() "
        "FROM ta_data.stkprice p JOIN ta_data.stkinfo i USING (market, stkcode) "
        "WHERE ($1 = '' OR btrim(p.market) = $1) AND ($2 = '' OR p.stkcode ILIKE '%' || $2 || '%')" +
        order + " , btrim(p.market), p.stkcode LIMIT $3::bigint OFFSET $4::bigint";
    const std::vector<std::optional<std::string>> parameters{
        market, stkcode, text_param(page.limit), text_param(page.offset)};
    PostgresFailure pg_failure;
    auto rows = lease->connection().exec_prepared("oxapp_stkprice_page", sql, parameters, pg_failure);
    if (!rows.ok()) {
        copy_failure(pg_failure, failure);
        return false;
    }
    result.total = rows.row_count() == 0 ? 0 : parse_total(rows, 0);
    result.rows.reserve(static_cast<std::size_t>(rows.row_count()));
    for (int row = 0; row < rows.row_count(); ++row) {
        result.rows.push_back({rows.value(row, 0), rows.value(row, 1), rows.value(row, 2),
                               rows.value(row, 3), rows.value(row, 4), rows.value(row, 5),
                               rows.value(row, 6), rows.value(row, 7), rows.value(row, 8),
                               rows.value(row, 9), rows.value(row, 10), rows.value(row, 11),
                               rows.value(row, 12), rows.value(row, 13), rows.value(row, 14),
                               rows.value(row, 15)});
    }
    failure = {RepositoryStatus::Ok, {}, {}};
    return true;
}

bool QmtRepository::list_futures_quotes(const std::string& market,
                                        const std::string& stkcode,
                                        const PageRequest& page,
                                        PageResult<FuturesQuoteRecord>& result,
                                        RepositoryFailure& failure) const {
    result = {};
    std::string error;
    if (!validate_page_request(page, error) || !valid_market(market) || stkcode.size() > 64) {
        failure = {RepositoryStatus::InvalidArgument,
                   error.empty() ? "invalid futures quote filter" : error,
                   {}};
        return false;
    }
    std::string order;
    if (!order_for(page, "receive_time", {
                       {"market", "btrim(p.market)"},
                       {"stkcode", "p.stkcode"},
                       {"quote_time", "p.quote_time"},
                       {"receive_time", "p.receive_time"},
                       {"last_price", "p.last_price"},
                   }, order, error)) {
        failure = {RepositoryStatus::InvalidArgument, error, {}};
        return false;
    }
    std::unique_ptr<PostgresConnectionPool::Lease> lease;
    if (!acquire(lease, failure)) {
        return false;
    }
    const std::string sql =
        "SELECT btrim(p.market), p.stkcode, p.quote_time::text, p.receive_time::text, "
        "COALESCE(p.last_price::text, ''), COALESCE(p.open_price::text, ''), "
        "COALESCE(p.high_price::text, ''), COALESCE(p.low_price::text, ''), "
        "COALESCE(p.pre_close::text, ''), COALESCE(p.settlement::text, ''), "
        "COALESCE(p.volume::text, ''), COALESCE(p.amount::text, ''), "
        "COALESCE(p.open_interest::text, ''), COALESCE(p.bid_price_1::text, ''), "
        "COALESCE(p.bid_volume_1::text, ''), COALESCE(p.ask_price_1::text, ''), "
        "COALESCE(p.ask_volume_1::text, ''), COALESCE(i.stkname, ''), COUNT(*) OVER() "
        "FROM ta_data.ft_stkprice p JOIN ta_data.ft_stkinfo i USING (market, stkcode) "
        "WHERE ($1 = '' OR btrim(p.market) = $1) AND ($2 = '' OR p.stkcode ILIKE '%' || $2 || '%')" +
        order + " , btrim(p.market), p.stkcode LIMIT $3::bigint OFFSET $4::bigint";
    const std::vector<std::optional<std::string>> parameters{
        market, stkcode, text_param(page.limit), text_param(page.offset)};
    PostgresFailure pg_failure;
    auto rows = lease->connection().exec_prepared("oxapp_ftstkprice_page", sql, parameters, pg_failure);
    if (!rows.ok()) {
        copy_failure(pg_failure, failure);
        return false;
    }
    result.total = rows.row_count() == 0 ? 0 : parse_total(rows, 0);
    result.rows.reserve(static_cast<std::size_t>(rows.row_count()));
    for (int row = 0; row < rows.row_count(); ++row) {
        result.rows.push_back({rows.value(row, 0), rows.value(row, 1), rows.value(row, 2),
                               rows.value(row, 3), rows.value(row, 4), rows.value(row, 5),
                               rows.value(row, 6), rows.value(row, 7), rows.value(row, 8),
                               rows.value(row, 9), rows.value(row, 10), rows.value(row, 11),
                               rows.value(row, 12), rows.value(row, 13), rows.value(row, 14),
                               rows.value(row, 15), rows.value(row, 16), rows.value(row, 17)});
    }
    failure = {RepositoryStatus::Ok, {}, {}};
    return true;
}

bool QmtRepository::list_freshness(std::vector<FreshnessRecord>& result,
                                   RepositoryFailure& failure) const {
    result.clear();
    std::unique_ptr<PostgresConnectionPool::Lease> lease;
    if (!acquire(lease, failure)) {
        return false;
    }
    const std::string sql =
        "SELECT 'stkprice', COUNT(*)::text, COALESCE(MAX(quote_time)::text, ''), "
        "COALESCE(MAX(receive_time)::text, '') FROM ta_data.stkprice "
        "UNION ALL SELECT 'ft_stkprice', COUNT(*)::text, COALESCE(MAX(quote_time)::text, ''), "
        "COALESCE(MAX(receive_time)::text, '') FROM ta_data.ft_stkprice";
    PostgresFailure pg_failure;
    auto rows = lease->connection().exec_prepared("oxapp_quote_freshness", sql, {}, pg_failure);
    if (!rows.ok()) {
        copy_failure(pg_failure, failure);
        return false;
    }
    for (int row = 0; row < rows.row_count(); ++row) {
        std::size_t count = 0;
        const auto count_text = rows.value(row, 1);
        const auto parsed = std::from_chars(count_text.data(), count_text.data() + count_text.size(), count);
        if (parsed.ec != std::errc{}) {
            failure = {RepositoryStatus::QueryFailed, "invalid row count returned by PostgreSQL", {}};
            return false;
        }
        result.push_back({rows.value(row, 0), count, rows.value(row, 2), rows.value(row, 3)});
    }
    failure = {RepositoryStatus::Ok, {}, {}};
    return true;
}

}  // namespace oxmbs::storage
