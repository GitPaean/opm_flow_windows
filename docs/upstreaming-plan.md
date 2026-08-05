# Upstreaming the Windows changes

The `windows_clean_further` branches were split into narrower branches so each
one can be reviewed, argued about and merged on its own. This file is the
working plan: what the branches are, what order they go in, and how to drive
them through review.

All branches are on `GitPaean/<module>` and are based on `upstream/master`.

## The one thing to say in every PR body

**No individual PR in this set can be validated on Windows on its own.** The
Windows build only succeeds once all of a module's PRs are applied together.
The clearest case: `opm-common/windows-portability` adds a `.gitattributes`
that fixes the checkout, but building it alone still fails with

    cl : Command line error D8021 : invalid numeric argument '/Wno-shadow'

because the `if (NOT MSVC)` guard that removes that flag lives in the *other*
PR. The split is for reviewability, not for independent testability.

Say this explicitly in each PR body, or a reviewer will reasonably ask why the
Windows CI is not green.

The other half of the caveat, which is easy to overstate: these branches have
**not** been built on Linux here. Every change was reviewed for Linux safety --
nothing is guarded on the compiler except where stated, and the platform code is
inside `#ifdef` -- but that is an argument, not a test result. The OPM CI is the
first thing that will actually compile them on Linux. Watch the first PR of each
module closely for that reason, and do not assume the later ones in the stack
inherit its result.

## Inventory

Nine PRs. Within a module they are stacked -- each branch contains the one
above it -- so they must merge in the order listed.

### opm-common

| # | Branch | Size | What it is |
|---|--------|------|------------|
| 1 | `windows-portability` | 34 files, +442/-69 | Standards conformance, real bugs, `.gitattributes` |
| 2 | `windows-msvc` | 11 files, +154/-29 | MSVC-only build plumbing |

PR 1 carries two genuine bugs worth calling out in the body: the `24L` in
`TimeService.cpp` made the seconds-per-day arithmetic 32-bit wherever `long` is
32-bit, so any date outside roughly 1902-2038 came back wrong (a schedule
reaching year 3000 returned 1911); and `TimeStampUTC` dereferenced whatever
`std::gmtime` returned, including `nullptr`, while also inheriting its static
buffer's thread-safety hazard. `tests/test_TimeService.cpp` is new and covers
both. It found the `24L` bug on its first run.

### opm-grid

| # | Branch | Size | What it is |
|---|--------|------|------------|
| 1 | `windows-portability` | 7 files, +51/-16 | Conformance, `typename`, dropping Fortran |
| 2 | `windows-openmp` | 2 files, +13/-1 | Index-form loop for OpenMP < 5.0 |

PR 1's Fortran removal is the change most likely to attract discussion, so lead
with the argument: `FCMacros.h` is generated and then never `#include`d
anywhere in the module, so the Fortran requirement bought nothing.

### opm-simulators

| # | Branch | Size | What it is |
|---|--------|------|------------|
| 1 | `windows-portability` | 23 files, +97/-37 | Platform-neutral fixes and two real bugs |
| 2 | `windows-msvc` | 6 files, +76/-52 | MSVC front-end workarounds |
| 3 | `windows-platform` | 5 files, +178/-23 | Windows equivalents for POSIX calls |
| 4 | `windows-ecoqos` | 1 file, +35 | Power-throttling exemption |

PR 1 contains the `pvsprimaryvariables` fix, which is the single most important
change in the whole set: with no phase present the code computed `ffs(0) - 1`
and used it as an index. Note in the body *why* the obvious modernisation is
wrong -- `std::countr_zero(0)` on a 16-bit argument is `16`, an index that looks
in-bounds and would quietly corrupt `saturation_` instead of faulting. That
argument is what justifies the `throw`.

PR 4 is the one to expect pushback on, since it is a performance hint rather
than a correctness fix. It is deliberately last and depends on nothing, so it
can be dropped without disturbing anything else.

### opm-upscaling

| # | Branch | Size | What it is |
|---|--------|------|------------|
| 1 | `windows-build` | 2 files, +60/-7 | Configure without a Fortran compiler |

Kept as one PR because it is two files. If the `FCMacros.h` fallback attracts
debate, split the two-line `IncompFlowSolverHybrid.hpp` fix out so it is not
held up by it.

## Order

The modules build in the order `opm-common` -> `opm-grid` -> `opm-simulators`
-> `opm-upscaling`, and the CI builds them together.

None of these changes alter an API, so there is **no cross-module dependency**:
opm-simulators PR 1 does not need opm-common PR 1 to be merged first. The four
modules can therefore run in parallel. The build order matters only for how CI
assembles the tree, not for merge sequencing.

Within a module the stack is strict. Do not open PR N+1 before PR N is merged
(see below for why).

## Driving one module through

Work one PR at a time. The stacked branches all contain their predecessors, so
opening them all at once would show reviewers a cumulative diff on every PR
after the first.

1. Open PR 1 against `OPM/<module>:master`.

2. In the description, state the no-Windows-CI caveat above, and say which PRs
   are queued behind this one so reviewers know the shape of what is coming.

3. Let the OPM Jenkins CI run. It builds the whole module chain on Linux. If a
   companion PR in another module is ever needed, add a trigger line naming it:

   ```
   jenkins build this with downstreams opm-simulators=1234
   ```

   None of these nine should need one, since nothing changes an API. If CI asks
   for one, that is a signal something is not as independent as intended --
   investigate rather than papering over it with a companion.

4. Once PR 1 merges, rebase the rest of that module's stack onto the new master
   and force-push. For opm-simulators:

   ```bash
   git fetch upstream && for b in windows-msvc windows-platform windows-ecoqos; do git rebase upstream/master $b; done && git push origin windows-msvc windows-platform windows-ecoqos --force-with-lease
   ```

   Since PR 1's commits are now in master, each remaining branch drops to only
   its own changes. If upstream squashed the merge, the rebase may stop on the
   already-applied commit; `git rebase --skip` is the right answer there, but
   confirm with `git diff` that what it skipped really is already in master
   rather than assuming it.

5. Open PR 2. Repeat.

## Re-verifying after a rebase

Every rebase is a chance to lose a hunk -- it has happened once already in this
work, where a stash dance silently dropped the C2026 string-literal split from
both opm-common branches and the next build failed on it.

After rebasing, check the union against the tree that was actually built and
tested:

```bash
git diff --stat windows_clean_further <top-of-stack>
```

Empty output means the stack still reconstructs the tested tree byte for byte.
Anything else needs explaining before pushing. Keep the `windows_clean_further`
branches around as the reference for exactly this.

To re-verify the union on Windows, check out the top of the stack in each module
and build with testing on:

```bash
./build-all.ps1 -Jobs 8 -Extra -DBUILD_TESTING=ON -DBUILD_EXAMPLES=ON
```

`BUILD_EXAMPLES=ON` is required alongside `BUILD_TESTING=ON`: opm-simulators'
`modelTests.cmake` registers tests against `obstacle_immiscible` and
`obstacle_pvs`, which are built from `EXAMPLE_SOURCE_FILES`. That is an upstream
constraint, not something these branches introduce.

Neither module reaches a green ctest on Windows, and that is expected:

  * **opm-common: 226/229.** `ParserIncludeTests` needs git symlinks;
    `rst_deck_test` and `rst_deck_test2` are driven by a shell script.

  * **opm-simulators: 73/139.** All 66 failures are `BAD_COMMAND` -- ctest
    could not launch the process at all -- and the failing set is *exactly* the
    set of tests whose ctest command is a `.sh` script (`run-vtu-test.sh`,
    `run-parallel-unitTest.sh`). Windows cannot execute those directly. Every
    test that does start passes.

That last point is worth reproducing rather than taking on trust, because it
makes the result independent of any baseline. Compare the failing names against
the shell-driven ones:

```bash
ctest --show-only=json-v1 | jq -r '.tests[] | select((.command|join(" "))|test("\\.sh")) | .name' | sort > /tmp/sh.txt
```

If the two sets match exactly, no test regressed. If a name appears in the
failures that is not in `/tmp/sh.txt`, that is a real failure and needs
investigating.

Beware of truncating ctest's output when checking this. `| Select-Object -Last N`
drops the `"N tests failed out of M"` summary line and leaves only the tail of
the failure list, which reads as a much smaller failure count than the real one.

## If a PR stalls

The stack is ordered so the least controversial work is at the bottom. If a
later PR stalls in review, everything below it is already merged and the Windows
build simply stays broken at that point in the chain -- nothing needs reverting.

The two most droppable pieces, in order: `opm-simulators/windows-ecoqos` (pure
performance hint, depends on nothing) and `opm-grid`'s Fortran removal (could be
narrowed to a host check if outright removal is rejected, though the header it
generates really is unused).
