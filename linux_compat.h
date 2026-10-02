// linux_compat.h - force-included into every file of the Linux build (see
// Makefile). A game server's glibc may be much older than the one this is
// built on, so references that newer glibc headers would tie to new symbol
// versions are pinned back to the originals:
//   - glibc 2.38 headers turn strtol/strtoul/strtoull (and atoi, which calls
//     strtol) and the scanf family (sscanf, fscanf, ...) into C23 "__isoc23_"
//     functions that older glibcs do not have - a server with an older glibc
//     then refuses to load the plugin at all;
//   - glibc 2.34 moved dladdr into libc with a new version.
// With this the plugin needs nothing newer than GLIBC_2.4.
#ifndef TFC_TEAMS_LINUX_COMPAT_H
#define TFC_TEAMS_LINUX_COMPAT_H
#if defined(__linux__) && defined(__i386__)
__asm__(".symver __isoc23_strtol,strtol@GLIBC_2.0");
__asm__(".symver __isoc23_strtoul,strtoul@GLIBC_2.0");
__asm__(".symver __isoc23_strtoull,strtoull@GLIBC_2.0");
__asm__(".symver __isoc23_strtoll,strtoll@GLIBC_2.0");
__asm__(".symver __isoc23_sscanf,sscanf@GLIBC_2.0");
__asm__(".symver __isoc23_fscanf,fscanf@GLIBC_2.0");
__asm__(".symver __isoc23_scanf,scanf@GLIBC_2.0");
__asm__(".symver __isoc23_vsscanf,vsscanf@GLIBC_2.0");
__asm__(".symver __isoc23_vfscanf,vfscanf@GLIBC_2.0");
__asm__(".symver dladdr,dladdr@GLIBC_2.0");
#endif
#endif
