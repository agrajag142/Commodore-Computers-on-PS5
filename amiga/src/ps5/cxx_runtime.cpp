// cxx_runtime.cpp
// The little of the C++ library's runtime the emulator app needs that a PS5 app
// doesn't link: the kit leaves out libc++ itself (no exceptions, no RTTI), and
// libc++ keeps std::string's functions in the library rather than in each
// program. They're made here instead.

#ifndef PS5_SIM
#include <__verbose_abort>
#include <stdio.h>
#include <stdlib.h>
#include <string>

// std::string, all of it
template class std::basic_string<char>;
template std::string std::operator+(const char *, const std::string &);

namespace std {
inline namespace __1 {
// libc++ calls this where it would throw (a string too long, and the like):
// there are no exceptions in a PS5 app, so it stops
void __libcpp_verbose_abort(char const *format, ...) {
  printf("Amiga: C++ library abort: %s\n", format);
  abort();
}
} // namespace __1
} // namespace std
#endif
