import sys
import os
import glob
import re
import readline

import sys
import os
import glob
import re



dir_path = "/mnt/c/Users/hp/OneDrive/GMU_Notes/Sem4/611/Assignment3/Results/"
file_list = glob.glob(dir_path+'applu_010') #use standard *NIX wildcards to get your file names, in this case, all the files with a .txt extension
'''file_list.sort()
#print(file_list)
with open('IPC.xls', 'w') as out_file:
    out_file.write('Benchmark \t \t IPC \n')
    for filename in file_list:
	#print(filename)
        with open(filename, 'r') as in_file:
		#print in_file.readline(10)
        	for line in in_file:		
				if re.match("(.*)total IPC(.*)", line):
					print line,
					out_file.write(filename.split('Results/')[1]+'\t=\t')
					out_file.write(line.split('=')[1])'''

					
file_list.sort()				
#print(file_list)
with open('WRQ.xls', 'w') as out_file:
    out_file.write('W \t R \t Q \n')
    for filename in file_list:
	#print(filename)
        with open(filename, 'r') as in_file:
		#print in_file.readline(10)
        	for line in in_file:		
				if re.match("(\s\s.) single_limit = (.*)", line):
					print line,
					#out_file.write(filename.split('IPC_512/')[1]+'=')
					line=line.split('=')[1]
					out_file.write(line.split(';')[0]+'\t')
					
					
					
