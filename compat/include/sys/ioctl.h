/* Minimal <sys/ioctl.h> shim for MSVC. Intentionally does NOT define TIOCGWINSZ,
   so OPM falls back to its default terminal width. */
#ifndef OPM_COMPAT_SYS_IOCTL_H
#define OPM_COMPAT_SYS_IOCTL_H
#endif
