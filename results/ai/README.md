# AI-generated reports

All reports in this folder are AI-generated. They contain research notes, benchmark results, negative
experiments, and implementation details. They are working notes, not peer-reviewed papers; numbers
and conclusions are specific to the hardware, compiler, and commit stated in each report.

Several reports were produced with multiple AI agents running benchmarks in parallel on the same
host. Mentions of other agents in a report describe that shared-host contention and are the reason
the reports rely on paired A/B measurements instead of absolute rates. This is a small selection of
the much larger set of agent and subagent reports generated during the work.

Some reports refer to local investigation paths (`tmp/...`, `worktrees/...`) and to branch names
that are not part of this repository. Those paths and branches are kept for reproducibility of the
notes; the measurements and conclusions do not depend on them.
