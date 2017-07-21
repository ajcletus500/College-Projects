import sys
import os
import glob
import re

#for L2 cache = 256

dir_path = "/mnt/c/Users/hp/OneDrive/GMU_Notes/Sem4/611/Assignment4/Results/"
file_list = glob.glob(dir_path+'*') #use standard *NIX wildcards to get your file names, in this case, all the files with a .txt extension
file_list.sort()
#print(file_list)
with open('IPC.xls', 'w') as out_file:
    for filename in file_list:
	#print(filename)
        with open(filename, 'r') as in_file:
		#print in_file.readline(10)
        	for line in in_file:		
				if re.match("(.*)total IPC(.*)", line):
					print line,
					out_file.write(filename.split('Results/')[1]+'\t=\t')
					out_file.write(line.split('=')[1])

					
'''					
#for L2 cache = 512
					
dir_path = "/mnt/c/Users/hp/OneDrive/GMU_Notes/Sem4/611/Assignment1/SMTSIM/results/HW2/IPC_512/"
file_list = glob.glob(dir_path+'*') #use standard *NIX wildcards to get your file names, in this case, all the files with a .txt extension
file_list.sort()
#print(file_list)
with open('L2_512.csv', 'w') as out_file:
    for filename in file_list:
	#print(filename)
        with open(filename, 'r') as in_file:
		#print in_file.readline(10)
        	for line in in_file:		
				if re.match("(.*)total IPC(.*)", line):
					print line,
					out_file.write(filename.split('IPC_512/')[1]+'=')
					out_file.write(line.split('=')[1])'''
					
					
					
