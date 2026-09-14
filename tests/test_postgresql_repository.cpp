#include "oxmbs/storage/qmt_repository.h"

#include <cassert>
#include <stdexcept>
#include <string>

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
    return 0;
}
