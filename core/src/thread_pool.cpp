#include "ip/thread_pool.h"
// 头文件自足实现（模板类），此翻译单元仅保证链接符号存在
namespace ip {
// 防止空静态库告警
namespace detail {
const char* threadPoolTag() { return "ip_thread_pool"; }
} // namespace detail
} // namespace ip
