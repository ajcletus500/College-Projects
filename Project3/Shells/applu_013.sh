#!/bin/bash


					cd "/mnt/c/Users/hp/OneDrive/GMU_Notes/Sem4/611/Assignment1/SMTSIM/benchmarks"
					/mnt/c/Users/hp/OneDrive/GMU_Notes/Sem4/611/Assignment1/SMTSIM/smtsim/build.linux-amd64/smtsim -conffile "/mnt/c/Users/hp/OneDrive/GMU_Notes/Sem4/611/Assignment1/SMTSIM/benchmarks/workloads-list_ffs0.conf" \
												  -confexpr "Syscall/root_paths_at_cwd = t;" \
												  -confexpr "Syscall/ForceUniqueNames = { \"fort.11\"; };" \
												  -confexpr "AppStatsLog/enable = t;" \
												  -confexpr "AppStatsLog/interval = 10e3;" \
												  -confexpr "AppStatsLog/base_name = \"/mnt/c/Users/hp/OneDrive/GMU_Notes/Sem4/611/Assignment4/Results/applu_013\";" \
												  -confexpr "AppStatsLog/stat_mask/all = f;" \
												  -confexpr "AppStatsLog/stat_mask/cyc = t;" \
												  -confexpr "AppStatsLog/stat_mask/commits = t;" \
												  -confexpr "AppStatsLog/stat_mask/l3cache_hr = t;" \
												  -confexpr "AppStatsLog/stat_mask/mem_delay = t;" \
												  -confexpr "AppStatsLog/stat_mask/itlb_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/dtlb_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/icache_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/dcache_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/l2cache_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/l3cache_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/bpred_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/fpalu_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/intalu_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/ldst_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/lsq_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/iq_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/fq_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/ireg_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/freg_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/iren_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/fren_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/rob_acc = t;" \
												  -confexpr "AppStatsLog/stat_mask/lsq_occ = t;" \
												  -confexpr "AppStatsLog/stat_mask/iq_occ = t;" \
												  -confexpr "AppStatsLog/stat_mask/fq_occ = t;" \
												  -confexpr "AppStatsLog/stat_mask/ireg_occ = t;" \
												  -confexpr "AppStatsLog/stat_mask/freg_occ = t;" \
												  -confexpr "AppStatsLog/stat_mask/iren_occ = t;" \
												  -confexpr "AppStatsLog/stat_mask/fren_occ = t;" \
												  -confexpr "AppStatsLog/stat_mask/rob_occ = t;" \
												  -confexpr "Workloads/applu/ff_dist = 1.000000e+06;" \
												  -confexpr "WorkQueue/Jobs/job_1 = { start_time = 0.; workload = \"applu\"};"\
												  -confexpr "WorkQueue/max_running_jobs = 1;"\
												  -confexpr "Global/thread_length = 1.000000e+06;" \
												  -confexpr "Global/num_cores = 1;" \
												  -confexpr "Global/num_contexts = 1;" \
												  -confexpr "Global/ThreadCoreMap/t0 = 0;" \
												  -confexpr "Global/Mem/private_l2caches = t;" \
												  -confexpr "Global/Mem/L2Cache/size_kb = 256;" \
												  -confexpr "Global/Mem/L2Cache/assoc = 4;" \
												  -confexpr "Global/Mem/L2Cache/access_time = { latency = 10; interval = 2; };" \
												  -confexpr "Global/Mem/L2Cache/access_time_wb = { latency = 10; interval = 2; };" \
												  -confexpr "Global/Mem/use_l3cache = t;" \
												  -confexpr "Global/Mem/L3Cache/size_kb = 1024;" \
												  -confexpr "Global/Mem/L3Cache/assoc = 8;" \
												  -confexpr "Global/Mem/L3Cache/access_time = { latency = 20; interval = 8; };" \
												  -confexpr "Global/Mem/L3Cache/access_time_wb = { latency = 20; interval = 8; };" \
												  -confexpr "Global/Mem/MainMem/read_time = { latency = 250; interval = 100; };" \
												  -confexpr "Global/Mem/MainMem/write_time = { latency = 250; interval = 100; };" \
												  -confexpr "Core/ICache/size_kb = 16;" \
												  -confexpr "Core/ICache/assoc = 4;" \
												  -confexpr "Core/ICache/access_time = { latency = 2; interval = latency; };"  \
												  -confexpr "Core/ICache/access_time_wb = { latency = 3; interval = latency; };"  \
												  -confexpr "Core/DCache/size_kb = 16;" \
												  -confexpr "Core/DCache/assoc = 4;" \
												  -confexpr "Core/DCache/access_time = { latency = 2; interval = latency; };"  \
												  -confexpr "Core/DCache/access_time_wb = { latency = 3; interval = latency; };"  \
												  -confexpr "Core/loadstore_queue_size = 16;" \
												  -confexpr "Core/Queue/int_queue_size = 24;" \
												  -confexpr "Core/Queue/float_queue_size = 24;" \
												  -confexpr "Core/Fetch/single_limit = 8;" \
												  -confexpr "Core/Fetch/total_limit = 8;" \
												  -confexpr "Core/Commit/single_limit = 4;" \
												  -confexpr "Core/Commit/total_limit = 4;" \
												  -confexpr "Core/Queue/max_int_issue = 2;" \
												  -confexpr "Core/Queue/max_float_issue = 1;" \
												  -confexpr "Core/Queue/max_ldst_issue = 1;" \
												  -confexpr "Core/Rename/int_rename_regs = 32;" \
												  -confexpr "Core/Rename/float_rename_regs = 32;" \
												  -confexpr "Thread/reorder_buffer_size = 64;" \
												  -confexpr "Thread/active_list_size = 512;" \
												  -confexpr "ResourcePooling/enable = f;" \
												  -confdump - > "/mnt/c/Users/hp/OneDrive/GMU_Notes/Sem4/611/Assignment4/Results/applu_013"
					[ $? -eq 0 ]||echo "Error: App did not exit normally"
					