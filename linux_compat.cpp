// linux_compat.cpp - Linux build only. The plugin is linked WITHOUT libstdc++
// (-fno-exceptions -fno-rtti, see Makefile), so it does not depend on the
// server's libstdc++ version or drag a newer glibc in with a static copy. The
// few things the compiler still expects from the C++ runtime are here: the
// allocation operators (plain malloc/free) and std::vector's "too big" error,
// which with exceptions off can only end the program.
#if defined(__linux__)
#include <stdlib.h>
#include <stddef.h>

#define TT_HIDDEN __attribute__((visibility("hidden")))

TT_HIDDEN void *operator new(size_t n) { void *p = malloc(n ? n : 1); if (!p) abort(); return p; }
TT_HIDDEN void *operator new[](size_t n) { void *p = malloc(n ? n : 1); if (!p) abort(); return p; }
TT_HIDDEN void operator delete(void *p) noexcept { free(p); }
TT_HIDDEN void operator delete[](void *p) noexcept { free(p); }
TT_HIDDEN void operator delete(void *p, size_t) noexcept { free(p); }
TT_HIDDEN void operator delete[](void *p, size_t) noexcept { free(p); }

namespace std
{
	TT_HIDDEN void __throw_length_error(const char *) { abort(); }
	TT_HIDDEN void __throw_bad_alloc() { abort(); }
	TT_HIDDEN void __throw_bad_array_new_length() { abort(); }
}
#endif
