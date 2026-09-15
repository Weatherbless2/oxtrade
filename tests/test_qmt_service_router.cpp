#include "oxmbs/service/qmt_service_router.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace oxmbs;

class MemoryQmtRepository final : public service::IQmtRepository {
public:
    std::vector<storage::InstrumentRecord> stock;
    std::vector<storage::FuturesInstrumentRecord> futures;
    std::vector<storage::CalendarRecord> calendars;
    std::vector<storage::StockQuoteRecord> stock_quotes;
    std::vector<storage::FuturesQuoteRecord> futures_quotes;
    storage::RepositoryStatus failure_status{storage::RepositoryStatus::Ok};
    mutable storage::PageRequest last_stock_page{};

    bool list_stock_instruments(const std::string& market, const std::string& keyword,
                                const storage::PageRequest& page,
                                storage::PageResult<storage::InstrumentRecord>& result,
                                storage::RepositoryFailure& failure) const override {
        last_stock_page = page;
        if (failure_status != storage::RepositoryStatus::Ok) return fail(failure);
        std::vector<storage::InstrumentRecord> filtered;
        for (const auto& row : stock) {
            if (!market.empty() && row.market != market) continue;
            if (!keyword.empty() && row.stkcode.find(keyword) == std::string::npos &&
                row.stkname.find(keyword) == std::string::npos) continue;
            filtered.push_back(row);
        }
        return page_rows(filtered, page, result, failure);
    }

    bool list_futures_instruments(const std::string& market, const std::string& keyword,
                                  const storage::PageRequest& page,
                                  storage::PageResult<storage::FuturesInstrumentRecord>& result,
                                  storage::RepositoryFailure& failure) const override {
        if (failure_status != storage::RepositoryStatus::Ok) return fail(failure);
        std::vector<storage::FuturesInstrumentRecord> filtered;
        for (const auto& row : futures) {
            if (!market.empty() && row.market != market) continue;
            if (!keyword.empty() && row.stkcode.find(keyword) == std::string::npos) continue;
            filtered.push_back(row);
        }
        return page_rows(filtered, page, result, failure);
    }

    bool list_calendar(const std::string&, const std::string&, const std::string& market,
                       const storage::PageRequest& page,
                       storage::PageResult<storage::CalendarRecord>& result,
                       storage::RepositoryFailure& failure) const override {
        if (failure_status != storage::RepositoryStatus::Ok) return fail(failure);
        std::vector<storage::CalendarRecord> filtered;
        for (const auto& row : calendars) {
            if (!market.empty() && std::find(row.markets.begin(), row.markets.end(), market) == row.markets.end()) continue;
            filtered.push_back(row);
        }
        return page_rows(filtered, page, result, failure);
    }

    bool list_stock_quotes(const std::string& market, const std::string& stkcode,
                           const storage::PageRequest& page,
                           storage::PageResult<storage::StockQuoteRecord>& result,
                           storage::RepositoryFailure& failure) const override {
        if (failure_status != storage::RepositoryStatus::Ok) return fail(failure);
        std::vector<storage::StockQuoteRecord> filtered;
        for (const auto& row : stock_quotes) {
            if (!market.empty() && row.market != market) continue;
            if (!stkcode.empty() && row.stkcode != stkcode) continue;
            filtered.push_back(row);
        }
        return page_rows(filtered, page, result, failure);
    }

    bool list_futures_quotes(const std::string& market, const std::string& stkcode,
                             const storage::PageRequest& page,
                             storage::PageResult<storage::FuturesQuoteRecord>& result,
                             storage::RepositoryFailure& failure) const override {
        if (failure_status != storage::RepositoryStatus::Ok) return fail(failure);
        std::vector<storage::FuturesQuoteRecord> filtered;
        for (const auto& row : futures_quotes) {
            if (!market.empty() && row.market != market) continue;
            if (!stkcode.empty() && row.stkcode != stkcode) continue;
            filtered.push_back(row);
        }
        return page_rows(filtered, page, result, failure);
    }

    bool list_all_latest_quotes(const std::string& stkcode,
                                const storage::PageRequest& page,
                                storage::PageResult<storage::LatestQuoteRecord>& result,
                                storage::RepositoryFailure& failure) const override {
        if (failure_status != storage::RepositoryStatus::Ok) return fail(failure);
        std::vector<storage::LatestQuoteRecord> filtered;
        for (const auto& row : stock_quotes) {
            if (!stkcode.empty() && row.stkcode != stkcode) continue;
            filtered.push_back({false, row, {}});
        }
        for (const auto& row : futures_quotes) {
            if (!stkcode.empty() && row.stkcode != stkcode) continue;
            filtered.push_back({true, {}, row});
        }
        return page_rows(filtered, page, result, failure);
    }

private:
    template <typename Row>
    static bool page_rows(const std::vector<Row>& source, const storage::PageRequest& page,
                          storage::PageResult<Row>& result, storage::RepositoryFailure& failure) {
        result.total = source.size();
        const auto begin = std::min(page.offset, source.size());
        const auto end = std::min(source.size(), begin + page.limit);
        result.rows.assign(source.begin() + static_cast<std::ptrdiff_t>(begin),
                           source.begin() + static_cast<std::ptrdiff_t>(end));
        failure = {};
        return true;
    }

    bool fail(storage::RepositoryFailure& failure) const {
        failure = {failure_status, "memory repository failure", ""};
        return false;
    }
};

service::QmtServiceRouter make_router(MemoryQmtRepository& repository, std::size_t max_response_payload_bytes = 0) {
    service::QmtServiceRouterOptions options;
    options.routes = {
        {1, 10, service::QmtOperation::InstrumentSearch},
        {1, 11, service::QmtOperation::CalendarPage},
        {1, 12, service::QmtOperation::QuoteLatest},
        {1, 13, service::QmtOperation::BarPage},
    };
    options.max_page_size = 20;
    options.max_instrument_count = 4;
    if (max_response_payload_bytes != 0) options.max_response_payload_bytes = max_response_payload_bytes;
    return service::QmtServiceRouter(repository, std::move(options));
}

ipc::IpcTaskMsg task_for(std::uint32_t function_id, std::string payload, std::uint64_t session_id = 77) {
    ipc::IpcTaskMsg task;
    task.task_seq = 1001;
    task.session_id = session_id;
    task.deadline_unix_ms = 4'000'000'000'000ULL;
    task.msg_type = 1;
    task.func_id = function_id;
    task.payload.assign(payload.begin(), payload.end());
    return task;
}

std::string text(const service::QmtServiceResult& result) {
    return {result.payload.begin(), result.payload.end()};
}

void test_instrument_query_and_pagination() {
    MemoryQmtRepository repository;
    repository.stock = {
        {"0", "000001", "000001.SZ", "平安银行", "stock", "A", "active", "1991-04-03", "", ""},
        {"0", "000002", "000002.SZ", "万科A", "stock", "A", "active", "1991-01-29", "", ""},
    };
    auto router = make_router(repository);
    const auto result = router.execute(task_for(
        10, R"({"schema_version":1,"request_id":"req-1","data":{"market":"0","keyword":"000002"},"page":{"page_size":1,"page_token":"v1:0","sort":"stkcode"}})"));
    assert(result.ipc_status == ipc::IpcResponseStatus::Ok);
    assert(result.error_code == network::PackMessageErrorCode::Ok);
    assert(text(result).find("000002.SZ") != std::string::npos);
    assert(repository.last_stock_page.limit == 1 && repository.last_stock_page.offset == 0);

    const auto second_page = router.execute(task_for(
        10, R"({"schema_version":1,"request_id":"req-2","data":{"market":"0"},"page":{"page_size":1,"page_token":"v1:1","sort":"stkcode"}})"));
    assert(second_page.ipc_status == ipc::IpcResponseStatus::Ok);
    assert(repository.last_stock_page.offset == 1);
    assert(text(second_page).find("000002.SZ") != std::string::npos);
}

void test_empty_invalid_and_unavailable_results() {
    MemoryQmtRepository repository;
    auto router = make_router(repository);
    const auto empty = router.execute(task_for(
        10, R"({"schema_version":1,"request_id":"empty","data":{"market":"0"}})"));
    assert(empty.ipc_status == ipc::IpcResponseStatus::Rejected);
    assert(empty.error_code == network::PackMessageErrorCode::EmptyResult);

    const auto invalid = router.execute(task_for(10, "not-json"));
    assert(invalid.error_code == network::PackMessageErrorCode::InvalidArgument);
    assert(invalid.ipc_status == ipc::IpcResponseStatus::Rejected);

    const auto unavailable = router.execute(task_for(
        13, R"({"schema_version":1,"request_id":"bar","data":{}})"));
    assert(unavailable.error_code == network::PackMessageErrorCode::FunctionNotAvailable);
}

void test_auth_gate_repository_mapping_and_worker_handler() {
    MemoryQmtRepository repository;
    repository.stock = {{"0", "000001", "000001.SZ", "平安银行", "stock", "A", "active", "", "", ""}};
    auto router = make_router(repository);
    const std::string request = R"({"schema_version":1,"request_id":"quote","data":{"market":"0","stkcode":"000001"}})";

    const auto unauthenticated = router.execute(task_for(12, request, 0));
    assert(unauthenticated.error_code == network::PackMessageErrorCode::NotAuthenticated);

    repository.failure_status = storage::RepositoryStatus::ConnectionUnavailable;
    const auto unavailable = router.execute(task_for(10,
        R"({"schema_version":1,"request_id":"db","data":{"market":"0"}})"));
    assert(unavailable.error_code == network::PackMessageErrorCode::PgUnavailable);

    repository.failure_status = storage::RepositoryStatus::Timeout;
    const auto timeout = router.execute(task_for(10,
        R"({"schema_version":1,"request_id":"db-timeout","data":{"market":"0"}})"));
    assert(timeout.error_code == network::PackMessageErrorCode::PgTimeout);

    repository.failure_status = storage::RepositoryStatus::Ok;
    const auto worker_result = service::QmtServiceRouter::handle(
        task_for(10, R"({"schema_version":1,"request_id":"worker","data":{"market":"0"}})"), &router);
    assert(worker_result.status == ipc::IpcResponseStatus::Ok);
    assert(std::string(worker_result.payload.begin(), worker_result.payload.end()).find("000001.SZ") != std::string::npos);
}

void test_quote_full_market_pagination_and_capacity() {
    MemoryQmtRepository repository;
    repository.stock_quotes = {
        {"0", "000001", "2026-09-15T09:30:00", "2026-09-15T09:30:01", "10", "", "", "", "", "", "", "", "", "", "", "平安银行"},
        {"1", "600000", "2026-09-15T09:30:00", "2026-09-15T09:30:01", "8", "", "", "", "", "", "", "", "", "", "浦发银行"},
    };
    repository.futures_quotes = {
        {"F", "IF2609", "2026-09-15T09:30:00", "2026-09-15T09:30:01", "4000", "", "", "", "", "", "", "", "", "", "", "", "股指期货"},
    };
    auto router = make_router(repository);
    const auto first = router.execute(task_for(
        12, R"({"schema_version":1,"request_id":"all-1","data":{},"page":{"page_size":1,"sort":"receive_time","descending":true}})"));
    assert(first.ipc_status == ipc::IpcResponseStatus::Ok);
    const auto first_text = text(first);
    assert(first_text.find("\"total\":3") != std::string::npos);
    assert(first_text.find("\"page\":1") != std::string::npos);
    assert(first_text.find("\"page_size\":1") != std::string::npos);
    assert(first_text.find("000001") != std::string::npos);

    const auto second = router.execute(task_for(
        12, R"({"schema_version":1,"request_id":"all-2","data":{},"page":{"page_size":1,"page_token":"v1:1","sort":"receive_time","descending":true}})"));
    assert(second.ipc_status == ipc::IpcResponseStatus::Ok);
    const auto second_text = text(second);
    assert(second_text.find("\"page\":2") != std::string::npos);
    assert(second_text.find("600000") != std::string::npos);

    auto limited_router = make_router(repository, 100);
    const auto limited = limited_router.execute(task_for(
        12, R"({"schema_version":1,"request_id":"too-large","data":{},"page":{"page_size":20}})"));
    assert(limited.ipc_status == ipc::IpcResponseStatus::Rejected);
    assert(limited.error_code == network::PackMessageErrorCode::FrameTooLarge);
}

}  // namespace

int main() {
    test_instrument_query_and_pagination();
    test_empty_invalid_and_unavailable_results();
    test_auth_gate_repository_mapping_and_worker_handler();
    test_quote_full_market_pagination_and_capacity();
    return 0;
}
