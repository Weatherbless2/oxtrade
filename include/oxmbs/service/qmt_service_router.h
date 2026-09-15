#pragma once

#include "oxmbs/network/pack_message.h"
#include "oxmbs/storage/qmt_repository.h"
#include "oxmbs/worker/query_worker_runtime.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oxmbs::service {

// Service 编号由正式 PackMessage/配置注入；枚举只表示已经冻结的业务能力。
enum class QmtOperation {
    InstrumentSearch,
    CalendarPage,
    QuoteLatest,
    BarPage,
    TickPage,
    DepthLatest,
};

const char* qmt_operation_name(QmtOperation operation) noexcept;

struct QmtRouteBinding {
    std::uint32_t message_type{0};
    std::uint32_t function_id{0};
    QmtOperation operation{QmtOperation::InstrumentSearch};
};

// Service 只依赖这个只读端口，生产使用 QmtRepository，测试可使用内存替身。
class IQmtRepository {
public:
    virtual ~IQmtRepository() = default;

    virtual bool list_stock_instruments(const std::string& market,
                                        const std::string& keyword,
                                        const storage::PageRequest& page,
                                        storage::PageResult<storage::InstrumentRecord>& result,
                                        storage::RepositoryFailure& failure) const = 0;
    virtual bool list_futures_instruments(const std::string& market,
                                          const std::string& keyword,
                                          const storage::PageRequest& page,
                                          storage::PageResult<storage::FuturesInstrumentRecord>& result,
                                          storage::RepositoryFailure& failure) const = 0;
    virtual bool list_calendar(const std::string& start_date,
                               const std::string& end_date,
                               const std::string& market,
                               const storage::PageRequest& page,
                               storage::PageResult<storage::CalendarRecord>& result,
                               storage::RepositoryFailure& failure) const = 0;
    virtual bool list_stock_quotes(const std::string& market,
                                   const std::string& stkcode,
                                   const storage::PageRequest& page,
                                   storage::PageResult<storage::StockQuoteRecord>& result,
                                   storage::RepositoryFailure& failure) const = 0;
    virtual bool list_futures_quotes(const std::string& market,
                                     const std::string& stkcode,
                                     const storage::PageRequest& page,
                                     storage::PageResult<storage::FuturesQuoteRecord>& result,
                                     storage::RepositoryFailure& failure) const = 0;
    virtual bool list_all_latest_quotes(const std::string& stkcode,
                                        const storage::PageRequest& page,
                                        storage::PageResult<storage::LatestQuoteRecord>& result,
                                        storage::RepositoryFailure& failure) const = 0;
};

// 生产适配器只转发到 OX-P0-06 的 QmtRepository，不复制 SQL 或连接池所有权。
class QmtRepositoryAdapter final : public IQmtRepository {
public:
    explicit QmtRepositoryAdapter(storage::QmtRepository& repository) noexcept : repository_(&repository) {}
    bool list_stock_instruments(const std::string&, const std::string&, const storage::PageRequest&,
                                storage::PageResult<storage::InstrumentRecord>&, storage::RepositoryFailure&) const override;
    bool list_futures_instruments(const std::string&, const std::string&, const storage::PageRequest&,
                                  storage::PageResult<storage::FuturesInstrumentRecord>&, storage::RepositoryFailure&) const override;
    bool list_calendar(const std::string&, const std::string&, const std::string&, const storage::PageRequest&,
                       storage::PageResult<storage::CalendarRecord>&, storage::RepositoryFailure&) const override;
    bool list_stock_quotes(const std::string&, const std::string&, const storage::PageRequest&,
                           storage::PageResult<storage::StockQuoteRecord>&, storage::RepositoryFailure&) const override;
    bool list_futures_quotes(const std::string&, const std::string&, const storage::PageRequest&,
                             storage::PageResult<storage::FuturesQuoteRecord>&, storage::RepositoryFailure&) const override;
    bool list_all_latest_quotes(const std::string&, const storage::PageRequest&,
                                storage::PageResult<storage::LatestQuoteRecord>&, storage::RepositoryFailure&) const override;

private:
    storage::QmtRepository* repository_{nullptr};
};

struct QmtServiceRouterOptions {
    std::vector<QmtRouteBinding> routes;
    std::size_t max_page_size{200};
    std::size_t max_instrument_count{200};
    std::size_t max_response_payload_bytes{
        ipc::TaskProtocolLimits{}.max_frame_bytes - ipc::IpcWireLayout::kHeaderBytes};  // 响应正文必须为协议固定头预留空间，避免回包阶段才发现超限。
    bool require_authenticated_session{true};
};

struct QmtServiceResult {
    ipc::IpcResponseStatus ipc_status{ipc::IpcResponseStatus::Failed};
    network::PackMessageErrorCode error_code{network::PackMessageErrorCode::InternalError};
    std::string message;
    ipc::IpcPayload payload;
    explicit operator bool() const noexcept { return ipc_status == ipc::IpcResponseStatus::Ok; }
};

class QmtServiceRouter {
public:
    QmtServiceRouter(IQmtRepository& repository, QmtServiceRouterOptions options = {});
    QmtServiceResult execute(const ipc::IpcTaskMsg& task) const;
    // 适配 QueryWorkerRuntime；context 必须指向仍存活的 QmtServiceRouter。
    static worker::QueryExecutionResult handle(const ipc::IpcTaskMsg& task, void* context);

private:
    const QmtRouteBinding* resolve(std::uint32_t message_type, std::uint32_t function_id) const noexcept;
    QmtServiceResult execute_operation(QmtOperation operation, const ipc::IpcTaskMsg& task,
                                       std::string_view payload) const;
    IQmtRepository* repository_{nullptr};
    QmtServiceRouterOptions options_;
};

}  // namespace oxmbs::service
