#pragma once

#include "oxmbs/storage/postgres_pool.h"

#include <cstddef>
#include <string>
#include <vector>

namespace oxmbs::storage {

struct PageRequest {
    std::size_t limit{100};
    std::size_t offset{0};
    std::string sort_field;
    bool descending{false};
};

bool validate_page_request(const PageRequest& request, std::string& error) noexcept;

template <typename Row>
struct PageResult {
    std::vector<Row> rows;
    std::size_t total{0};
};

struct InstrumentRecord {
    std::string market;
    std::string stkcode;
    std::string qmt_code;
    std::string stkname;
    std::string instrument_type;
    std::string qmt_type;
    std::string status;
    std::string listing_date;
    std::string expire_date;
    std::string updated_at;
};

struct FuturesInstrumentRecord {
    std::string market;
    std::string stkcode;
    std::string qmt_code;
    std::string product_id;
    std::string stkname;
    std::string instrument_type;
    std::string status;
    std::string listing_date;
    std::string expire_date;
    std::string contract_unit;
    std::string price_tick;
    std::string main_level;
    std::string updated_at;
};

struct CalendarRecord {
    std::string phydate;
    std::vector<std::string> markets;
    std::string source;
    std::string updated_at;
};

struct StockQuoteRecord {
    std::string market;
    std::string stkcode;
    std::string quote_time;
    std::string receive_time;
    std::string last_price;
    std::string open_price;
    std::string high_price;
    std::string low_price;
    std::string pre_close;
    std::string volume;
    std::string amount;
    std::string bid_price_1;
    std::string bid_volume_1;
    std::string ask_price_1;
    std::string ask_volume_1;
    std::string stkname;
};

struct FuturesQuoteRecord {
    std::string market;
    std::string stkcode;
    std::string quote_time;
    std::string receive_time;
    std::string last_price;
    std::string open_price;
    std::string high_price;
    std::string low_price;
    std::string pre_close;
    std::string settlement;
    std::string volume;
    std::string amount;
    std::string open_interest;
    std::string bid_price_1;
    std::string bid_volume_1;
    std::string ask_price_1;
    std::string ask_volume_1;
    std::string stkname;
};

struct LatestQuoteRecord {
    bool is_futures{false};
    StockQuoteRecord stock;
    FuturesQuoteRecord futures;
};

struct FreshnessRecord {
    std::string dataset;
    std::size_t rows{0};
    std::string latest_quote_time;
    std::string latest_receive_time;
};

enum class RepositoryStatus {
    Ok,
    InvalidArgument,
    ConnectionUnavailable,
    QueryFailed,
    Timeout,
    EmptyResult,
    FunctionNotAvailable,
};

const char* repository_status_name(RepositoryStatus status) noexcept;

struct RepositoryFailure {
    RepositoryStatus status{RepositoryStatus::QueryFailed};
    std::string message;
    std::string sqlstate;
};

class QmtRepository {
public:
    explicit QmtRepository(PostgresConnectionPool& pool) noexcept : pool_(&pool) {}

    bool list_stock_instruments(const std::string& market,
                                const std::string& keyword,
                                const PageRequest& page,
                                PageResult<InstrumentRecord>& result,
                                RepositoryFailure& failure) const;

    bool list_futures_instruments(const std::string& market,
                                  const std::string& keyword,
                                  const PageRequest& page,
                                  PageResult<FuturesInstrumentRecord>& result,
                                  RepositoryFailure& failure) const;

    bool list_calendar(const std::string& start_date,
                       const std::string& end_date,
                       const std::string& market,
                       const PageRequest& page,
                       PageResult<CalendarRecord>& result,
                       RepositoryFailure& failure) const;

    bool list_stock_quotes(const std::string& market,
                           const std::string& stkcode,
                           const PageRequest& page,
                           PageResult<StockQuoteRecord>& result,
                           RepositoryFailure& failure) const;

    bool list_futures_quotes(const std::string& market,
                             const std::string& stkcode,
                             const PageRequest& page,
                             PageResult<FuturesQuoteRecord>& result,
                             RepositoryFailure& failure) const;

    bool list_all_latest_quotes(const std::string& stkcode,
                                const PageRequest& page,
                                PageResult<LatestQuoteRecord>& result,
                                RepositoryFailure& failure) const;

    bool list_freshness(std::vector<FreshnessRecord>& result,
                        RepositoryFailure& failure) const;

    static RepositoryStatus classify_failure(const PostgresFailure& failure) noexcept;

private:
    bool acquire(std::unique_ptr<PostgresConnectionPool::Lease>& lease,
                 RepositoryFailure& failure) const;
    static void copy_failure(const PostgresFailure& source, RepositoryFailure& target) noexcept;

    PostgresConnectionPool* pool_{nullptr};
};

}  // namespace oxmbs::storage
