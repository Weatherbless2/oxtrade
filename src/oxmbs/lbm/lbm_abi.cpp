#include "oxmbs/lbm/lbm_abi.h"

namespace oxmbs::lbm {

bool is_valid_lbm_response_code(LbmResponseCode code) noexcept {
    switch (code) {
    case LbmResponseCode::Ok:
    case LbmResponseCode::Rejected:
    case LbmResponseCode::Failed:
    case LbmResponseCode::TimedOut:
        return true;
    }
    return false;
}

}  // namespace oxmbs::lbm
