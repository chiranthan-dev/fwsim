# Pre-Edge-AI baseline

Saved before any Edge AI change, from a clean build of commit `acac711`
(see `COMMIT`), with the same commands `make bench` runs:

    ./build/lockfree/fwbench --csv results/baseline/results.csv --all
    ./build/mutex/fwbench --csv results/baseline/results.csv --append --only queue_e2e

- `results.csv`, `powerloss_trials.csv`: the benchmark output
- `bench_stdout.txt`: its console output (plus wall time)
- `make_test.txt`: `make test` results at that commit

Most bench metrics are timed on the wall clock at time_scale 0.25, so they
vary from run to run even with identical code; `docs/baseline-check.md`
compares the post-change build against this baseline accordingly.
