#ifndef DEBUG_HH
#define DEBUG_HH

namespace util {

#if FAST_DEBUG
inline void print_dbg(const std::string& s, const std::string& eol = "\n") { std::cerr << s << eol; }
#else
inline void print_dbg([[maybe_unused]] const std::string& s, [[maybe_unused]] const std::string& eol = "\n") {}
#endif

}  // namespace util

#endif  // DEBUG_HH
