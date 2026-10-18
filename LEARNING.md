# LEARNING.md: a guided walkthrough of fwsim

This is the "explain it to me" companion to the README. Read it top to
bottom once. After that, use the 60-second and 5-minute scripts near the end
before an interview. Every number quoted here is from `results/results.md`
(one `make bench` run); if you re-run the benchmark, re-read the numbers from
there.

Contents:

1. [The real firmware concepts behind each module](#1-the-real-firmware-concepts)
2. [File-by-file, function-by-function tour](#2-file-by-file-tour)
3. [Concurrency, explained simply](#3-concurrency-explained-simply)
4. [Why each design choice, and the alternatives](#4-design-choices-and-alternatives)
5. [How fwsim and disk-sim are one system](#5-fwsim--disk-sim-one-system)
6. [The 60-second explanation](#6-the-60-second-explanation)
7. [The 5-minute explanation](#7-the-5-minute-explanation)
8. [Resume bullets](#8-resume-bullets)

---

## 1. The real firmware concepts

### What a drive's firmware actually does

A hard drive is a small computer. A controller SoC runs firmware that:
talks to the host over SATA/SAS (the **host interface**), decodes commands
(the **command layer**), manages a DRAM buffer (the **cache manager**),
translates LBAs to physical locations and handles defects (**translation /
defect management**), and drives the actuator and read channel (the **servo
and channel** layers). fwsim simulates the middle of that stack: command
queueing, command processing, caching, data integrity and power-loss
behaviour. disk-sim simulates the bottom: the mechanics.

### NCQ and tags → `fw_cmd_t.tag`, the tag bitmap

**Native Command Queuing** (SATA) lets the host have up to **32 commands
outstanding** at once, each identified by a 5-bit **tag**. The drive may
complete them *in any order*. That's the point: it can reorder them to
minimise seek and rotation (that reordering is what disk-sim's SPTF scheduler
measures). The tag is how a completion is matched to its command. Reusing a
tag that is still outstanding is a protocol error. In fwsim, `tag` is 0–31,
`fw_submit` claims it atomically in a 32-bit bitmap, and a reused tag is
rejected with `TAG_IN_USE`. That's sense ABORTED COMMAND / 4Eh, "overlapped
commands attempted", the real code.

### Submission and completion queues → `ring_spsc.c`, `ring_mutex.c`

NVMe makes this explicit: a **submission queue** in host memory that the host
writes and the drive reads, and a **completion queue** going the other way.
Each queue is a ring buffer with one writer per index, which is exactly what
a single-producer/single-consumer lock-free ring is. AHCI/NCQ has the same
shape with command slots and a completion bitmap. fwsim uses one SPSC ring
each way.

### Command lifecycle → `fw_cmd_state_t`

Every command goes **queued → active → completed**: the host queues it, the
firmware picks it up, and the firmware posts its completion. Real firmware
tracks the same states per tag so it can handle aborts, timeouts and resets.

### Write caching (write-back vs write-through) → `cache.c`, `h_write`

- **Write-through**: data goes to the platters, *then* the drive says
  "done". This is slow (a seek and a rotation per write) but always safe.
- **Write-back** (ATA "write cache enabled", WCE): data goes into DRAM, the
  drive says "done" immediately, and the platters are updated later. That
  later step is **destaging**. It's fast, but the DRAM is volatile.

`SET FEATURES` 02h/82h is the ATA command that turns the write cache on or
off (`hdparm -W1/-W0`). fwsim implements it as `FW_OP_SET_FEATURES` with
`FW_FEAT_WRITE_CACHE`. Turning the cache off first destages everything
that's dirty.

### FUA and FLUSH → `FW_CMD_FUA`, `h_flush`

The host's tools for durability on a write-back drive:

- **FUA** (Force Unit Access): a flag on one write meaning "this one must be
  on the media before you ack it".
- **FLUSH CACHE**: "everything you've acknowledged so far must be on the
  media before you complete this command".

Filesystems and databases issue these at commit points, which is why a
write-back drive is safe for a journaling filesystem but *unacknowledged*
writes and *unflushed* writes are fair game on power loss.

### Destaging → `destage()`, `plan_start()`, `plan_step()`

Destaging means writing dirty cache data back to the media. The real design
questions are when, which data, and in what order:

- **When**: on a high watermark (with hysteresis down to a low one), on
  FLUSH, and when idle.
- **Which**: the oldest dirty data, because recently written data is likely
  to be rewritten.
- **In what order**: sorted by LBA, which approximates sorted by position,
  an elevator sweep. Adjacent sectors are coalesced into one media write.
- **How it interleaves with host I/O**: in small slices, so host commands
  aren't stuck behind a huge destage.

fwsim does all of these, and the benchmark shows each one's effect.

### Read-ahead → `read_ahead()`, `media_read_ahead()`

When the host reads sequentially, the drive keeps reading past the request
into its buffer. The next request then hits the cache, and more importantly
the drive avoids losing a full revolution waiting for the next sector to come
around again. The benchmark shows sequential 64 KiB reads at QD 1 going from
108 to 743 IOPS.

### CRC vs ECC → `crc32.c`, `media.c`

- **ECC** (error-correcting code; LDPC in modern drives, Reed-Solomon before)
  lives in the read channel and *corrects* most raw bit errors on the platter.
- **CRC** (and T10 protection information) is an end-to-end *detection*
  check, often seeded with the LBA, that catches anything ECC missed or that
  happened in buffers, buses or firmware bugs.

When even ECC fails, the drive **retries** the read on later revolutions,
sometimes with different channel parameters. If that still fails, it reports
**MEDIUM ERROR / unrecovered read error** with the failing LBA, and marks the
sector **pending reallocation**. The next write to it gets remapped to a spare
sector, the **G-list** (grown defect list). fwsim models detection + retry +
MEDIUM ERROR + pending + reallocation; ECC correction is future work.

### Power-loss protection

A consumer HDD's cache is volatile: power loss destroys whatever wasn't
destaged. Enterprise drives may use stored energy (capacitors, or platter
spindown energy) to save the cache. That's **power-loss protection (PLP)**,
and drives that have it can report the cache as non-volatile. fwsim models
the no-PLP drive and measures exactly what is lost under each host policy.

### Torn writes

If power fails in the middle of a multi-sector write, some sectors are new
and the rest old. Sector writes themselves are atomic. That's why
filesystems need journaling or checksums. fwsim's media layer produces torn
writes at sector granularity, and a test checks the result is a clean prefix.

---

## 2. File-by-file tour

Read in this order: `fwsim.h` → `firmware.c` → `cache.c` → `media.c` →
`ring_spsc.h/.c` → the tests.

### `src/fwsim.h`: the public API

- `fw_cmd_t`: the command. Host-filled fields (`opcode`, `flags`, `tag`,
  `lba`, `count`, `buf`, `feature*`) and firmware-filled fields (`status`,
  `sense`, `err_lba`, `state`, three timestamps). The comment at the top
  explains **ownership**: who may touch the struct when.
- `fw_status_t` + `fw_sense_t`: status codes that map to real SCSI sense
  key/ASC/ASCQ.
- `fw_config_t`: everything tunable, including cache size, watermarks,
  destage order and slice size, read-ahead length, retries, fault rate and
  `time_scale`.
- `fw_stats_t`: the counters every benchmark reads.
- Functions: `fw_create`, `fw_submit`, `fw_poll`, `fw_exec`, `fw_shutdown`,
  `fw_power_cut`, `fw_destroy`, `fw_get_stats`, `fw_inject_media_bitflip`.

### `src/firmware.c`: the firmware

- `struct fw`: the shared parts (queues, `power_on`, `stop_req`,
  `tags_busy`, counters) and the firmware-only parts (cache, media,
  `wce`, read-ahead state, destage plan).
- `fw_config_default()`: the defaults, with the reasoning in comments.
- `check_range()`: argument validation. It's overflow-safe:
  `count > cap - lba`, never `lba + count > cap`.
- **Destaging**:
  - `write_run()` writes one run of adjacent dirty sectors as one media
    write, then marks them clean.
  - `destage(max)` is the synchronous version: collect the oldest dirty,
    sort, coalesce, write.
  - `destage_all()` does it for FLUSH, shutdown and turning WCE off.
  - `plan_start()` / `plan_step()` are the background version: a sorted
    list of LBAs executed a few runs at a time. It re-checks every LBA,
    because the entry may have changed since.
- **Handlers**, the dispatch targets:
  - `h_identify` fills `fw_identify_t` into one sector.
  - `h_read` serves hits from the cache (including dirty entries, for
    coherence), turns each run of misses into one media read, and caches
    what it read.
  - `h_write` handles write-through/FUA (media first, then a clean cache
    copy that supersedes any dirty one) and write-back (dirty insert, with a
    forced destage if every entry is dirty).
  - `h_flush` and `h_set_features`.
- `dispatch[]`: the opcode → handler table.
- `read_ahead()`: sequential detection and prefetch after the completion
  is posted.
- `threshold_destage()`: the watermark check that starts a plan.
- `complete()`: fills status and sense, frees the tag *before* posting, and
  pushes to the completion queue. After the push, the struct belongs to the
  host.
- `process()`: one command, end to end: ACTIVE → dispatch → (if power is
  still on) complete → background work.
- `idle_wait_ns()` / `fw_main()`: the loop. Pop with a timeout, process,
  otherwise do a destage slice or start an idle destage. Also handles
  shutdown and power loss.
- `fw_submit()`: tag claim with `fetch_or`, state = QUEUED, push.
- `fw_poll()` and `fw_exec()`.
- `fw_shutdown()` is a clean stop (destages everything). `fw_power_cut()`
  sets the flag, wakes the thread and joins it, with no flush.
  `fw_destroy()` frees everything.

### `src/cache.c`: the LRU cache

- `bucket_of()`: Fibonacci hashing of the LBA.
- `list_unlink()` / `list_push_front()`: O(1) LRU moves around a sentinel.
- `hash_insert()` / `hash_remove()`: chained buckets.
- `cache_init()`: a preallocated pool, slab and free list.
- `cache_lookup(lba, touch)`: `touch=false` is for lookups that must not
  disturb LRU order (read-ahead, tests).
- `find_victim()`: the LRU *clean* entry.
- `cache_insert()`: insert or overwrite, with dirty bookkeeping.
- `cache_collect_dirty()`: oldest first.
- `cache_discard_all()`: what power loss does.
- `cache_check()`: verifies every invariant; the tests call it.

### `src/media.c`: the platters

- The record format: `data[512] + crc32(data || lba)`. An all-zero record
  means "never written".
- `sim_now()`: the simulated clock passed to disk-sim. It follows the wall
  clock when time is being played back.
- `mechanics()`: asks disk-sim how long the access takes, plays it back
  with `delay_until()`, and reports how far it got if power was cut.
- `media_read()` / `media_read_ahead()` → `do_read()`: pread the records,
  inject transient flips, verify CRC, retry once per revolution, then
  MEDIUM_ERROR + pending.
- `media_write()`: reallocates pending LBAs (disk-sim G-list), runs the
  mechanics, writes the records. On a cut, it writes only the prefix that
  passed under the head (a torn write).
- `media_inject_bitflip()`: a persistent media defect.

### `src/ring_spsc.h/.c` and `src/ring_mutex.h/.c`: the queues

The SPSC header carries the correctness proof; read it twice. The mutex
ring is the textbook bounded buffer with two condvars, and `rbm_close()`
wakes everyone. `squeue.h` picks one at build time.

### The smaller pieces

- `src/clock.c`: `now_ns()` (CLOCK_MONOTONIC) and `delay_until()` (sleep,
  then spin; interruptible).
- `src/crc32.c`: the table-driven CRC-32 built under `pthread_once`, plus a
  bitwise reference used by the tests.
- `src/status.c`: the status → sense table.
- `src/util.h`: splitmix64 PRNG and single-writer counters.
- `src/latency.c`: percentile summaries (nearest rank).
- `src/pattern.c`: self-describing sector contents
  (magic, version, LBA, PRNG fill).
- `src/workload.c`: the closed-loop host at QD *n*.
- `src/powerloss.c`: one power-loss trial (write → cut → reboot → verify,
  with the FLUSH snapshot logic).
- `src/main.c`: the `fwsim` CLI.
- `bench/bench.c`: all experiments → CSV. `bench/plot.py`: CSV →
  markdown + PNG, and it refreshes the README.

---

## 3. Concurrency explained simply

### Race conditions

A **data race** is when two threads access the same memory at the same
time, at least one of them writes, and nothing orders the accesses. The
classic example is `counter++` from two threads. It's really "load,
add, store", so two threads can both load 5 and both store 6, and one
increment is lost. In C, a data race is **undefined behaviour**: the
compiler is allowed to assume it never happens, so anything can result.

fwsim's defence is to **share as little as possible**. The cache and media
are touched by one thread only. What *is* shared has a protocol:

| Shared thing | Protection |
|---|---|
| submission / completion queues | the ring's own synchronisation |
| `fw_cmd_t` contents | ownership transfer through the queues |
| `power_on`, `stop_req` | `atomic_int`, release on store, acquire on load |
| tag bitmap | atomic `fetch_or` / `fetch_and` |
| statistics | single-writer relaxed atomics |

### Mutex vs condition variable

- A **mutex** is a lock: one thread at a time inside. It answers "how do we
  not trample each other?"
- A **condition variable** is a waiting room attached to a mutex. It
  answers "how do I sleep until something changes, without busy-waiting and
  without missing the wake-up?" `pthread_cond_wait` atomically *releases the
  mutex and sleeps*, then re-acquires the mutex when woken. Always call it in
  a `while (!condition)` loop: wake-ups can be spurious, and someone else may
  have consumed the item first.

The mutex ring has two condvars (`not_empty`, `not_full`) because producers
and consumers wait for different things.

### Memory ordering, in plain words

Modern CPUs and compilers reorder memory operations when they can prove a
*single* thread can't tell the difference. Another thread *can* tell.
Example: the producer writes the slot, then bumps `tail`. If those two
become visible in the opposite order, the consumer sees the new tail and
reads a stale slot.

C11 atomics let you say which orderings you need:

- **release** (on a store): "everything I wrote before this store is visible
  to anyone who sees this store."
- **acquire** (on a load): "if I see that store, I also see everything
  before it."
- **relaxed**: "just make this one access atomic, no ordering promises."
  Fine for statistics.
- **seq_cst** (the default): a single global order of all such operations.
  It's the strongest and sometimes the most expensive. fwsim doesn't need it.

A release store paired with an acquire load of the *same variable* is a
**synchronises-with** edge. The SPSC ring needs exactly two of them: data →
tail (so the consumer sees the item) and slot read → head (so the producer
doesn't overwrite too early).

### Why `volatile` isn't enough

`volatile` means "don't optimise away or cache this access". It was made for
memory-mapped hardware registers. It does **not**:

- make `x++` atomic;
- prevent the compiler from moving *other* (non-volatile) accesses across
  it;
- emit the CPU fences needed on ARM, POWER or RISC-V;
- make a racing access defined behaviour in the C11 model.

The SPSC ring with `volatile` indices might pass tests on x86, which
reorders very little, and then break on an ARM controller. Firmware
engineers get asked this because real controllers are ARM cores.

### Lock-free vs blocking, as measured

| | mutex + condvar | lock-free SPSC |
|---|---:|---:|
| raw handoff (M items/s) | 0.69 | 18.25 |
| firmware QD 1 (K cmds/s) | 20.9 | 1,177 |
| firmware QD 32 (K cmds/s) | 1,038 | 2,418 |

The mutex queue's cost is mostly **sleeping and waking** (futex syscalls and
the scheduler), which is paid on every handoff when the queue keeps running
empty (QD 1). At QD 32 the queue is rarely empty, threads rarely sleep, and
the gap narrows to 2.3×. The lock-free version spins and yields instead of
sleeping, so it burns CPU while idle. That's the honest trade-off.

### How the tools prove it

- `make tsan`: ThreadSanitizer instruments every memory access and reports
  any pair of conflicting accesses not ordered by a happens-before edge.
  It's clean for both queue builds. (A deliberately racy program *is* caught
  in this environment, so the silence means something.)
- `make asan`: AddressSanitizer + UBSan catch out-of-bounds, use-after-free,
  leaks, misaligned accesses and signed overflow. Also clean.

---

## 4. Design choices and alternatives

| Choice | Why | Alternatives considered |
|---|---|---|
| One firmware thread | matches a single command-layer core; cache and media need no locks | worker pool + locked cache: contention, lock ordering, still one actuator |
| Pointer + ownership transfer through queues | the queue's release/acquire publishes the whole struct, so no per-command lock | copy commands into the ring (more copying; still need the buffer handoff); lock per command |
| Build-time queue switch | zero cost on the hot path; both still unit-tested | runtime vtable (indirect call in the measured path) |
| SPSC with free-running counters + cached indices | no CAS, no wasted slot, minimal cache-line traffic | MPMC queue (needs CAS, slower); `head == tail` + one wasted slot |
| Per-sector cache entries | exact LRU and dirty tracking, simple tests | segments (less metadata, closer to real drives; future work) |
| Hash map + doubly linked LRU, preallocated | O(1) everything, no malloc on the I/O path | CLOCK/second-chance (cheaper touch, approximate LRU); ARC (adaptive, more state) |
| Evict LRU *clean* only | dirty data exists nowhere else | separate clean/dirty lists: O(1) eviction, but loses one global age order |
| 75%/50% watermarks | batches big enough to sort; no one-sector destages | single threshold (thrashes); time-based destage only |
| Sorted, coalesced destage | 2.1× vs age order on random writes | age order (simpler, measured worse); full SPTF (needs rotational estimates) |
| Destage in slices of 2 writes | bounded host wait | one big synchronous batch (measured: terrible tail latency) |
| Streaming read-ahead | models the read channel not stopping; 6.9× on sequential reads | prefetch as a fresh access (pays a revolution each time, no gain) |
| CRC-32 over data + LBA | detects bit flips *and* misdirected I/O | data-only CRC (misses misdirection); stronger CRC-64; real ECC |
| All-zero record = unwritten | sparse file, no format pass, still detects flips | format the whole surface at create (slow for 100 GB) |
| Reallocate on next write | never silently replace host data | reallocate on read failure (data would be invented) |
| Real-time playback with `time_scale` | genuine concurrency; power cut can land mid-seek | pure virtual clock (deterministic, but no real host/firmware overlap) |

---

## 5. fwsim + disk-sim: one system

Present them as **two layers of one simulated drive**:

```
 host workload ─► fwsim (controller firmware) ─► disk-sim (head + platters)
                  queues, tags, cache, CRC,       zones, seek curve, rotation,
                  destage, FUA/FLUSH, power loss  skew, defects, G-list
```

- disk-sim's public header, `disksim.h`, is the **media API**. fwsim calls
  `disk_service_request()` for every media access and plays back the
  `total_ms` it returns, uses `disk_get_info()` for capacity, sector size and
  revolution time (one retry costs one revolution), and calls
  `disk_add_grown_defect()` to reallocate a sector that failed CRC. That's
  disk-sim's own G-list. No change to disk-sim was needed.
- **The two projects agree with each other.** disk-sim measured FCFS random
  4K at about 78 IOPS; fwsim, which serves commands in FIFO order on the same
  drive model, measured 73 IOPS for write-through random writes. disk-sim
  showed that *reordering* requests (SPTF: 184 IOPS at QD 32) is where
  mechanical throughput comes from. fwsim's LBA-sorted destage is the
  write-cache version of the same idea (2.1× over age order), and SPTF
  reordering of host commands is fwsim's top future-work item.
- The story in one line: *disk-sim answers "how long does this access take
  and why"; fwsim answers "what should the firmware do with 32 queued
  commands and 2 MiB of RAM, and what does the host lose when the power
  goes?"*

---

## 6. The 60-second explanation

> "fwsim simulates the firmware inside a hard drive. A host thread submits
> NCQ-style tagged commands through a submission queue; a firmware thread
> dispatches them by opcode, serves them from an LRU write-back cache, and
> writes to simulated platters. The platters' timing comes from my other
> project, disk-sim, and every sector carries a CRC.
>
> I built two queues, a mutex/condvar ring and a lock-free single-producer
> ring with C11 acquire/release. The lock-free one was 56 times faster at
> queue depth 1 in the real firmware.
>
> The main experiment is power loss. I cut power at a random moment and
> reboot. Write-back without FLUSH lost acknowledged writes in all 100
> trials, about 350 per trial. FLUSH limits the loss to the last flush
> window, and FUA or write-through lose nothing, at around an 8× throughput
> cost. Everything passes ThreadSanitizer and AddressSanitizer."

## 7. The 5-minute explanation

1. **Framing (30 s).** "A drive is a computer: host interface, command
   layer, cache, media. disk-sim was the media. fwsim is the firmware
   between the host and the media. It's simulated, but the semantics are
   the real ones: NCQ tags, FUA, FLUSH, sense codes."
2. **The command path (60 s).** The host claims a tag in an atomic bitmap
   and pushes a pointer. The firmware pops it, marks it ACTIVE, and calls
   `dispatch[opcode]`. A write in write-back mode goes into the hash-map +
   linked-list LRU cache as dirty and is acknowledged at once. The
   completion goes back on the completion queue with SCSI sense data. Then
   the background work runs: read-ahead after sequential reads, destage
   after writes.
3. **Concurrency (60 s).** One firmware thread owns everything stateful, so
   only the queues, flags, tag bitmap and counters are shared. The SPSC
   ring's two release/acquire pairs, data-before-index and
   read-before-reuse. Why `volatile` isn't enough. Mutex vs lock-free:
   26× raw, 56× at QD 1, 2.3× at QD 32, and why the gap shrinks. TSan and
   ASan are clean.
4. **Caching results (60 s).** On sustained random writes, write-back was
   only 2× write-through including the final flush, because the destage
   still has to happen. The 2× comes from LBA-sorted destaging, which by
   itself was 2.1× age order. The tail-latency trap: p99 was *worse* with
   write-back on a 1 GiB region, 56 vs 36 ms. Read-ahead gave 6.9× on
   sequential reads, because it avoids a missed revolution per command, not
   just because of hits.
5. **Durability (60 s).** The checker separates "lost" (allowed) from
   "violation" (a bug), using a snapshot taken when FLUSH is issued. Results
   from 100 trials per mode: 351 lost per trial without flush; 18.5 with a
   flush every 32 writes, never more than one interval; 0 with FUA or
   write-through; 0 violations anywhere. Torn writes come out as a clean
   prefix.
6. **Integrity (30 s).** CRC-32 seeded with the LBA, retries on later
   revolutions, MEDIUM ERROR with the exact LBA, a pending list, and
   reallocation through disk-sim's G-list on the next write. 20/20 transient
   flips recovered, 25/25 persistent flips reported, 0 silent corruptions.
7. **What I'd do next (30 s).** SPTF reordering of host commands,
   read-priority destage, an ECC model, power-loss-protection energy
   budgeting.

---

## 8. Resume bullets

Pick one or two. All the numbers are from `make bench`.

- **Built a simulated HDD firmware command processor in C11/pthreads**
  (NCQ-style 32-tag queue, opcode dispatch table, LRU write-back cache with
  LBA-sorted destaging, CRC-32 per sector with retry and MEDIUM ERROR
  reporting) on top of my own disk-mechanics simulator. It is clean under
  ThreadSanitizer and AddressSanitizer.
- **Designed a power-loss experiment** that cuts power at random moments and
  verifies acknowledged writes after reboot, over 400 trials. Write-back
  caching lost **351 acknowledged writes per trial**, FLUSH bounded the loss
  to one flush window, and FUA/write-through lost **0**, at a measured
  **7.9× throughput cost**. There were zero durability-contract violations.
- **Implemented a lock-free SPSC ring with C11 acquire/release atomics** for
  the host↔firmware queues. It was **56× higher command throughput than a
  mutex/condvar queue at queue depth 1** (1.18 M vs 21 K cmds/s), with a
  written correctness argument and stress tests.
- **Optimised write-back destaging and read-ahead using measurements**:
  LBA-sorted, coalesced destaging **doubled random-write throughput (2.1×)**
  and halved p99 latency versus age order, and streaming read-ahead raised
  sequential-read throughput **6.9×**.
