#!/usr/bin/python

import os
import os.path
import sys
import string
import socket
#from workload_combs_alphabet import single_thread_workloads
single_thread_workloads = ['applu', 'apsi', 'art_470', 'bwaves_06', 'bzip2_source', 'cactusADM_06']

#GLOBALS
bench_dir = "/home/aaron/Desktop/SMTSIM/benchmarks/alltogether/"
scr_dir = "/home/aaron/Desktop/SMTSIM/smtsim-scripts/one_core/"
exe = "smtsim"
exe_path = os.path.join("/home/aaron/Desktop/SMTSIM/smtsim/build.linux-amd64", exe)
sims_group = "/home/aaron/Desktop/SMTSIM/results/one_core/"
thread_length = 1e6
ff_dist = 1e6
host = ''



#Creates a list of dictionaries. Each dictionary has a key for each config parameter
def create_configurations():
    
    # Configuration creation
    c0 = [24, 24, 32, 32, 64, 16, 8, 32, 8, 32, 2, 1, 1, 8, 4 ]


    configs_to_test = [c0]
    confs = [] 

    for c in configs_to_test:    
        #print c
        conf_dict = {}
        conf_dict['iqs']=c[0]  # Integer Instruction queue size
        conf_dict['fqs']=c[1]  # Fp Instruction queue size 
        conf_dict['ipr']=c[2]  # Integer phys. regs
        conf_dict['fpr']=c[3]  # Fp phys. regs
        conf_dict['rob']=c[4]  # Reorder buffer size
        conf_dict['lsq']=c[5]  # Load Store Queue size
        conf_dict['ica']=c[6]  # ICache assoc
        conf_dict['ics']=c[7]  # ICache size
        conf_dict['dca']=c[8]  # Dcache assoc
        conf_dict['dcs']=c[9]  # Dcache size
        conf_dict['mii']=c[10] # Max integer issue
        conf_dict['mfi']=c[11] # Max fp issue
        conf_dict['mli']=c[12] # Max load-store issue
        conf_dict['fb']=c[13]  # Fetch width
        conf_dict['mci']=c[14] # Commit width
#        conf_dict['orpol']=c[15] # Order policy 
#        conf_dict['lipol']=c[16] # Limit policy 
        confs.append(conf_dict)
    return confs                                            
    

def get_ceil_power_of_two(n):
    i = 1
    while (i < n):
        i <<= 1
    return i

def gen_script(input1,ff_dist,conf,thread_length,simout_name):
    script = '#!/bin/bash\n'
    global host
    if host == '':
        host = socket.gethostname()
    if host == 'mercury4.uvic.ca':
        #script += '# PBS -N PCSTL\n'
        script += '#PBS -l walltime=5:00:00\n'
        #script += '#PBS -l walltime=72:00:00\n'
        script += '#PBS -q express\n'
        #script += '#PBS -q general\n'
        #script += '#PBS -q long\n'
        script += '#PBS -S /bin/bash\n'
        #script += '# PBS -m abe\n'
        #script += '# PBS -M vkontori@cs.ucsd.edu'
    if host == '':
        print 'Error: Unknown host'
        sys.exit(1)
    script +=r"""

cd "%s"
%s -conffile "/home/aaron/Desktop/SMTSIM/smtsim-scripts/workloads-list_ffs0.conf" \
                              -confexpr "Syscall/root_paths_at_cwd = t;" \
                              -confexpr "Syscall/ForceUniqueNames = { \"fort.11\"; };" \
                              -confexpr "AppStatsLog/enable = t;" \
                              -confexpr "AppStatsLog/interval = 10e3;" \
                              -confexpr "AppStatsLog/base_name = \"%s\";" \
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
                              -confexpr "Workloads/%s/ff_dist = %s;" \
                              -confexpr "WorkQueue/Jobs/job_1 = { start_time = 0.; workload = \"%s\"};"\
                              -confexpr "WorkQueue/max_running_jobs = 1;"\
                              -confexpr "Global/thread_length = %s;" \
                              -confexpr "Global/num_cores = 1;" \
                              -confexpr "Global/num_contexts = 1;" \
                              -confexpr "Global/ThreadCoreMap/t0 = 0;" \
                              -confexpr "Global/Mem/private_l2caches = t;" \
                              -confexpr "Global/Mem/L2Cache/size_kb = 512;" \
                              -confexpr "Global/Mem/L2Cache/access_time = { latency = 10; interval = 2; };" \
                              -confexpr "Global/Mem/L2Cache/access_time_wb = { latency = 10; interval = 2; };" \
                              -confexpr "Global/Mem/use_l3cache = t;" \
                              -confexpr "Global/Mem/L3Cache/size_kb = 8192;" \
                              -confexpr "Global/Mem/L3Cache/assoc = 8;" \
                              -confexpr "Global/Mem/L3Cache/access_time = { latency = 20; interval = 8; };" \
                              -confexpr "Global/Mem/L3Cache/access_time_wb = { latency = 20; interval = 8; };" \
                              -confexpr "Global/Mem/MainMem/read_time = { latency = 250; interval = 100; };" \
                              -confexpr "Global/Mem/MainMem/write_time = { latency = 250; interval = 100; };" \
                              -confexpr "Core/ICache/size_kb = %s;" \
                              -confexpr "Core/ICache/assoc = %s;" \
                              -confexpr "Core/ICache/access_time = { latency = 2; interval = latency; };"  \
                              -confexpr "Core/ICache/access_time_wb = { latency = 3; interval = latency; };"  \
                              -confexpr "Core/DCache/size_kb = %s;" \
                              -confexpr "Core/DCache/assoc = %s;" \
                              -confexpr "Core/DCache/access_time = { latency = 2; interval = latency; };"  \
                              -confexpr "Core/DCache/access_time_wb = { latency = 3; interval = latency; };"  \
                              -confexpr "Core/loadstore_queue_size = %s;" \
                              -confexpr "Core/Queue/int_queue_size = %s;" \
                              -confexpr "Core/Queue/float_queue_size = %s;" \
                              -confexpr "Core/Fetch/single_limit = %s;" \
                              -confexpr "Core/Fetch/total_limit = %s;" \
                              -confexpr "Core/Commit/single_limit = %s;" \
                              -confexpr "Core/Commit/total_limit = %s;" \
                              -confexpr "Core/Queue/max_int_issue = %s;" \
                              -confexpr "Core/Queue/max_float_issue = %s;" \
                              -confexpr "Core/Queue/max_ldst_issue = %s;" \
                              -confexpr "Core/Rename/int_rename_regs = %s;" \
                              -confexpr "Core/Rename/float_rename_regs = %s;" \
                              -confexpr "Thread/reorder_buffer_size = %s;" \
                              -confexpr "Thread/active_list_size = %s;" \
                              -confexpr "ResourcePooling/enable = f;" \
                              -confdump - > "%s%s"
[ $? -eq 0 ]||echo "Error: App did not exit normally"
""" % (bench_dir, exe_path,  sims_group+simout_name, input1, "%e" %ff_dist, input1, "%e" %thread_length,conf['ics'],conf['ica'],conf['dcs'],conf['dca'], conf['lsq'], conf['iqs'], conf['fqs'],conf['fb'],conf['fb'],conf['mci'],conf['mci'],conf['mii'],conf['mfi'],conf['mli'],conf['ipr'],conf['fpr'],conf['rob'],get_ceil_power_of_two(conf['rob']*8), sims_group, simout_name)
    return script
    
def get_simout_name(exe,input1,ff_dist,c):
    s = ""
    #s += "exe_%s@inp_%s@ffs_%1.1e" % (exe, input1, ff_dist)
    s += "%s" % (input1)
    #s += '@iqs='+str(c['iqs'])
    #s += '@fqs='+str(c['fqs'])
    #s += '@ipr='+str(c['ipr'])
    #s += '@fpr='+str(c['fpr'])
    #s += '@rob='+str(c['rob'])
    #s += '@lsq='+str(c['lsq'])
    #s += '@ics='+str(c['ics'])
    #s += '@dcs='+str(c['dcs'])
    #s += '_'+str(c['mii'])
    #s += str(c['mfi'])
    #s += str(c['mli'])
    #s += '@fb='+str(c['fb'])
    #s += '@mci='+str(c['mci'])
    #s += '@l3=8MB'
#    s += '@orpol='+str(c['orpol'])
#    s += '@lipol='+str(c['lipol'])

    return s

def main():
    li=[]
    newpath = scr_dir + 'executeall.sh'
    z = open(newpath,'w')
    z.writelines("#!/bin/bash \n\n")
    c_list = create_configurations()
    for (input1) in single_thread_workloads:
        #if input != 'gamess_06_triazolium': 
        #    continue

        for c in c_list:
            simout_name = get_simout_name(exe,input1,ff_dist,c)
            s = gen_script(input1, ff_dist, c, thread_length, simout_name)
            s_name = simout_name+".sh"
            li.append(s_name);
            #knew = s_name
            f = open(scr_dir+s_name,'w')
            print s_name
            f.writelines(s)
            #print s
            f.close()
            os.system("chmod u+x %s/%s" % (scr_dir, s_name))
    print li
    cmd_name = "#!/bin/bash \n"
    for i in li:
    	cmd_name = './%s \n' % i
	z.writelines(cmd_name)
    z.close()
    os.system("chmod u+x %s/executeall.sh" % (scr_dir))	  

if __name__ == "__main__":
        main()

