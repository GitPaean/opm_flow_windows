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
