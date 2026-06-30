/* Minimal POSIX getopt() shim for MSVC (single-TU tools only). */
#ifndef OPM_COMPAT_GETOPT_H
#define OPM_COMPAT_GETOPT_H
#include <stdio.h>
#include <string.h>
static char* optarg = NULL;
static int   optind = 1;
static int   opterr = 1;
static int   optopt = 0;
static int getopt(int argc, char* const argv[], const char* optstring) {
    static int optpos = 1;
    const char* arg; int optchar; const char* match;
    optarg = NULL;
    if (optind >= argc) return -1;
    arg = argv[optind];
    if (arg[0] != '-' || arg[1] == '\0') return -1;
    if (arg[1] == '-' && arg[2] == '\0') { ++optind; return -1; }
    optchar = (unsigned char)arg[optpos];
    match = strchr(optstring, optchar);
    if (optchar == ':' || match == NULL) {
        optopt = optchar;
        if (opterr && *optstring != ':') fprintf(stderr, "%s: illegal option -- %c\n", argv[0], optchar);
        if (arg[++optpos] == '\0') { ++optind; optpos = 1; }
        return '?';
    }
    if (match[1] == ':') {
        if (arg[optpos + 1] != '\0') { optarg = (char*)&arg[optpos + 1]; ++optind; }
        else if (optind + 1 < argc) { optarg = (char*)argv[optind + 1]; optind += 2; }
        else {
            optopt = optchar; ++optind; optpos = 1;
            if (opterr && *optstring != ':') fprintf(stderr, "%s: option requires an argument -- %c\n", argv[0], optchar);
            return (*optstring == ':') ? ':' : '?';
        }
        optpos = 1;
    } else {
        if (arg[++optpos] == '\0') { ++optind; optpos = 1; }
    }
    return optchar;
}
#endif
