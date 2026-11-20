#!/usr/bin/env python3
# Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
"""Compile the probes in this directory on Compiler Explorer (godbolt.org).

    python3 compiler-explorer.py                       # the default compilers
    python3 compiler-explorer.py vcpp_v19_latest_x64 dilu_omp friend_class

The first argument, when it names compilers, is a comma-separated list of
Compiler Explorer ids (https://godbolt.org/api/compilers/c++ lists them). MSVC
gets the flags build-module.ps1 gives OPM, GCC the plain C++20 equivalent.
long_raw_string is generated here rather than kept as a 38 kB file.
"""
import json
import pathlib
import sys
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
DEFAULT = "vcpp_v19_44_VS17_14_x64,vcpp_v19_latest_x64,g153"
MSVC_FLAGS = ("/std:c++20 /permissive- /Zc:__cplusplus /Zc:preprocessor /bigobj /EHsc"
              " /wd4068 -D_USE_MATH_DEFINES /c")
OPENMP = {"dilu_omp", "omp_range_for"}


def long_raw_string():
    # One raw literal of 38400 characters: the old cap was 16380 per literal.
    body = "-- a line of deck text that fills the literal out to its length\n" * 300
    return f'#include <cstddef>\nconst char* deck = R"({body})";\n'


def source(case):
    return long_raw_string() if case == "long_raw_string" else (HERE / f"{case}.cpp").read_text()


def compile_on(cid, src, args):
    req = urllib.request.Request(
        f"https://godbolt.org/api/compiler/{cid}/compile",
        data=json.dumps({"source": src, "options": {
            "userArguments": args, "filters": {"binary": False, "execute": False}}}).encode(),
        headers={"Content-Type": "application/json", "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.load(r)


def main(argv):
    compilers = DEFAULT
    if argv and ("," in argv[0] or argv[0].startswith(("vcpp", "g1", "clang"))):
        compilers, argv = argv[0], argv[1:]
    cases = argv or sorted(p.stem for p in HERE.glob("*.cpp")) + ["long_raw_string"]
    for case in cases:
        src = source(case)
        for cid in compilers.split(","):
            if cid.startswith("vcpp"):
                args = MSVC_FLAGS + (" /openmp:llvm" if case in OPENMP else "")
            else:
                args = "-std=c++20 -c -Wall" + (" -fopenmp" if case in OPENMP else "")
            res = compile_on(cid, src, args)
            text = "\n".join(l["text"] for l in res.get("stderr", []) + res.get("stdout", []))
            errors = [l.strip() for l in text.splitlines() if "error" in l]
            print(f"{case:24s} {cid:26s} {'compiles' if res.get('code') == 0 else 'FAILS'}")
            for line in errors[:2]:
                print(f"    {line[:160]}")
            time.sleep(1)


if __name__ == "__main__":
    main(sys.argv[1:])
