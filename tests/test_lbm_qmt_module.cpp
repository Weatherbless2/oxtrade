#include "oxmbs/ipc/task_protocol.h"
#include "oxmbs/lbm/lbm_task_dispatcher.h"
#include "oxmbs/lbm/qmt_lbm_host.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class MemoryQmtRepository final : public oxmbs::service::IQmtRepository {
public:
    bool list_stock_instruments(const std::string&, const std::string&, const oxmbs::storage::PageRequest&,
                                oxmbs::storage::PageResult<oxmbs::storage::InstrumentRecord>& result,
                                oxmbs::storage::RepositoryFailure& failure) const override {
        result = {};
        failure = {};
        return true;
    }
    bool list_futures_instruments(const std::string&, const std::string&, const oxmbs::storage::PageRequest&,
                                  oxmbs::storage::PageResult<oxmbs::storage::FuturesInstrumentRecord>& result,
                                  oxmbs::storage::RepositoryFailure& failure) const override {
        result = {};
        failure = {};
        return true;
    }
    bool list_calendar(const std::string&, const std::string&, const std::string&, const oxmbs::storage::PageRequest&,
                       oxmbs::storage::PageResult<oxmbs::storage::CalendarRecord>& result,
                       oxmbs::storage::RepositoryFailure& failure) const override {
        result = {};
        failure = {};
        return true;
    }
    bool list_stock_quotes(const std::string&, const std::string&, const oxmbs::storage::PageRequest&,
                           oxmbs::storage::PageResult<oxmbs::storage::StockQuoteRecord>& result,
                           oxmbs::storage::RepositoryFailure& failure) const override {
        result = {};
        failure = {};
        return true;
    }
    bool list_futures_quotes(const std::string&, const std::string&, const oxmbs::storage::PageRequest&,
                             oxmbs::storage::PageResult<oxmbs::storage::FuturesQuoteRecord>& result,
                             oxmbs::storage::RepositoryFailure& failure) const override {
        result = {};
        failure = {};
        return true;
    }
    bool list_all_latest_quotes(const std::string&, const oxmbs::storage::PageRequest& page,
                                oxmbs::storage::PageResult<oxmbs::storage::LatestQuoteRecord>& result,
                                oxmbs::storage::RepositoryFailure& failure) const override {
        const oxmbs::storage::StockQuoteRecord quote{
            "0", "000001", "2026-09-15T09:30:00", "2026-09-15T09:30:01", "10.50", "10.20", "10.60",
            "10.10", "10.00", "100", "1050", "10.49", "20", "10.51", "30", "平安银行"};
        result = {};
        result.total = 1;
        if (page.offset == 0 && page.limit != 0) {
            result.rows.push_back({false, quote, {}});
        }
        failure = {};
        return true;
    }
};

std::filesystem::path write_funclist() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("oxmbs_lbm_qmt_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "funclist.xml";
    std::ofstream stream(path, std::ios::binary);
    assert(stream);
    stream << "<funclist>\n"
           << "<function msg_type=\"2\" func_id=\"1\" library=\"" << OXMBS_LBM_QMT_LIBRARY
           << "\" symbol=\"oxmbs_lbm_qmt_instrument_search\" />\n"
           << "<function msg_type=\"2\" func_id=\"2\" library=\"" << OXMBS_LBM_QMT_LIBRARY
           << "\" symbol=\"oxmbs_lbm_qmt_calendar_page\" />\n"
           << "<function msg_type=\"2\" func_id=\"3\" library=\"" << OXMBS_LBM_QMT_LIBRARY
           << "\" symbol=\"oxmbs_lbm_qmt_quote_latest\" />\n"
           << "</funclist>\n";
    assert(stream.good());
    return path;
}

oxmbs::ipc::IpcTaskMsg quote_task() {
    oxmbs::ipc::IpcTaskMsg task;
    task.task_seq = 501;
    task.session_id = 77;
    task.deadline_unix_ms = 4'000'000'000'000ULL;
    task.msg_type = 2;
    task.func_id = 3;
    constexpr char kRequest[] = R"({"schema_version":1,"request_id":"lbm-qmt-quote","data":{},"page":{"page_size":1,"sort":"receive_time"}})";
    task.payload.assign(kRequest, kRequest + sizeof(kRequest) - 1);
    return task;
}

void test_qmt_module_loads_from_xml_and_uses_host_repository() {
    MemoryQmtRepository repository;
    oxmbs::lbm::QmtLbmHostContext host;
    host.repository = &repository;
    host.max_page_size = 50;
    host.max_response_payload_bytes = oxmbs::ipc::TaskProtocolLimits{}.max_frame_bytes - oxmbs::ipc::IpcWireLayout::kHeaderBytes;
    oxmbs::lbm::LbmModuleLoaderOptions options;
    options.host_context = &host;
    oxmbs::lbm::LbmTaskDispatcher dispatcher(options);
    const auto path = write_funclist();
    assert(dispatcher.load_funclist(path));

    const auto result = dispatcher.execute(quote_task());
    assert(result.status == oxmbs::ipc::IpcResponseStatus::Ok);
    const std::string payload(result.payload.begin(), result.payload.end());
    assert(payload.find("000001") != std::string::npos);
    assert(payload.find("QMT/PG") != std::string::npos);  // 结果由 .so 内 QMT 路由经宿主 Repository 生成，不是迁移桥接回包。
    assert(dispatcher.loader().loaded_library_count() == 1);

    auto unknown = quote_task();
    unknown.func_id = 99;
    const auto rejected = dispatcher.execute(unknown);
    assert(rejected.status == oxmbs::ipc::IpcResponseStatus::Rejected);
    std::filesystem::remove_all(path.parent_path());
}

}  // namespace

int main() {
    test_qmt_module_loads_from_xml_and_uses_host_repository();
    return 0;
}
