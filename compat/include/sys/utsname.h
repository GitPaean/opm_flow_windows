/* Minimal <sys/utsname.h> shim for MSVC. */
#ifndef OPM_COMPAT_SYS_UTSNAME_H
#define OPM_COMPAT_SYS_UTSNAME_H
#include <string.h>
#include <stdlib.h>
#define _UTSNAME_LENGTH 256
struct utsname {
    char sysname[_UTSNAME_LENGTH];
    char nodename[_UTSNAME_LENGTH];
    char release[_UTSNAME_LENGTH];
    char version[_UTSNAME_LENGTH];
    char machine[_UTSNAME_LENGTH];
};
static __inline int uname(struct utsname* buf) {
    if (!buf) { return -1; }
    strncpy_s(buf->sysname, _UTSNAME_LENGTH, "Windows", _TRUNCATE);
    const char* host = getenv("COMPUTERNAME");
    strncpy_s(buf->nodename, _UTSNAME_LENGTH, host ? host : "localhost", _TRUNCATE);
    buf->release[0] = '\0';
    buf->version[0] = '\0';
    const char* arch = getenv("PROCESSOR_ARCHITECTURE");
    strncpy_s(buf->machine, _UTSNAME_LENGTH, arch ? arch : "x86_64", _TRUNCATE);
    return 0;
}
#endif
