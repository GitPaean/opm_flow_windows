# flow-gui — ideas not yet implemented

Things worth doing that nobody has done yet. Not a plan and not a promise:
an idea lands here when it is worth remembering but not worth stopping for,
and leaves when it is either built or decided against. Keep enough of the
reasoning that the idea can be judged later, not just the sentence.

---

## 1. Turn auto-refresh on while a run is going, and put it back afterwards

Re-reading a summary file is cheap next to what the simulation is doing, so
a plot could simply follow a running job by itself: switch auto-refresh on
when a job starts, and restore whatever the setting was when the queue
finishes — a temporary override rather than a change the user has to undo.

*Where it would go:* `SummaryPlotWidget` owns the checkbox (`autoRef_`) and
the 10 s `timer_`; `FlowGuiWindow::startNextJob()` and the job-finished
handler already know when a run begins and ends, and already call
`summary_->caseFinished()`.

*State of play:* auto-refresh now defaults to **on**, and a refresh whose
files have not changed since they were read returns after a few `stat`
calls instead of re-reading them (measured on Norne: 22 ms of reading,
tree rebuild and replot, down to 0.02 ms per idle tick). Between them
those cover what this was after: leaving auto-refresh on is no longer
something a user pays for.

What is left is the narrower idea — overriding the setting for the
duration of a run and restoring the user's own choice afterwards — which
now matters only for someone who deliberately turned it off and would
still like a running job followed. Worth keeping in mind that a saved
session restores its own choice, so "the old setting" has to mean the
user's, not the default.

---

## 2. Per-case run settings, so one queue can hold differently-configured runs

Every run option is currently global to the window: MPI ranks, OMP threads,
output policy and directory, TUNING, extra flow arguments, simulator
override. A queue is therefore a list of decks that all run the same way,
and studying "this deck on 8 ranks, that one on 1" means running the queue
twice and changing the options in between.

The idea: let each queued job carry its own settings, so a queue can hold
genuinely different runs and be started once.

*Where it would go:* the settings live as widgets on `FlowGuiWindow`
(`ranksSpin_`, `threadsSpin_`, `outdirMode_`, `outdirEdit_`, `extraEdit_`,
`tuningChk_`) and are read in `startNextJob()` when a job starts. `struct
Job` already carries per-job state (`deck`, `outdir`, `state`, progress,
exit code), so it is the natural home for a per-job options record. The
project file and the session already serialise both the jobs array and the
global options, so both would need to carry the per-job set too — and keep
reading an older file that only has the global one.

*Questions to settle first, since they decide the shape of it:*
- What do the option widgets mean once jobs own their settings — the
  defaults for newly added decks, an editor for the selected job(s), or
  both? Editing several selected jobs at once is the case that makes a
  study quick to set up.
- How is per-job configuration visible in the queue table without turning
  it into a spreadsheet? Perhaps only where a job differs from the
  defaults, so an ordinary queue still looks like one.
- Does a job keep the settings it actually ran with, so a finished run
  records how it was produced? That is arguably the more valuable half of
  the feature, and it is close to free once the record exists.

---

## 3. Audit the summary-keyword descriptions against a real source

The "Oil Production Rate" text beside each mnemonic comes from a table
hand-written in `SummaryPlotWidget::friendlyName()`. Nothing authoritative
backs it: the entries were written from familiarity, matched on the stem
left after stripping the scope letter.

That matching was scope-blind, which made `GPR` read "Average Reservoir
Pressure" (the meaning of `FPR`/`RPR`) when it is the group nodal
pressure, and `BPR` the same when it is a block's. Both are fixed, and
`friendlyName()` now returns nothing for a scope-dependent stem it has no
entry for, so an unlabelled mnemonic is the failure mode rather than a
confident wrong one.

*What is left:* the rest of the table has never been checked. It is ~130
entries and the same class of error can hide anywhere in it - a
description that is right for one scope and quietly wrong for another.

*Where to check against:* the OPM reference manual's summary appendix
(`opm-reference-manual`, `parts/appendices`, ODF). Its `keyword-names`
lists are deck keywords only and do not carry descriptions, so the text
has to come from the appendix itself. Worth extracting once into a table
that says where each line came from, rather than correcting entries one
report at a time.

---

## 4. A button that plots the summary vectors which regressed most

The Compare tab answers "which of these parted, and when" for restart
data: it reduces every cell field per report date and puts the worst in
front of you. Summary vectors have no equivalent. A run writes hundreds
to thousands of them, two runs are opened side by side all the time, and
finding which ones moved is done by knowing where to look — you plot the
vectors you already suspect, so a regression in one you did not think to
check is not found at all.

The idea: a button that ranks the vectors the checked cases share by how
far apart the runs are, and loads the worst of them straight into the
subplot grid. The interesting half is that it needs no guess about which
vector matters.

*Where it would go:* `vecs_` holds every plottable vector; `checkedCases()`
returns each checked case with its `ESmry*`; `seriesData(v, smry,
isActive, out)` already yields one vector's values for one case and
returns false when that case does not carry it, which is exactly the
"only in A/B" case the Compare tab reports. Filling the result is the
existing subplot path — `setLayoutGrid()`, `ensureCharts()`,
`applyChartLayout()` and the per-subplot key lists the tree edits.

*Questions to settle first, since they decide the shape of it:*
- **Which two runs?** The tab plots any number of checked cases at once,
  while a regression is a statement about a pair. Either an A/B pair is
  borrowed from the Compare tab, or the measure becomes the spread across
  everything checked — the second is a different and vaguer question.
- **The runs need not share timesteps.** Each case is plotted against its
  own `TIME` (`SummaryPlotWidget.cpp:3460`), and two runs will not have
  stepped identically, so a vector cannot be subtracted sample by sample.
  Restart comparison sidesteps this by pairing report *dates*; summary
  data would need either interpolation onto common times or a comparison
  restricted to dates both wrote. Worth deciding before anything else —
  it determines whether this shares code with the Compare tab or not.
- **What counts as "big"?** Ranked on absolute difference the list fills
  with whatever has the largest units — `FOPT` will always outrank a
  well's `WWCT` — and ranked on relative difference it fills with vectors
  that pass near zero. The abs+rel pair the Compare tab already takes
  from compareECL is the obvious precedent. Separately, a vector that
  diverges once and returns is a different failure from one that drifts
  the whole run, and a single ranking cannot say which you want.
- **Scan everything, or what the filter currently shows?** Scanning all
  of them is the point of the feature; honouring the filter is what makes
  it usable when you already know the region of interest.

---

## 5. Step through a set of vectors on a timer, like a presentation

Once a filter has picked out a set — every `WBHP`, say, or the ranked
list from the idea above — looking at them means clicking each in turn.
A play button that advanced the plot through the set on an interval would
turn that into watching, which is both a faster way to review a run and
the natural way to show one to somebody else.

*Where it would go:* `filter_` and `tree_` already produce the candidate
set, and the tree edits the focused subplot's key list, so advancing is
rewriting that list and replotting. `timer_` and `autoRef_` are the
precedent for a timed action, and the caution they carry applies here:
the two timers must not fight, since a refresh mid-step would replot
underneath the step.

*Questions to settle first:*
- **What advances — one subplot or the whole grid?** A 3x2 layout paging
  six at a time reviews a long list far quicker than one vector at a
  time, and is the better fit for showing someone. One at a time is the
  better fit for study.
- **Where does the list come from:** everything the filter matches, or
  only what is checked in the tree? The first needs no setup; the second
  lets a set be built deliberately and then played.
- **Zoom has to be reset as it steps.** Per-subplot axis ranges are kept
  across refreshes on purpose; a range that suits one vector will hide
  the next one entirely, so stepping should clear it rather than inherit
  it.
- Does it stop at the end or loop, and does interacting pause it? Both
  matter more for presenting than for reviewing.

These two compose: rank by regression, then play the ranked list — which
is close to an automatic review of what changed between two runs.

---

## 6. Scale the Compare overview by the tolerance rather than by a guess

Each overview plot holds its axis open to `kMinRelSpan` — a fixed share
of the property's own magnitude — so that two runs which agree are not
drawn as a chasm. That constant reached its present value by being tried
and looked at: fitted to the data every property screamed; at a fiftieth
a one-percent gap still filled most of the frame; anchored at zero every
curve went flat against an empty frame. A seventh looks right on the
cases it was tried on. Nothing says it is right on a case with different
numbers in it, and the next person to find it too loud or too quiet has
no way to reason about it — only the same eye and a different case.

The tolerance is the honest yardstick. The tab already asks for `abs`
and `rel` and defines "these runs agree" by them, so an axis scaled in
units of the tolerance would make *flat* mean exactly *passes the test* —
and a curve with visible shape mean exactly *does not*. The picture and
the verdict would then be the same statement, and a number nobody chose
by eye would set the scale.

*Where it would go:* `PropertyPlots::setResult()` computes `rowLo_`/
`rowHi_` and applies `kMinRelSpan` there. The blocker is small and worth
naming: `DiffTol` is passed to `compareRestarts()` (`RestartCompare.h`)
but never stored on `CompareResult`, so the plots cannot currently see
the tolerances at all. Putting it on the result is the prerequisite.

*Worth weighing before doing it:* the tolerance is a per-cell test, while
these plots draw the field average, so scaling one by the other is not
the tidy identity it first looks like — see the next idea. And a run
compared at a deliberately loose tolerance would flatten everything,
which is either exactly right or a trap, depending on why it was
loosened.

---

## 7. Say why a property's name is red above a flat curve

A property whose name is red and bold in the overview has cells outside
tolerance. Its curve is the pore-volume weighted field average. Those are
different scopes, and they disagree in a way that is entirely possible
and reads as a contradiction: on `COMP_EQUIL_1D_VERTICAL` against its
rerun, `PRESSURE` has 30 cells outside tolerance and a max |A−B| of
0.0267, while the two runs' field averages sit less than 0.01% apart —
so the name says *this differs* directly above a flat line and a caption
reading `gap < 0.01%`.

Both are true. Averaging is what hides it: a bounded disagreement spread
over 20 cells barely moves a mean, and on a real field a handful of bad
cells in ten thousand never will. The overview is not wrong here, but it
is quietly inviting the wrong conclusion — that the property is fine.

The fix is probably a sentence, not a feature: the per-cell measures are
already in the picker (`cells outside tolerance`, `max |A-B|`, `RMS`) and
already tell the other half of the story. What is missing is anything
that says to go and look at them.

*Where it would go:* `PropertyPlots::paintEvent()` draws the name from
`k.clean()` and the caption from `rowRel_`; both halves are in hand at
the same moment, so the case of *not clean, but the averages agree* can
be detected exactly where it is drawn.

---

## 8. A supported way to drive the tab for a screenshot

Checking that a change to a plot actually looks right means opening two
cases, pressing Compare, switching to a view, setting a control and
looking — and there is no way to ask the program to do that. The
workaround is to add a temporary block to `FlowGuiWindow`'s constructor
that reads a few environment variables and fires timed lambdas at the
widgets, take the screenshots, then strip it out again.

That workaround was written, extended, stripped and rewritten several
times over one afternoon's work on the Compare tab, and it earns its
place here on the strength of what it caught: markers that were never
actually switched on because the code assumed `findChildren` returns
widgets in construction order, and it does not — the box being ticked
was the other one, already ticked, so the feature looked implemented and
had never once run. No amount of re-reading the diff would have found
that. Only driving it did.

Against that, every rebuild ships whatever is in the constructor, and the
block is `// TEMP` in a file that is otherwise released. It has to be
checked for in the binary before every commit, which is not a safe thing
to depend on remembering.

*What it might be:* small and honest rather than a test framework — a
switch, off unless asked for, that opens the named cases, selects a tab
and view, sets named controls, and exits. Enough to make the picture in a
commit message reproducible by whoever reads it.

*Where it would go:* `FlowGuiWindow`'s constructor already has
`openCaseEverywhere()` for the cases, and the tabs and views are ordinary
`QTabWidget`s. The lesson from the bug above is the design constraint:
address controls by what they *are* — the tooltip, the text, the
property they belong to — never by their position in a `findChildren`
list, which is not the order they were built in.
