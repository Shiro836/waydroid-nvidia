// Local definitions for symbols NDK r29 libc++ headers expect from the
// dylib but Android 13's platform libc++.so does not export.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace std { inline namespace __1 {
void __libcpp_verbose_abort(const char *format, ...) noexcept {
  va_list ap;
  va_start(ap, format);
  vfprintf(stderr, format, ap);
  va_end(ap);
  abort();
}
} }

// Emit vtables/VTTs locally for stream classes the new headers declare
// extern-template.
template class std::basic_istringstream<char>;
template class std::basic_ostringstream<char>;
template class std::basic_stringstream<char>;
template class std::basic_stringbuf<char>;
