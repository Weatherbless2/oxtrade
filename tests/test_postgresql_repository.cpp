#include "oxmbs/storage/qmt_repository.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {

void run_live_smoke(const char* conninfo) {
    oxmbs::storage::PostgresPoolOptions options;
    options.connection.conninfo = conninfo;
    options.min_size = 1;
    options.max_size = 2;
    options.acquire_timeout = std::chrono::milliseconds{500};
    oxmbs::storage::PostgresConnectionPool pool(options);
    oxmbs::storage::PostgresFailure pool_failure;
    if (!pool.open(pool_failure)) {
        std::cerr << "PostgreSQL live smoke failed: status="
                  << oxmbs::storage::postgres_status_name(pool_failure.status)
                  << " sqlstate=" << pool_failure.sqlstate << " message=" << pool_failure.message << '\n';
        assert(false);
    }
    assert(pool.total_size() == 1 && pool.idle_size() == 1);

    oxmbs::storage::QmtRepository repository(pool);
    oxmbs::storage::PageRequest page;
    page.limit = 5;
    oxmbs::storage::PageResult<oxmbs::storage::InstrumentRecord> instruments;
    oxmbs::storage::RepositoryFailure failure;
    assert(repository.list_stock_instruments("", "", page, instruments, failure));
    assert(instruments.total >= instruments.rows.size());
    assert(!instruments.rows.empty());

    oxmbs::storage::PageResult<oxmbs::storage::CalendarRecord> calendar;
    assert(repository.list_calendar("", "", "", page, calendar, failure));
    assert(!calendar.rows.empty());

    std::vector<oxmbs::storage::FreshnessRecord> freshness;
    assert(repository.list_freshness(freshness, failure));
    assert(freshness.size() == 2);
    pool.close();
}

}  // namespace

int main() {
    {
        oxmbs::storage::PageRequest page;
        page.limit = 0;
        std::string error;
        assert(!oxmbs::storage::validate_page_request(page, error));
        assert(error.find("limit") != std::string::npos);
    }

    {
        oxmbs::storage::PageRequest page;
        page.sort_field = "updated_at;drop";
        std::string error;
        // Repository 只接受上层明确登记的排序字段，排序方向由 bool 单独表达。
        assert(oxmbs::storage::validate_page_request(page, error));
        oxmbs::storage::PostgresConnectionPool pool;
        oxmbs::storage::QmtRepository repository(pool);
        oxmbs::storage::PageResult<oxmbs::storage::InstrumentRecord> result;
        oxmbs::storage::RepositoryFailure failure;
        assert(!repository.list_stock_instruments("0", "", page, result, failure));
        assert(failure.status == oxmbs::storage::RepositoryStatus::InvalidArgument);
    }

    {
        oxmbs::storage::PostgresPoolOptions options;
        options.max_size = 0;
        bool thrown = false;
        try {
            oxmbs::storage::PostgresConnectionPool pool(options);
        } catch (const std::invalid_argument&) {
            thrown = true;
        }
        assert(thrown);
    }

    {
        oxmbs::storage::PostgresConnectionPool pool;
        oxmbs::storage::QmtRepository repository(pool);
        oxmbs::storage::PageResult<oxmbs::storage::CalendarRecord> result;
        oxmbs::storage::PageResult<oxmbs::storage::InstrumentRecord> instrument_result;
        oxmbs::storage::RepositoryFailure failure;
        oxmbs::storage::PageRequest page;
        assert(!repository.list_calendar("2026-02-3", "", "", page, result, failure));
        assert(failure.status == oxmbs::storage::RepositoryStatus::InvalidArgument);

        assert(!repository.list_stock_instruments("0", "600000", page,
                                                   instrument_result, failure));
        assert(failure.status == oxmbs::storage::RepositoryStatus::ConnectionUnavailable);
    }

    if (const char* conninfo = std::getenv("OXAPP_PG_LIVE_URL"); conninfo != nullptr && *conninfo != '\0') {
        run_live_smoke(conninfo);  // 显式开启时只读验证真实 QMT/PG；默认测试不依赖外部数据库。
    }
    return 0;
}
