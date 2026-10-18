# fwsim - simulated HDD firmware command processor on top of disk-sim
#
#   make                 build the library, the fwsim CLI and the benchmark
#   make test            build + run all unit/integration tests, for BOTH
#                        queue implementations (lock-free and mutex)
#   make asan            the same under AddressSanitizer + UBSan
#   make tsan            the same under ThreadSanitizer
#   make bench           run every experiment; writes results/results.csv,
#                        results/results.md and results/*.png
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
DISKSIM_DIR ?= $(firstword $(wildcard ../disk-sim ../Projects/disk-sim))

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
          src/workload.c src/powerloss.c
FW_OBJS = $(FW_SRCS:%.c=$(BUILD)/%.o)
FW_LIB  = $(BUILD)/libfwsim.a

TESTS     = test_crc test_ring test_cache test_dispatch test_integrity test_powerloss
TEST_BINS = $(TESTS:%=$(BUILD)/tests/%)
CLI       = $(BUILD)/fwsim
BENCH     = $(BUILD)/fwbench

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

.PHONY: all lib test asan tsan bench run-tests clean
# Keep object files that make would otherwise treat as intermediates.
.SECONDARY:

all: $(CLI) $(BENCH)

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

$(BUILD)/tests/%: $(BUILD)/tests/%.o $(FW_LIB) $(DS_LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

# Run the suite in the current (QUEUE, SAN) configuration, then a short
# end-to-end CLI run of each workload type so the sanitizers also see the
# real command paths (read-ahead, destage, FUA, write-through).
run-tests: $(TEST_BINS) $(CLI)
	@for t in $(TEST_BINS); do \
	    echo "== $$t"; $(RUN) ./$$t || exit 1; \
	done
	$(RUN) ./$(CLI) --quiet --workload randwrite --ops 400 --qd 8 --time-scale 0.002
	$(RUN) ./$(CLI) --quiet --workload mixed --ops 400 --qd 4 --region 65536 --time-scale 0.002
	$(RUN) ./$(CLI) --quiet --workload seqread --ops 200 --xfer 64 --time-scale 0.002
	$(RUN) ./$(CLI) --quiet --workload randwrite --ops 200 --write-through --time-scale 0.002
	$(RUN) ./$(CLI) --quiet --workload randwrite --ops 200 --fua --unsorted-destage --time-scale 0.002
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

clean:
	rm -rf build

-include $(FW_OBJS:.o=.d) $(DS_OBJS:.o=.d) $(BUILD)/src/main.d $(BUILD)/bench/bench.d \
         $(TEST_BINS:=.d)
