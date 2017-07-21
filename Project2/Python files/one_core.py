#!/usr/bin/python

import os
import os.path
import sys
import string
import socket
import copy
#from workload_combs_alphabet import single_thread_workloads
single_thread_workloads = ['applu', 'apsi', 'crafty', 'bwaves_06', 'bzip2_source', 'cactusADM_06']


#GLOBALS
bench_dir = "/mnt/c/Users/hp/OneDrive/GMU_Notes/Sem4/611/Assignment1/SMTSIM/benchmarks"
scr_dir = "/mnt/c/Users/hp/OneDrive/GMU_Notes/Sem4/611/Assignment3/Shells/" #--Please provide the exact path
exe = "smtsim"
exe_path = os.path.join("/mnt/c/Users/hp/OneDrive/GMU_Notes/Sem4/611/Assignment1/SMTSIM/smtsim/build.linux-amd64", exe)
sims_group = "/mnt/c/Users/hp/OneDrive/GMU_Notes/Sem4/611/Assignment3/Results/"  #-Please provide the exact path
thread_length = 1e6
ff_dist = 1e6
host = ''

blue =[1, 2, 4]
b_len=len(blue)
yellow = [32, 64, 96]
y_len=len(yellow)
##white = [32, 64, 96]
##w_len=len(white)
##olive = [16,4, 32,8]
##o_len=len(olive)
amber = [16,32,48]
a_len=len(amber)
##green = [256,512]
##g_len=len(green)
miss = [48,16,4,16,4,2,2,2]
p=0

'''yellow16 = [16,16,16, 32,32, 32, 16,4,16,4, 2,1,1, 1,1,1 ]
yellow32 = [32,32,32, 32,32, 32, 16,4,16,4, 2,1,1, 1,1,1 ]
yellow48 = [48,48,48, 32,32, 32, 16,4,16,4, 2,1,1, 1,1,1 ]
amber32 =  [16,16,16, 32,32, 32, 16,4,16,4, 2,1,1, 1,1,1 ]
amber64 =  [16,16,16, 64,64, 32, 16,4,16,4, 2,1,1, 1,1,1 ]
amber96 =  [16,16,16, 96,96, 32, 16,4,16,4, 2,1,1, 1,1,1 ]
white32=   [16,16,16, 32,32, 32, 16,4,16,4, 2,1,1, 1,1,1 ]
white64=   [16,16,16, 32,32, 64, 16,4,16,4, 2,1,1, 1,1,1 ]
white96=   [16,16,16, 32,32, 96, 16,4,16,4, 2,1,1, 1,1,1 ]
olive16 =  [16,16,16, 32,32, 32, 16,4,16,4, 2,1,1, 1,1,1 ]
olive32 =  [16,16,16, 32,32, 32, 32,8,32,8, 2,1,1, 1,1,1 ]
blue1 =    [16,16,16, 32,32, 32, 16,4,16,4, 2,1,1, 1,1,1 ]
blue2 =    [16,16,16, 32,32, 32, 16,4,16,4, 2,1,1, 2,2,2 ]
blue4 =    [16,16,16, 32,32, 32, 16,4,16,4, 2,1,1, 4,4,4 ]

yellow16max = [16,16,16, 96,96, 96, 32,8,32,8, 2,1,1, 4,4,4 ]
yellow32max = [32,32,32, 96,96, 96, 32,8,32,8, 2,1,1, 4,4,4 ]
yellow48max = [48,48,48, 96,96, 96, 32,8,32,8, 2,1,1, 4,4,4 ]
amber32max =  [48,48,48, 32,32, 96, 32,8,32,8, 2,1,1, 4,4,4 ]
amber64max =  [48,48,48, 64,64, 96, 32,8,32,8, 2,1,1, 4,4,4 ]
amber96max =  [48,48,48, 96,96, 96, 32,8,32,8, 2,1,1, 4,4,4 ]
white32max=   [48,48,48, 96,96, 32, 32,8,32,8, 2,1,1, 4,4,4 ]
white64max=   [48,48,48, 96,96, 64, 32,8,32,8, 2,1,1, 4,4,4 ]
white96max=   [48,48,48, 96,96, 96, 32,8,32,8, 2,1,1, 4,4,4 ]
olive16max =  [48,48,48, 96,96, 96, 16,4,16,4, 2,1,1, 4,4,4 ]
olive32max =  [48,48,48, 96,96, 96, 32,8,32,8, 2,1,1, 4,4,4 ]
blue1max =    [48,48,48, 96,96, 96, 32,8,32,8, 2,1,1, 1,1,1 ]
blue2max =    [48,48,48, 96,96, 96, 32,8,32,8, 2,1,1, 2,2,2 ]
blue4max =    [48,48,48, 96,96, 96, 32,8,32,8, 2,1,1, 4,4,4 ]
'''


newpath = scr_dir + 'executeall.sh'
z = open(newpath,'w')
z.writelines("#!/bin/bash \n")
with open('WRQ.xls', 'w') as out_file:
    out_file.write('W \t R \t Q \n')
out_file.close()							
for b in range (b_len):
	#print (blue[b])
	new_blue = [blue[b], blue[b], blue[b]]
	#print (a)
	for y in range (y_len):
		new_yellow = [yellow[y],yellow[y]]
		for a in range (a_len):
					new_amber = [amber[a],amber[a],amber[a]]
					#for g in range (g_len):
						#new_green = [green[g],green[g]]
						
					c0= new_amber + new_yellow + miss + new_blue
					p=p+1
					#if c0==yellow16 or c0==yellow32 or c0==yellow48 or c0==amber32 or c0==amber64 or c0==amber96 or c0==white32 or c0==white64 or c0==white96 or c0==olive16 or c0==olive32 or c0==blue1 or c0==blue2 or c0==blue4 or c0==yellow16max or c0==yellow32max or c0==yellow48max or c0==amber32max or c0==amber64max or c0==amber96max or c0==white32max or c0==white64max or c0==white96max or c0==olive16max or c0==olive32max or c0==blue1max or c0==blue2max or c0==blue4max:
					#with open('WRQ.xls', 'w') as out_file:
					#		out_file.write('W \t R \t Q \n')
					#		out_file.write("%s \t %s \t %s \t %s \n" %(blue[b],yellow[y],amber[a],p) )
					'''with open('out_file_512.csv', 'a') as out_file:
						out_file.write("%s = %s \n" %(c0,p) )'''
					#print(c0)
						
						
#Creates a list of dictionaries. Each dictionary has a key for each config parameter
					def create_configurations():
						
						# Configuration creation
						#c0 = [24, 24, 32, 32, 64, 16, 8, 32, 8, 32, 2, 1, 1, 8, 4, 4 ]
						global c0

						configs_to_test = [c0]
						confs = [] 

						for c in configs_to_test:    
							#print c
							conf_dict = {}
							conf_dict['iqs']=c[0]  # Integer Instruction queue size 	amber
							conf_dict['fqs']=c[1]  # Fp Instruction queue size  		amber
							conf_dict['lsq']=c[2]  # Load Store Queue size 				amber
							conf_dict['ipr']=c[3]  # Integer phys. regs 				yellow
							conf_dict['fpr']=c[4]  # Fp phys. regs  					yellow
							 
							conf_dict['rob']=c[5]  # Reorder buffer size				white  
							#conf_dict['lsq']=c[5]  # Load Store Queue size 			yellow 
							conf_dict['ica']=c[6]  # ICache assoc 						olive
							conf_dict['ics']=c[7]  # ICache size 						olive
							conf_dict['dca']=c[8]  # Dcache assoc 						olive
							conf_dict['dcs']=c[9]  # Dcache size 						olive
							conf_dict['mii']=c[10] # Max integer issue
							conf_dict['mfi']=c[11] # Max fp issue	
							conf_dict['mli']=c[12] # Max load-store issue
							conf_dict['fb']=c[13]  # Fetch width 						blue
							conf_dict['mci']=c[14] # Commit width 						blue
							conf_dict['dec']=c[15] # Decode limit 						blue
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
					%s -conffile "/mnt/c/Users/hp/OneDrive/GMU_Notes/Sem4/611/Assignment1/SMTSIM/benchmarks/workloads-list_ffs0.conf" \
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
												  -confexpr "Global/Mem/L2Cache/size_kb = 256;" \
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
												  -confexpr "Core/Decode/total_limit = %s;" \
												  -confexpr "Thread/active_list_size = %s;" \
												  -confexpr "ResourcePooling/enable = f;" \
												  -confdump - > "%s%s"
					[ $? -eq 0 ]||echo "Error: App did not exit normally"
					""" % (bench_dir, exe_path,  sims_group+simout_name, input1, "%e" %ff_dist, input1, "%e" %thread_length,conf['ics'],conf['ica'],conf['dcs'],conf['dca'], conf['lsq'], conf['iqs'], conf['fqs'],conf['fb'],conf['fb'],conf['mci'],conf['mci'],conf['mii'],conf['mfi'],conf['mli'],conf['ipr'],conf['fpr'],conf['rob'],conf['dec'],get_ceil_power_of_two(conf['rob']*8), sims_group, simout_name)
						return script
						
					def get_simout_name(exe,input1,ff_dist,c):
						global p
						s = ""
						#s += "exe_%s@inp_%s@ffs_%1.1e" % (exe, input1, ff_dist)
						s += "%s" % (input1)
						if p < 10:
							s += "_00%s" % (p)
						elif p > 9 and p < 100:
							s += "_0%s" % (p)
						else:
							s +="_%s" % (p)
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
						
						global p
						#m=" "
						#m=copy.deepcopy(i)
						newpath = scr_dir + 'executeall.sh'
						z = open(newpath,'a')
						#z.writelines("#!/bin/bash \n\n")
						c_list = create_configurations()
						for (input1) in single_thread_workloads:
							#if input != 'gamess_06_triazolium': 
							#    continue

							for c in c_list:
								simout_name = get_simout_name(exe,input1,ff_dist,c)
								s = gen_script(input1, ff_dist, c, thread_length, simout_name)
								s_name = simout_name+".sh"
								with open('WRQ.xls', 'a') as out_file:
									out_file.write("%s \t %s \t %s \t %s \n" %(blue[b],yellow[y],amber[a],simout_name) )
								out_file.close()
								li.append(s_name);
								#knew = s_name
								f = open(scr_dir+s_name,'w')
								#print simout_name
								f.writelines(s)
								#print s
								f.close()
								os.system("chmod u+x %s/%s" % (scr_dir, s_name))
								
						print li
						cmd_name = "#!/bin/bash \n\n"
						if p==1:
							z.writelines(cmd_name)
						#print(len(li))
						for i in li:
							cmd_name = './%s \n' % i
							z.writelines(cmd_name)
						z.close()
						os.system("chmod u+x %s/executeall.sh" % (scr_dir))	  

					if __name__ == "__main__":
							main()

