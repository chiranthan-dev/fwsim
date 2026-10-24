# `make bench` before and after the Edge AI feature

The feature is off by default, so `make bench` must not change. Because most of its metrics are wall-clock measurements (time_scale 0.25 on a shared WSL2 VM) that vary between runs of identical code, the same commands as `make bench` were run with the pre-feature binaries (commit acac711) and the post-feature binaries, back to back:

- OLD: the baseline saved before any change (`results/baseline/`), a rerun of the same binaries, and for the experiments that looked suspicious a third run
- NEW: the post-feature binaries, each run directly after an OLD run

A metric is *identical* when every NEW value equals the OLD rerun; *within noise* when the NEW values' range overlaps the OLD values' range widened by max(10% of the OLD mean, 0.5); otherwise *outside noise*.

**248 metrics: 96 identical, 146 within run-to-run noise, 6 outside.** Deterministic metrics (op counts, errors, durability violations, the CRC check value, the time-scale-0 integrity experiment): **50 of 50 identical.**

Outside noise: queue_micro/lockfree_spsc/handoff_rate_median, queue_micro/lockfree_spsc/handoff_rate_min, queue_micro/lockfree_spsc/handoff_rate_max, queue_e2e/lockfree/qd1/cmd_rate, queue_e2e/lockfree/qd1/lat_p99, queue_e2e/lockfree/qd32/lat_p99

| experiment | variant | metric | OLD runs | NEW runs | verdict |
|---|---|---|---|---|---|
| meta | config | time_scale | 0.25 / 0.25 / 0.25 | 0.25 / 0.25 | identical |
| meta | config | cache_entries | 4096 / 4096 / 4096 | 4096 / 4096 | identical |
| meta | config | quick | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randwrite_full/write_through | ops | 1500 / 1500 / 1500 | 1500 / 1500 | identical |
| cache_mode | randwrite_full/write_through | errors | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randwrite_full/write_through | iops | 74.8894 / 73.4231 / 72.942 | 73.0997 / 73.154 | within noise |
| cache_mode | randwrite_full/write_through | iops_durable | 74.8894 / 73.423 / 72.942 | 73.0997 / 73.154 | within noise |
| cache_mode | randwrite_full/write_through | mbps | 0.306747 / 0.300741 / 0.29877 | 0.299416 / 0.299639 | within noise |
| cache_mode | randwrite_full/write_through | lat_avg | 53.3519 / 54.4171 / 54.7755 | 54.6555 / 54.6166 | within noise |
| cache_mode | randwrite_full/write_through | lat_p50 | 52.9315 / 54.0386 / 54.3694 | 54.343 / 54.1412 | within noise |
| cache_mode | randwrite_full/write_through | lat_p95 | 67.908 / 69.4896 / 69.6444 | 69.3721 / 70.1082 | within noise |
| cache_mode | randwrite_full/write_through | lat_p99 | 74.8084 / 75.1297 / 76.4413 | 75.0332 / 78.0981 | within noise |
| cache_mode | randwrite_full/write_through | write_lat_p99 | 74.8084 / 75.1297 / 76.4413 | 75.0332 / 78.0981 | within noise |
| cache_mode | randwrite_full/write_through | media_busy | 19722.9 / 19664.2 / 19725.8 | 19614.4 / 19714.8 | within noise |
| cache_mode | randwrite_full/write_through | destage_runs | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randwrite_full/write_through | forced_destages | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randwrite_full/write_back | ops | 1500 / 1500 / 1500 | 1500 / 1500 | identical |
| cache_mode | randwrite_full/write_back | errors | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randwrite_full/write_back | iops | 199.126 / 193.156 / 189.768 | 188.529 / 192.013 | within noise |
| cache_mode | randwrite_full/write_back | iops_durable | 156.944 / 151.547 / 148.831 | 147.936 / 151.216 | within noise |
| cache_mode | randwrite_full/write_back | mbps | 0.81562 / 0.791167 / 0.777292 | 0.772215 / 0.786487 | within noise |
| cache_mode | randwrite_full/write_back | lat_avg | 20.0868 / 20.707 / 21.0762 | 21.2142 / 20.83 | within noise |
| cache_mode | randwrite_full/write_back | lat_p50 | 0.073908 / 0.033264 / 0.02968 | 0.032252 / 0.050724 | within noise |
| cache_mode | randwrite_full/write_back | lat_p95 | 59.1381 / 60.8209 / 63.451 | 63.3375 / 60.7832 | within noise |
| cache_mode | randwrite_full/write_back | lat_p99 | 64.971 / 67.2197 / 69.2516 | 70.6367 / 65.8779 | within noise |
| cache_mode | randwrite_full/write_back | write_lat_p99 | 64.971 / 67.2197 / 69.2516 | 70.6367 / 65.8779 | within noise |
| cache_mode | randwrite_full/write_back | media_busy | 9303.66 / 9259.28 / 9296.62 | 9355.15 / 9280.56 | within noise |
| cache_mode | randwrite_full/write_back | destage_runs | 1500 / 1500 / 1500 | 1500 / 1500 | identical |
| cache_mode | randwrite_full/write_back | forced_destages | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randwrite_1g/write_through | ops | 1500 / 1500 / 1500 | 1500 / 1500 | identical |
| cache_mode | randwrite_1g/write_through | errors | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randwrite_1g/write_through | iops | 169.673 / 163.974 / 161.951 | 159.58 / 165.175 | within noise |
| cache_mode | randwrite_1g/write_through | iops_durable | 169.673 / 163.974 / 161.951 | 159.58 / 165.175 | within noise |
| cache_mode | randwrite_1g/write_through | mbps | 0.694979 / 0.671637 / 0.663352 | 0.653641 / 0.676559 | within noise |
| cache_mode | randwrite_1g/write_through | lat_avg | 23.5534 / 24.3727 / 24.677 | 25.0433 / 24.1935 | within noise |
| cache_mode | randwrite_1g/write_through | lat_p50 | 23.424 / 24.0225 / 24.4154 | 24.911 / 23.9732 | within noise |
| cache_mode | randwrite_1g/write_through | lat_p95 | 32.19 / 33.0576 / 33.3248 | 33.829 / 32.9456 | within noise |
| cache_mode | randwrite_1g/write_through | lat_p99 | 34.7878 / 37.1924 / 38.017 | 38.6643 / 37.878 | within noise |
| cache_mode | randwrite_1g/write_through | write_lat_p99 | 34.7878 / 37.1924 / 38.017 | 38.6643 / 37.878 | within noise |
| cache_mode | randwrite_1g/write_through | media_busy | 8601.56 / 8622.55 / 8622.66 | 8525.57 / 8564.73 | within noise |
| cache_mode | randwrite_1g/write_through | destage_runs | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randwrite_1g/write_through | forced_destages | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randwrite_1g/write_back | ops | 1500 / 1500 / 1500 | 1500 / 1500 | identical |
| cache_mode | randwrite_1g/write_back | errors | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randwrite_1g/write_back | iops | 259.307 / 246.533 / 244.259 | 239.657 / 250.413 | within noise |
| cache_mode | randwrite_1g/write_back | iops_durable | 199.842 / 190.743 / 188.959 | 184.492 / 192.236 | within noise |
| cache_mode | randwrite_1g/write_back | mbps | 1.06212 / 1.0098 / 1.00049 | 0.981635 / 1.02569 | within noise |
| cache_mode | randwrite_1g/write_back | lat_avg | 15.4249 / 16.2236 / 16.3742 | 16.6874 / 15.9716 | within noise |
| cache_mode | randwrite_1g/write_back | lat_p50 | 0.01178 / 0.018972 / 0.016864 | 0.032392 / 0.02382 | within noise |
| cache_mode | randwrite_1g/write_back | lat_p95 | 48.0817 / 49.9237 / 50.705 | 51.8269 / 49.4851 | within noise |
| cache_mode | randwrite_1g/write_back | lat_p99 | 54.85 / 57.2527 / 61.5011 | 59.55 / 56.754 | within noise |
| cache_mode | randwrite_1g/write_back | write_lat_p99 | 54.85 / 57.2527 / 61.5011 | 59.55 / 56.754 | within noise |
| cache_mode | randwrite_1g/write_back | media_busy | 7322.37 / 7390.46 / 7320.55 | 7347.51 / 7349.99 | within noise |
| cache_mode | randwrite_1g/write_back | destage_runs | 1498 / 1498 / 1498 | 1498 / 1498 | identical |
| cache_mode | randwrite_1g/write_back | forced_destages | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | mixed_hot/write_through | ops | 2000 / 2000 / 2000 | 2000 / 2000 | identical |
| cache_mode | mixed_hot/write_through | errors | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | mixed_hot/write_through | iops | 441.103 / 428.504 / 426.216 | 418.786 / 429.225 | within noise |
| cache_mode | mixed_hot/write_through | iops_durable | 441.103 / 428.504 / 426.216 | 418.785 / 429.224 | within noise |
| cache_mode | mixed_hot/write_through | mbps | 1.80676 / 1.75515 / 1.74578 | 1.71535 / 1.75811 | within noise |
| cache_mode | mixed_hot/write_through | lat_avg | 9.06727 / 9.33325 / 9.38354 | 9.54961 / 9.31738 | within noise |
| cache_mode | mixed_hot/write_through | lat_p50 | 8.50746 / 8.8099 / 8.77016 | 8.93416 / 8.79244 | within noise |
| cache_mode | mixed_hot/write_through | lat_p95 | 19.8005 / 19.9776 / 20.3521 | 20.3921 / 20.2603 | within noise |
| cache_mode | mixed_hot/write_through | lat_p99 | 24.2061 / 24.3254 / 24.8596 | 25.6907 / 24.553 | within noise |
| cache_mode | mixed_hot/write_through | write_lat_p99 | 24.9084 / 24.532 / 27.7032 | 29.177 / 25.7843 | within noise |
| cache_mode | mixed_hot/write_through | read_lat_p99 | 23.6568 / 24.12 / 24.1529 | 25.0173 / 24.0459 | within noise |
| cache_mode | mixed_hot/write_through | read_hit_rate | 70.0422 / 70.0422 / 70.0422 | 70.0422 / 70.0422 | identical |
| cache_mode | mixed_hot/write_through | media_busy | 4390.31 / 4437.62 / 4428.73 | 4399.8 / 4392.91 | within noise |
| cache_mode | mixed_hot/write_through | destage_runs | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | mixed_hot/write_through | forced_destages | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | mixed_hot/write_back | ops | 2000 / 2000 / 2000 | 2000 / 2000 | identical |
| cache_mode | mixed_hot/write_back | errors | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | mixed_hot/write_back | iops | 1014.94 / 981.816 / 957.945 | 958.06 / 993.835 | within noise |
| cache_mode | mixed_hot/write_back | iops_durable | 704.274 / 661.544 / 661.468 | 661.579 / 678.397 | within noise |
| cache_mode | mixed_hot/write_back | mbps | 4.15721 / 4.02152 / 3.92374 | 3.92421 / 4.07075 | within noise |
| cache_mode | mixed_hot/write_back | lat_avg | 3.94036 / 4.07298 / 4.17384 | 4.17319 / 4.02334 | within noise |
| cache_mode | mixed_hot/write_back | lat_p50 | 2.01366 / 2.0897 / 2.14165 | 2.23888 / 2.12244 | within noise |
| cache_mode | mixed_hot/write_back | lat_p95 | 14.6372 / 15.0687 / 15.2396 | 15.2305 / 14.718 | within noise |
| cache_mode | mixed_hot/write_back | lat_p99 | 18.8396 / 20.3756 / 20.3363 | 20.0961 / 19.2466 | within noise |
| cache_mode | mixed_hot/write_back | write_lat_p99 | 15.5404 / 16.1151 / 16.6294 | 16.4522 / 15.4847 | within noise |
| cache_mode | mixed_hot/write_back | read_lat_p99 | 19.891 / 20.8249 / 20.6091 | 20.717 / 20.3061 | within noise |
| cache_mode | mixed_hot/write_back | read_hit_rate | 69.9719 / 69.9719 / 69.9719 | 69.9719 / 69.9719 | identical |
| cache_mode | mixed_hot/write_back | media_busy | 2768.66 / 2800.41 / 2753.98 | 2790.23 / 2802.45 | within noise |
| cache_mode | mixed_hot/write_back | destage_runs | 143 / 143 / 143 | 143 / 143 | identical |
| cache_mode | mixed_hot/write_back | forced_destages | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randread_full/write_through | ops | 800 / 800 / 800 | 800 / 800 | identical |
| cache_mode | randread_full/write_through | errors | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randread_full/write_through | iops | 75.4701 / 73.2327 / 74.1323 | 73.4061 / 74.9344 | within noise |
| cache_mode | randread_full/write_through | iops_durable | 75.4701 / 73.2327 / 74.1322 | 73.406 / 74.9344 | within noise |
| cache_mode | randread_full/write_through | mbps | 0.309126 / 0.299961 / 0.303646 | 0.300671 / 0.306931 | within noise |
| cache_mode | randread_full/write_through | lat_avg | 52.8611 / 54.4795 / 53.8146 | 54.3493 / 53.2383 | within noise |
| cache_mode | randread_full/write_through | lat_p50 | 53.2369 / 54.476 / 54.2211 | 54.4581 / 53.3166 | within noise |
| cache_mode | randread_full/write_through | lat_p95 | 65.9116 / 68.9336 / 67.7183 | 68.7057 / 66.6077 | within noise |
| cache_mode | randread_full/write_through | lat_p99 | 72.5155 / 75.2692 / 75.3307 | 76.5659 / 73.4713 | within noise |
| cache_mode | randread_full/write_through | read_lat_p99 | 72.5155 / 75.2692 / 75.3307 | 76.5659 / 73.4713 | within noise |
| cache_mode | randread_full/write_through | read_hit_rate | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randread_full/write_through | media_busy | 10457.6 / 10469.7 / 10451.6 | 10459.9 / 10382.2 | within noise |
| cache_mode | randread_full/write_through | destage_runs | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randread_full/write_through | forced_destages | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randread_full/write_back | ops | 800 / 800 / 800 | 800 / 800 | identical |
| cache_mode | randread_full/write_back | errors | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randread_full/write_back | iops | 76.1699 / 73.6394 / 73.235 | 74.4591 / 74.5138 | within noise |
| cache_mode | randread_full/write_back | iops_durable | 76.1698 / 73.6394 / 73.2349 | 74.459 / 74.5138 | within noise |
| cache_mode | randread_full/write_back | mbps | 0.311992 / 0.301627 / 0.29997 | 0.304985 / 0.305209 | within noise |
| cache_mode | randread_full/write_back | lat_avg | 52.3752 / 54.1769 / 54.4769 | 53.5792 / 53.5414 | within noise |
| cache_mode | randread_full/write_back | lat_p50 | 52.4392 / 54.1954 / 54.1184 | 53.782 / 53.8094 | within noise |
| cache_mode | randread_full/write_back | lat_p95 | 65.707 / 68.3132 / 68.5209 | 67.0035 / 68.1492 | within noise |
| cache_mode | randread_full/write_back | lat_p99 | 72.1687 / 75.6863 / 81.0785 | 75.2715 / 74.4577 | within noise |
| cache_mode | randread_full/write_back | read_lat_p99 | 72.1687 / 75.6863 / 81.0785 | 75.2715 / 74.4577 | within noise |
| cache_mode | randread_full/write_back | read_hit_rate | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randread_full/write_back | media_busy | 10399.4 / 10495.4 / 10459.4 | 10446 / 10438.1 | within noise |
| cache_mode | randread_full/write_back | destage_runs | 0 / 0 / 0 | 0 / 0 | identical |
| cache_mode | randread_full/write_back | forced_destages | 0 / 0 / 0 | 0 / 0 | identical |
| destage_order | lba_sorted | ops | 1500 / 1500 | 1500 | identical |
| destage_order | lba_sorted | errors | 0 / 0 | 0 | identical |
| destage_order | lba_sorted | iops | 198.893 / 190.741 | 191.373 | within noise |
| destage_order | lba_sorted | iops_durable | 155.271 / 148.788 | 148.905 | within noise |
| destage_order | lba_sorted | mbps | 0.814667 / 0.781276 | 0.783865 | within noise |
| destage_order | lba_sorted | lat_avg | 20.1101 / 20.9681 | 20.8994 | within noise |
| destage_order | lba_sorted | lat_p50 | 0.012156 / 0.025144 | 0.01762 | within noise |
| destage_order | lba_sorted | lat_p95 | 59.8209 / 62.6124 | 61.7282 | within noise |
| destage_order | lba_sorted | lat_p99 | 64.724 / 67.7796 | 66.946 | within noise |
| destage_order | lba_sorted | write_lat_p99 | 64.724 / 67.7796 | 66.946 | within noise |
| destage_order | lba_sorted | media_busy | 9305.52 / 9266.27 | 9320.73 | within noise |
| destage_order | lba_sorted | destage_runs | 1500 / 1500 | 1500 | identical |
| destage_order | lba_sorted | forced_destages | 0 / 0 | 0 | identical |
| destage_order | age_order | ops | 1500 / 1500 | 1500 | identical |
| destage_order | age_order | errors | 0 / 0 | 0 | identical |
| destage_order | age_order | iops | 97.0142 / 94.2194 | 95.0751 | within noise |
| destage_order | age_order | iops_durable | 75.0062 / 72.7637 | 73.4825 | within noise |
| destage_order | age_order | mbps | 0.39737 / 0.385923 | 0.389428 | within noise |
| destage_order | age_order | lat_avg | 41.2301 / 42.4511 | 42.0695 | within noise |
| destage_order | age_order | lat_p50 | 0.012588 / 0.022336 | 0.015 | within noise |
| destage_order | age_order | lat_p95 | 120.286 / 123.811 | 123.661 | within noise |
| destage_order | age_order | lat_p99 | 129.627 / 133.021 | 132.048 | within noise |
| destage_order | age_order | write_lat_p99 | 129.627 / 133.021 | 132.048 | within noise |
| destage_order | age_order | media_busy | 19662.3 / 19774.7 | 19763.1 | within noise |
| destage_order | age_order | destage_runs | 1500 / 1500 | 1500 | identical |
| destage_order | age_order | forced_destages | 0 / 0 | 0 | identical |
| readahead | on | ops | 2000 / 2000 | 2000 | identical |
| readahead | on | errors | 0 / 0 | 0 | identical |
| readahead | on | iops | 788.153 / 729.751 | 766.816 | within noise |
| readahead | on | iops_durable | 788.153 / 729.751 | 766.816 | within noise |
| readahead | on | mbps | 51.6524 / 47.825 | 50.254 | within noise |
| readahead | on | lat_avg | 1.26776 / 1.36937 | 1.30333 | within noise |
| readahead | on | lat_p50 | 1.17284 / 1.23557 | 1.26687 | within noise |
| readahead | on | lat_p95 | 1.99416 / 2.23334 | 2.0722 | within noise |
| readahead | on | lat_p99 | 2.55682 / 3.76842 | 2.92134 | within noise |
| readahead | on | read_lat_p99 | 2.55682 / 3.76842 | 2.92134 | within noise |
| readahead | on | read_hit_rate | 99.9 / 99.9 | 99.9 | identical |
| readahead | on | media_busy | 1702.93 / 1705.42 | 1703.86 | within noise |
| readahead | on | destage_runs | 0 / 0 | 0 | identical |
| readahead | on | forced_destages | 0 / 0 | 0 | identical |
| readahead | off | ops | 2000 / 2000 | 2000 | identical |
| readahead | off | errors | 0 / 0 | 0 | identical |
| readahead | off | iops | 108.964 / 108.252 | 108.667 | within noise |
| readahead | off | iops_durable | 108.964 / 108.252 | 108.667 | within noise |
| readahead | off | mbps | 7.14103 / 7.09437 | 7.1216 | within noise |
| readahead | off | lat_avg | 9.17637 / 9.23459 | 9.20025 | within noise |
| readahead | off | lat_p50 | 9.1625 / 9.206 | 9.2 | within noise |
| readahead | off | lat_p95 | 9.66608 / 10.9586 | 10.6564 | within noise |
| readahead | off | lat_p99 | 10.2596 / 12.4685 | 11.8355 | within noise |
| readahead | off | read_lat_p99 | 10.2596 / 12.4685 | 11.8355 | within noise |
| readahead | off | read_hit_rate | 0 / 0 | 0 | identical |
| readahead | off | media_busy | 17579.3 / 16429.9 | 16820.9 | within noise |
| readahead | off | destage_runs | 0 / 0 | 0 | identical |
| readahead | off | forced_destages | 0 / 0 | 0 | identical |
| queue_micro | lockfree_spsc | handoff_rate_median | 13.8167 / 18.6838 / 13.9139 | 44.7666 / 41.028 | **outside noise** |
| queue_micro | lockfree_spsc | handoff_rate_min | 12.7385 / 16.2347 / 13.0732 | 29.6387 / 40.5424 | **outside noise** |
| queue_micro | lockfree_spsc | handoff_rate_max | 20.9917 / 20.5146 / 16.6949 | 50.0833 / 44.2261 | **outside noise** |
| queue_micro | mutex_condvar | handoff_rate_median | 0.856043 / 0.740133 / 0.770549 | 0.755225 / 0.742302 | within noise |
| queue_micro | mutex_condvar | handoff_rate_min | 0.840572 / 0.690737 / 0.764537 | 0.711991 / 0.725249 | within noise |
| queue_micro | mutex_condvar | handoff_rate_max | 0.859634 / 0.763888 / 0.776732 | 0.767636 / 0.758638 | within noise |
| queue_e2e | lockfree/qd1 | cmd_rate | 1360.82 / 963.752 | 795.932 | **outside noise** |
| queue_e2e | lockfree/qd1 | lat_avg | 0.630764 / 0.906705 | 1.00711 | within noise |
| queue_e2e | lockfree/qd1 | lat_p99 | 0.736 / 2.234 | 3.614 | **outside noise** |
| queue_e2e | lockfree/qd32 | cmd_rate | 3355.13 / 3117.65 | 3296.16 | within noise |
| queue_e2e | lockfree/qd32 | lat_avg | 9.40198 / 10.1075 | 9.54493 | within noise |
| queue_e2e | lockfree/qd32 | lat_p99 | 21.393 / 54.064 | 58.423 | **outside noise** |
| powerloss | wb_noflush | trials | 100 / 100 / 100 | 100 / 100 | identical |
| powerloss | wb_noflush | acked_writes_per_trial | 548.07 / 531.48 / 526 | 533.65 / 528.2 | within noise |
| powerloss | wb_noflush | lost_per_trial_avg | 351.88 / 353.07 / 353.18 | 354.97 / 352.36 | within noise |
| powerloss | wb_noflush | lost_per_trial_max | 383 / 382 / 383 | 383 / 383 | within noise |
| powerloss | wb_noflush | lost_total | 35188 / 35307 / 35318 | 35497 / 35236 | within noise |
| powerloss | wb_noflush | lost_fraction_avg | 68.6396 / 70.7016 / 71.2814 | 70.7889 / 70.8656 | within noise |
| powerloss | wb_noflush | trials_with_loss | 100 / 100 / 100 | 100 / 100 | identical |
| powerloss | wb_noflush | durability_violations | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_noflush | unreadable_after_reboot | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_noflush | torn_writes | 78 / 52 / 48 | 61 / 52 | within noise |
| powerloss | wb_noflush | unacked_writes_that_landed | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_noflush | flushes_per_trial | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_noflush | write_iops_avg | 1441.05 / 1411.39 / 1403.06 | 1413.63 / 1407.31 | within noise |
| powerloss | wb_flush | trials | 100 / 100 / 100 | 100 / 100 | identical |
| powerloss | wb_flush | acked_writes_per_trial | 124.6 / 119.8 / 118.69 | 117.06 / 119.48 | within noise |
| powerloss | wb_flush | lost_per_trial_avg | 17.43 / 16.55 / 16.92 | 15.86 / 17.17 | within noise |
| powerloss | wb_flush | lost_per_trial_max | 32 / 32 / 32 | 31 / 32 | within noise |
| powerloss | wb_flush | lost_total | 1743 / 1655 / 1692 | 1586 / 1717 | within noise |
| powerloss | wb_flush | lost_fraction_avg | 19.3149 / 18.7216 / 18.8861 | 18.2786 / 19.1461 | within noise |
| powerloss | wb_flush | trials_with_loss | 99 / 98 / 98 | 99 / 99 | within noise |
| powerloss | wb_flush | durability_violations | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_flush | unreadable_after_reboot | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_flush | torn_writes | 76 / 70 / 64 | 54 / 65 | within noise |
| powerloss | wb_flush | unacked_writes_that_landed | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_flush | flushes_per_trial | 2.8 / 2.66 / 2.63 | 2.57 / 2.64 | within noise |
| powerloss | wb_flush | write_iops_avg | 291.449 / 279.287 / 274.736 | 271.033 / 277.141 | within noise |
| powerloss | wb_fua | trials | 100 / 100 / 100 | 100 / 100 | identical |
| powerloss | wb_fua | acked_writes_per_trial | 67.65 / 93.22 / 93.51 | 92.84 / 92.32 | within noise |
| powerloss | wb_fua | lost_per_trial_avg | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_fua | lost_per_trial_max | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_fua | lost_total | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_fua | lost_fraction_avg | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_fua | trials_with_loss | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_fua | durability_violations | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_fua | unreadable_after_reboot | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_fua | torn_writes | 46 / 60 / 69 | 61 / 60 | within noise |
| powerloss | wb_fua | unacked_writes_that_landed | 1 / 0 / 0 | 2 / 0 | within noise |
| powerloss | wb_fua | flushes_per_trial | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | wb_fua | write_iops_avg | 129.86 / 182.747 / 182.984 | 182.969 / 180.732 | within noise |
| powerloss | write_through | trials | 100 / 100 / 100 | 100 / 100 | identical |
| powerloss | write_through | acked_writes_per_trial | 79.21 / 93.52 / 95.58 | 95.4 / 94.15 | within noise |
| powerloss | write_through | lost_per_trial_avg | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | write_through | lost_per_trial_max | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | write_through | lost_total | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | write_through | lost_fraction_avg | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | write_through | trials_with_loss | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | write_through | durability_violations | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | write_through | unreadable_after_reboot | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | write_through | torn_writes | 64 / 60 / 63 | 63 / 57 | within noise |
| powerloss | write_through | unacked_writes_that_landed | 0 / 1 / 1 | 0 / 1 | within noise |
| powerloss | write_through | flushes_per_trial | 0 / 0 / 0 | 0 / 0 | identical |
| powerloss | write_through | write_iops_avg | 147.351 / 177.649 / 181.707 | 181.099 / 178.796 | within noise |
| integrity | crc32 | check_value_123456789 | 3.42178e+09 / 3.42178e+09 | 3.42178e+09 | identical |
| integrity | faults | sectors_read | 20000 / 20000 | 20000 | identical |
| integrity | faults | persistent_flips_injected | 25 / 25 | 25 | identical |
| integrity | faults | transient_flips_injected | 20 / 20 | 20 | identical |
| integrity | faults | crc_errors_detected | 45 / 45 | 45 | identical |
| integrity | faults | retries | 95 / 95 | 95 | identical |
| integrity | faults | recovered_by_retry | 20 / 20 | 20 | identical |
| integrity | faults | medium_errors_returned | 25 / 25 | 25 | identical |
| integrity | faults | medium_errors_at_injected_lba | 25 / 25 | 25 | identical |
| integrity | faults | good_reads | 19975 / 19975 | 19975 | identical |
| integrity | faults | silent_corruptions | 0 / 0 | 0 | identical |
| queue_e2e | mutex/qd1 | cmd_rate | 36.1345 / 20.563 / 20.8621 | 22.6293 / 20.9468 | within noise |
| queue_e2e | mutex/qd1 | lat_avg | 27.2324 / 48.4346 / 47.753 | 44.0231 / 47.5512 | within noise |
| queue_e2e | mutex/qd1 | lat_p99 | 166.162 / 131.2 / 161.597 | 132.623 / 154.772 | within noise |
| queue_e2e | mutex/qd32 | cmd_rate | 1312.3 / 1215.56 / 1676.28 | 1313.56 / 1258.16 | within noise |
| queue_e2e | mutex/qd32 | lat_avg | 24.2193 / 26.1441 / 18.9436 | 24.2199 / 25.2913 | within noise |
| queue_e2e | mutex/qd32 | lat_p99 | 183.385 / 182.48 / 120.987 | 126.957 / 148.711 | within noise |

## Follow-up on the metrics outside noise

**`queue_micro` / `lockfree_spsc` (handoff rate): most likely a code-alignment
effect, not a behaviour change.** The NEW binaries move items through the lock-free
ring ~2.5x faster in every run (38-45 vs 14-24 M items/s). This experiment
never creates a drive - it is two threads and `ring_spsc.c` - and neither
`ring_spsc.c` nor the benchmark code changed. Checks:

- Same path length (so the same initial stack layout): the OLD binary copied
  to `build/oldbinxx/fwbench` (22 characters, like `build/lockfree/fwbench`)
  still measured 16-24 M items/s against 38-39 for NEW, run alternately.
- The machine code of the hot functions is identical apart from call
  targets (`objdump` of `spsc_pop_timed`), but linking the new objects moved
  every hot function by 32 bytes relative to the 64-byte cache line:

  | function | OLD address mod 64 | NEW address mod 64 |
  |---|---|---|
  | q_producer | 32 | 0 |
  | spsc_try_push | 48 | 16 |
  | spsc_try_pop | 16 | 48 |
  | spsc_push | 32 | 0 |
  | spsc_pop_timed | 48 | 16 |

So the same instructions sit at a different alignment, which changes how
the tight spin loops fetch and decode. The micro-benchmark is sensitive to
code layout; it says nothing about the feature. (`-falign-functions=64`
would remove this sensitivity; not done, to leave the original build
untouched.)

**`queue_e2e` / `lockfree` (QD 1 and QD 32): within noise when repeated.**
Only one NEW run existed for these, so both binaries were run five times,
alternating (K commands/s, wall clock):

| | OLD (5 runs) | NEW (5 runs) |
|---|---|---|
| QD 1 command rate | 426, 554, 569, 741, 967 | 597, 691, 767, 770, 778 |
| QD 32 command rate | 2147, 2603, 2713, 3116, 3291 | 1688, 2004, 2954, 2967, 3002 |
| QD 1 p99 latency (us) | 2.62, 3.55, 3.87, 4.10, 7.38 | 3.10, 3.59, 4.21, 4.64, 4.69 |
| QD 32 p99 latency (us) | 28.98, 38.77, 54.51, 71.40, 83.02 | 46.53, 63.20, 74.27, 94.64, 162.62 |

All ranges overlap. Command rates show no consistent direction; the QD 32
p99 tends higher in NEW (median 74 vs 55 us, a microsecond-scale wall-clock
tail with a wide spread), which these five runs cannot separate from noise.
With the feature off, the per-command difference in the firmware loop is a
NULL check of `fw->vib` after each command.

**Conclusion.** With the feature off, every deterministic output is
identical (and `tests/test_policy.c` checks a bit-exact fingerprint of a
deterministic workload against the pre-feature firmware on every `make
test`). The wall-clock metrics are within run-to-run noise (the QD 32 p99 tail
leaning higher is the one to watch), except the
lock-free ring micro-benchmark, whose difference most likely comes from
where the linker places identical code (the checks above rule out the
stack layout and the code itself; the alignment shift is the remaining
candidate, not proven by an experiment that varies only alignment).
