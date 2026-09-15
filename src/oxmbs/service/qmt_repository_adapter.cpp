#include "oxmbs/service/qmt_service_router.h"

namespace oxmbs::service {

bool QmtRepositoryAdapter::list_stock_instruments(const std::string& market, const std::string& keyword,
                                                  const storage::PageRequest& page,
                                                  storage::PageResult<storage::InstrumentRecord>& result,
                                                  storage::RepositoryFailure& failure) const {
    return repository_ != nullptr && repository_->list_stock_instruments(market, keyword, page, result, failure);
}

bool QmtRepositoryAdapter::list_futures_instruments(const std::string& market, const std::string& keyword,
                                                    const storage::PageRequest& page,
                                                    storage::PageResult<storage::FuturesInstrumentRecord>& result,
                                                    storage::RepositoryFailure& failure) const {
    return repository_ != nullptr && repository_->list_futures_instruments(market, keyword, page, result, failure);
}

bool QmtRepositoryAdapter::list_calendar(const std::string& start_date, const std::string& end_date,
                                         const std::string& market, const storage::PageRequest& page,
                                         storage::PageResult<storage::CalendarRecord>& result,
                                         storage::RepositoryFailure& failure) const {
    return repository_ != nullptr && repository_->list_calendar(start_date, end_date, market, page, result, failure);
}

bool QmtRepositoryAdapter::list_stock_quotes(const std::string& market, const std::string& stkcode,
                                             const storage::PageRequest& page,
                                             storage::PageResult<storage::StockQuoteRecord>& result,
                                             storage::RepositoryFailure& failure) const {
    return repository_ != nullptr && repository_->list_stock_quotes(market, stkcode, page, result, failure);
}

bool QmtRepositoryAdapter::list_futures_quotes(const std::string& market, const std::string& stkcode,
                                               const storage::PageRequest& page,
                                               storage::PageResult<storage::FuturesQuoteRecord>& result,
                                               storage::RepositoryFailure& failure) const {
    return repository_ != nullptr && repository_->list_futures_quotes(market, stkcode, page, result, failure);
}

bool QmtRepositoryAdapter::list_all_latest_quotes(const std::string& stkcode,
                                                  const storage::PageRequest& page,
                                                  storage::PageResult<storage::LatestQuoteRecord>& result,
                                                  storage::RepositoryFailure& failure) const {
    return repository_ != nullptr && repository_->list_all_latest_quotes(stkcode, page, result, failure);
}

}  // namespace oxmbs::service
