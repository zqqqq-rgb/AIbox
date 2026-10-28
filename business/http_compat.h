#pragma once

#include <exception>
#include <string>

namespace aibox {
namespace biz {

// cpp-httplib releases use either exception& or exception_ptr callbacks. Keep
// board packages and distribution shared libraries on the same error path.
inline std::string http_exception_message(const std::exception& error) {
    return error.what();
}

inline std::string http_exception_message(const std::exception_ptr& error) {
    try {
        if (error) std::rethrow_exception(error);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unknown exception";
    }
    return "unknown exception";
}

}  // namespace biz
}  // namespace aibox
