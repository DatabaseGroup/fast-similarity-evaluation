#ifndef DEBUG_HH
#define DEBUG_HH

namespace util {

enum LogLevel {
  INFO,
  DEBUG
};

#if FAST_DEBUG
inline void print_dbg(const std::string& s, LogLevel level = INFO, const std::string& eol = "\n") {
  if (level == INFO) {
    std::cerr << s << eol;
  }
}
#else
inline void print_dbg([[maybe_unused]] const std::string& s, [[maybe_unused]] LogLevel level = INFO, [[maybe_unused]] const std::string& eol = "\n") {}
#endif

}  // namespace util

#endif  // DEBUG_HH
