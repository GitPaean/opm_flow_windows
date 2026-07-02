/* Minimal <unistd.h> shim for MSVC. */
#ifndef OPM_COMPAT_UNISTD_H
#define OPM_COMPAT_UNISTD_H
#ifndef _CRT_NONSTDC_NO_DEPRECATE
#define _CRT_NONSTDC_NO_DEPRECATE
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#endif
#include <io.h>
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
#endif /* OPM_COMPAT_UNISTD_H */
