# fwsim - simulated HDD firmware command processor on top of disk-sim
#
#   make                 build the library, the fwsim CLI and the benchmark
#   make test            build + run all unit/integration tests, for BOTH
#                        queue implementations (lock-free and mutex)
#   make asan            the same under AddressSanitizer + UBSan
#   make tsan            the same under ThreadSanitizer
#   make bench           run every experiment; writes results/results.csv,
#                        results/results.md and results/*.png
#   make bench-smart     SMART failure prediction end to end: train on the
#                        Backblaze data in SMART_DATA, export the model,
#                        check C vs Python, time inference, run the demo;
#                        writes results/smart/ (see tools/smart/README.md)
#   make smart-demo      only the firmware demo (no data needed)
#   make QUEUE=mutex     build with the mutex/condvar queue instead of the
#                        lock-free SPSC ring (default QUEUE=lockfree)
#   make clean
#
# Each (queue, sanitizer) combination builds into its own directory, e.g.
# build/lockfree, build/mutex-tsan, so instrumented and plain objects are
# never mixed.
#
# disk-sim: we compile its library sources ourselves (everything in
# $(DISKSIM_DIR)/src except main.c, which is exactly its libdisksim.a) into
# $(BUILD)/libdisksim.a, with OUR sanitizer flags. WHY not link its
# prebuilt build/libdisksim.a: under TSan/ASan the media layer must be
# instrumented too, or races and overflows inside it go unseen.

ifeq ($(origin CC),default)
  CC = gcc
endif
QUEUE       ?= lockfree
SAN         ?=
PYTHON      ?= python3
# The SMART pipeline needs pandas/scikit-learn/duckdb (tools/smart/requirements.txt).
SMART_PY    ?= $(if $(wildcard tools/smart/.venv/bin/python),tools/smart/.venv/bin/python,$(PYTHON))
SMART_DATA  ?= tools/smart/data
# Prefer a live sibling checkout of disk-sim; fall back to the snapshot of
# its library sources vendored in third_party/ so a fresh clone builds.
DISKSIM_DIR ?= $(firstword $(wildcard ../disk-sim ../Projects/disk-sim third_party/disk-sim))

ifeq ($(DISKSIM_DIR),)
  $(error disk-sim not found: pass DISKSIM_DIR=/path/to/disk-sim)
endif

WARN = -Wall -Wextra -Werror -pedantic -Wshadow -Wstrict-prototypes \
       -Wmissing-prototypes -Wpointer-arith -Wformat=2

ifeq ($(SAN),asan)
  OPT  = -O1 -g -fno-omit-frame-pointer
  SANF = -fsanitize=address,undefined -fno-sanitize-recover=all
else ifeq ($(SAN),tsan)
  OPT  = -O1 -g -fno-omit-frame-pointer
  SANF = -fsanitize=thread
else ifeq ($(SAN),)
  OPT  = -O2 -g
  SANF =
else
  $(error SAN must be empty, asan or tsan)
endif

ifeq ($(QUEUE),mutex)
  QDEF = -DFWSIM_QUEUE_MUTEX
else ifeq ($(QUEUE),lockfree)
  QDEF =
else
  $(error QUEUE must be lockfree or mutex)
endif

BUILD    = build/$(QUEUE)$(if $(SAN),-$(SAN))
CFLAGS   = -std=c11 $(WARN) $(OPT) $(SANF) -pthread -MMD -MP
CPPFLAGS = -D_POSIX_C_SOURCE=200809L $(QDEF) -Isrc -I$(DISKSIM_DIR)/include
LDFLAGS  = $(SANF) -pthread
LDLIBS   = -lm

# --- disk-sim --------------------------------------------------------------
DS_SRCS = $(filter-out $(DISKSIM_DIR)/src/main.c,$(wildcard $(DISKSIM_DIR)/src/*.c))
DS_OBJS = $(patsubst $(DISKSIM_DIR)/src/%.c,$(BUILD)/disksim/%.o,$(DS_SRCS))
DS_LIB  = $(BUILD)/libdisksim.a

# --- fwsim -----------------------------------------------------------------
FW_SRCS = src/clock.c src/crc32.c src/status.c src/ring_mutex.c src/ring_spsc.c \
          src/cache.c src/media.c src/firmware.c src/latency.c src/pattern.c \
          src/workload.c src/powerloss.c src/smart.c
FW_OBJS = $(FW_SRCS:%.c=$(BUILD)/%.o)
FW_LIB  = $(BUILD)/libfwsim.a

TESTS     = test_crc test_ring test_cache test_dispatch test_integrity test_powerloss test_smart
TEST_BINS = $(TESTS:%=$(BUILD)/tests/%)
CLI       = $(BUILD)/fwsim
BENCH     = $(BUILD)/fwbench
UI        = $(BUILD)/fwsim-ui
SMART     = $(BUILD)/fwsim-smart
SMART_DEMO = $(BUILD)/fwsim-smart-demo
UI_PORT  ?= 8080

# Sanitizer runtime options: fail loudly on the first report.
export ASAN_OPTIONS  = detect_leaks=1:abort_on_error=1:halt_on_error=1
export UBSAN_OPTIONS = print_stacktrace=1:halt_on_error=1
export TSAN_OPTIONS  = halt_on_error=1:second_deadlock_stack=1

# TSan maps shadow memory at fixed addresses; newer kernels' high mmap ASLR
# entropy can collide with it ("unexpected memory mapping"). Running the
# binary with ASLR disabled sidesteps that without touching sysctls.
ifeq ($(SAN),tsan)
  RUN = setarch $(shell uname -m) -R
else
  RUN =
endif

.PHONY: all lib test asan tsan bench bench-smart smart-demo run-tests clean ui run-ui
# Keep object files that make would otherwise treat as intermediates.
.SECONDARY:

all: $(CLI) $(BENCH) $(UI) $(SMART) $(SMART_DEMO)

ui: $(UI)

# The live web dashboard: open http://localhost:$(UI_PORT) in a browser.
run-ui: $(UI)
	./$(UI) --port $(UI_PORT) --static ui/static

lib: $(FW_LIB) $(DS_LIB)

$(BUILD)/disksim/%.o: $(DISKSIM_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(WARN) $(OPT) $(SANF) -MMD -MP -I$(DISKSIM_DIR)/include -I$(DISKSIM_DIR)/src -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(DS_LIB): $(DS_OBJS)
	ar rcs $@ $^

$(FW_LIB): $(FW_OBJS)
	ar rcs $@ $^

$(CLI): $(BUILD)/src/main.o $(FW_LIB) $(DS_LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BENCH): $(BUILD)/bench/bench.o $(FW_LIB) $(DS_LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(UI): $(BUILD)/ui/server.o $(FW_LIB) $(DS_LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(SMART): $(BUILD)/bench/smart_bench.o $(FW_LIB) $(DS_LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(SMART_DEMO): $(BUILD)/bench/smart_demo.o $(FW_LIB) $(DS_LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/tests/%: $(BUILD)/tests/%.o $(FW_LIB) $(DS_LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

# Run the suite in the current (QUEUE, SAN) configuration, then a short
# end-to-end CLI run of each workload type so the sanitizers also see the
# real command paths (read-ahead, destage, FUA, write-through).
run-tests: $(TEST_BINS) $(CLI) $(SMART_DEMO)
	@for t in $(TEST_BINS); do \
	    echo "== $$t"; $(RUN) ./$$t || exit 1; \
	done
	$(RUN) ./$(CLI) --quiet --workload randwrite --ops 400 --qd 8 --time-scale 0.002
	$(RUN) ./$(CLI) --quiet --workload mixed --ops 400 --qd 4 --region 65536 --time-scale 0.002
	$(RUN) ./$(CLI) --quiet --workload seqread --ops 200 --xfer 64 --time-scale 0.002
	$(RUN) ./$(CLI) --quiet --workload randwrite --ops 200 --write-through --time-scale 0.002
	$(RUN) ./$(CLI) --quiet --workload randwrite --ops 200 --fua --unsorted-destage --time-scale 0.002
	$(RUN) ./$(SMART_DEMO) --days 28 --quiet
	@echo "run-tests: QUEUE=$(QUEUE) SAN=$(if $(SAN),$(SAN),none): all passed"

test:
	$(MAKE) --no-print-directory QUEUE=lockfree SAN=$(SAN) run-tests
	$(MAKE) --no-print-directory QUEUE=mutex SAN=$(SAN) run-tests

asan:
	$(MAKE) --no-print-directory test SAN=asan

tsan:
	$(MAKE) --no-print-directory test SAN=tsan

# The benchmark binary is built twice (one per queue implementation) so the
# end-to-end queue comparison runs the real firmware with each queue.
bench:
	$(MAKE) --no-print-directory QUEUE=lockfree SAN= all
	$(MAKE) --no-print-directory QUEUE=mutex SAN= all
	@mkdir -p results
	./build/lockfree/fwbench --csv results/results.csv --all
	./build/mutex/fwbench --csv results/results.csv --append --only queue_e2e
	$(PYTHON) bench/plot.py results

# SMART failure prediction. The Python steps are skipped when their output
# is newer than their input (ingest/features take minutes on two quarters).
SMART_OUT = results/smart
smart-demo: $(SMART_DEMO)
	@mkdir -p $(SMART_OUT)
	./$(SMART_DEMO) --csv $(SMART_OUT)/demo_timeline.csv
	$(SMART_PY) tools/smart/plot_demo.py $(SMART_OUT)/demo_timeline.csv $(SMART_OUT)/demo_timeline.png

bench-smart:
	@test -f $(SMART_DATA)/data_Q1_2026.zip || { echo "no Backblaze data in $(SMART_DATA): run tools/smart/download.sh $(SMART_DATA)"; exit 1; }
	@mkdir -p $(SMART_OUT)
	$(SMART_PY) -I tools/smart/ingest.py $(SMART_DATA)
	test $(SMART_DATA)/features.parquet -nt $(SMART_DATA)/daily || $(SMART_PY) -I tools/smart/features.py $(SMART_DATA)
	$(SMART_PY) -I tools/smart/train.py $(SMART_DATA) $(SMART_OUT)
	$(SMART_PY) -I tools/smart/export.py $(SMART_OUT)/model_deploy.json $(SMART_OUT)/vectors.csv src/smart_model.h tests/smart_vectors.h
	$(MAKE) --no-print-directory QUEUE=lockfree SAN= all build/lockfree/tests/test_smart
	./build/lockfree/tests/test_smart
	$(SMART_PY) -I tools/smart/agree.py $(SMART_DATA) $(SMART_OUT) ./build/lockfree/fwsim-smart
	./build/lockfree/fwsim-smart bench --csv $(SMART_OUT)/inference.csv
	$(MAKE) --no-print-directory QUEUE=lockfree SAN= smart-demo
	$(SMART_PY) -I tools/smart/report.py $(SMART_OUT)

clean:
	rm -rf build

-include $(FW_OBJS:.o=.d) $(DS_OBJS:.o=.d) $(BUILD)/src/main.d $(BUILD)/bench/bench.d $(BUILD)/ui/server.d \
         $(BUILD)/bench/smart_bench.d $(BUILD)/bench/smart_demo.d \
         $(TEST_BINS:=.d)
