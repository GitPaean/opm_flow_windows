/* Minimal <unistd.h> shim for MSVC. */
/* Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see repository LICENSE). */
#ifndef OPM_COMPAT_UNISTD_H
#define OPM_COMPAT_UNISTD_H
#ifndef _CRT_NONSTDC_NO_DEPRECATE
#define _CRT_NONSTDC_NO_DEPRECATE
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#endif
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <direct.h>
#include <stdlib.h>
#include <string.h>
#ifndef F_OK
#define F_OK 0
#endif
#ifndef X_OK
#define X_OK 0
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllimport) void __stdcall Sleep(unsigned long dwMilliseconds);
/* kernel32; reports installed RAM in kilobytes, or fails on a machine whose
   firmware does not say. Used by sysconf() below, and declared here rather
   than through <windows.h>, which this header must not drag into every
   translation unit that wants sleep(). */
__declspec(dllimport) int __stdcall GetPhysicallyInstalledSystemMemory(unsigned long long* TotalMemoryInKilobytes);
#ifdef __cplusplus
}
#endif
static __inline unsigned int sleep(unsigned int seconds) { Sleep(seconds * 1000u); return 0; }
static __inline int usleep(unsigned int usec) { Sleep(usec / 1000u); return 0; }
/* POSIX gethostname(): absent on MSVC (winsock only). Best-effort from the
   COMPUTERNAME environment variable, enough for the log banners OPM uses it for. */
static __inline int gethostname(char* name, size_t namelen) {
    const char* cn = getenv("COMPUTERNAME");
    if (!name || namelen == 0) { return -1; }
    strncpy_s(name, namelen, cn ? cn : "localhost", _TRUNCATE);
    return 0;
}
/* POSIX getlogin(): the user name, from the environment. */
static __inline char* getlogin(void) { return getenv("USERNAME"); }
/* The two sysconf() values OPM asks for, chosen so that their product is the
   installed memory in bytes. Zero when the firmware does not report it, which
   is what sysconf() returns for an unknown name anyway. */
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE   30
#define _SC_PAGE_SIZE  _SC_PAGESIZE
#define _SC_PHYS_PAGES 85
#endif
static __inline long sysconf(int name) {
    unsigned long long kb = 0;
    if (name == _SC_PAGESIZE) { return 1024; }
    if (name == _SC_PHYS_PAGES) {
        return GetPhysicallyInstalledSystemMemory(&kb) ? (long)kb : 0L;
    }
    return -1;
}
/* POSIX setenv()/unsetenv() over _putenv_s, which always overwrites, so the
   overwrite flag is honoured by looking first. */
static __inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && getenv(name)) { return 0; }
    return _putenv_s(name, value ? value : "");
}
static __inline int unsetenv(const char* name) { return _putenv_s(name, ""); }
/* _fileno() returns -2 for a standard stream attached to nothing - a process
   started by a service or by a process manager without a console - and the
   CRT treats such a descriptor as an invalid parameter, which by default ends
   the process. Answer as the POSIX calls do for a bad descriptor instead.
   Macros: the names are already declared by <io.h> above. */
static __inline int opm_compat_isatty(int fd) { return (fd >= 0) ? _isatty(fd) : 0; }
static __inline int opm_compat_dup2(int fd1, int fd2) {
    if (fd1 < 0 || fd2 < 0) { errno = EBADF; return -1; }
    return _dup2(fd1, fd2);
}
#define isatty opm_compat_isatty
#define dup2   opm_compat_dup2
#endif /* OPM_COMPAT_UNISTD_H */
