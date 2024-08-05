#ifndef DEBUG_HH
#define DEBUG_HH

namespace util {

#if FAST_DEBUG
inline void print_dbg(const std::string& s) {
  std::cerr << s << std::endl;
}
#else
inline void print_dbg([[maybe_unused]] const std::string& s) {}
#endif

}

#endif //DEBUG_HH
