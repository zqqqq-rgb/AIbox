// NonCopyable —— 禁拷贝基类（详设 §1.2 common/ 清单）
#pragma once

namespace aibox {

class NonCopyable {
public:
    NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;

protected:
    ~NonCopyable() = default;
};

}  // namespace aibox
