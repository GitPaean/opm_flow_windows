/* Minimal POSIX getopt() and GNU getopt_long() shim for MSVC (single-TU tools only). */
/* Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see repository LICENSE). */
#ifndef OPM_COMPAT_GETOPT_H
#define OPM_COMPAT_GETOPT_H
#include <stdio.h>
#include <string.h>
static char* optarg = NULL;
static int   optind = 1;
static int   opterr = 1;
static int   optopt = 0;
/* Position within a group of short options (-abc); 1 between arguments. */
static int   opm_optpos = 1;
static int getopt(int argc, char* const argv[], const char* optstring) {
    const char* arg; int optchar; const char* match;
    optarg = NULL;
    if (optind >= argc) return -1;
    arg = argv[optind];
    if (arg[0] != '-' || arg[1] == '\0') return -1;
    if (arg[1] == '-' && arg[2] == '\0') { ++optind; return -1; }
    optchar = (unsigned char)arg[opm_optpos];
    match = strchr(optstring, optchar);
    if (optchar == ':' || match == NULL) {
        optopt = optchar;
        if (opterr && *optstring != ':') fprintf(stderr, "%s: illegal option -- %c\n", argv[0], optchar);
        if (arg[++opm_optpos] == '\0') { ++optind; opm_optpos = 1; }
        return '?';
    }
    if (match[1] == ':') {
        if (arg[opm_optpos + 1] != '\0') { optarg = (char*)&arg[opm_optpos + 1]; ++optind; }
        else if (optind + 1 < argc) { optarg = (char*)argv[optind + 1]; optind += 2; }
        else {
            optopt = optchar; ++optind; opm_optpos = 1;
            if (opterr && *optstring != ':') fprintf(stderr, "%s: option requires an argument -- %c\n", argv[0], optchar);
            return (*optstring == ':') ? ':' : '?';
        }
        opm_optpos = 1;
    } else {
        if (arg[++opm_optpos] == '\0') { ++optind; opm_optpos = 1; }
    }
    return optchar;
}
/* GNU getopt_long(): --name, --name=value and, for a required argument,
   --name value; an unambiguous prefix of a name will do. Anything else goes to
   getopt(), and as there the options come before the other arguments. */
struct option {
    const char* name;
    int         has_arg;
    int*        flag;
    int         val;
};
#define no_argument       0
#define required_argument 1
#define optional_argument 2
static int getopt_long(int argc, char* const argv[], const char* optstring,
                       const struct option* longopts, int* longindex) {
    const char* name; const char* eq; size_t len; int i, found = -1, ambiguous = 0;
    if (opm_optpos != 1 || optind >= argc) return getopt(argc, argv, optstring);
    name = argv[optind];
    if (name[0] != '-' || name[1] != '-' || name[2] == '\0') return getopt(argc, argv, optstring);
    name += 2;
    eq = strchr(name, '=');
    len = eq ? (size_t)(eq - name) : strlen(name);
    for (i = 0; longopts[i].name; ++i) {
        if (strncmp(longopts[i].name, name, len) != 0) continue;
        if (strlen(longopts[i].name) == len) { found = i; ambiguous = 0; break; }
        if (found < 0) found = i; else ambiguous = 1;
    }
    optarg = NULL; optopt = 0; ++optind;
    if (found < 0 || ambiguous) {
        if (opterr && *optstring != ':') fprintf(stderr, "%s: %s option '--%.*s'\n", argv[0], ambiguous ? "ambiguous" : "unrecognized", (int)len, name);
        return '?';
    }
    if (eq) {
        if (longopts[found].has_arg == no_argument) {
            optopt = longopts[found].val;
            if (opterr && *optstring != ':') fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n", argv[0], longopts[found].name);
            return '?';
        }
        optarg = (char*)eq + 1;
    } else if (longopts[found].has_arg == required_argument) {
        if (optind >= argc) {
            optopt = longopts[found].val;
            if (opterr && *optstring != ':') fprintf(stderr, "%s: option '--%s' requires an argument\n", argv[0], longopts[found].name);
            return (*optstring == ':') ? ':' : '?';
        }
        optarg = argv[optind++];
    }
    if (longindex) *longindex = found;
    if (longopts[found].flag) { *longopts[found].flag = longopts[found].val; return 0; }
    return longopts[found].val;
}
#endif
