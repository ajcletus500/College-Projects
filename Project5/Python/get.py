import sys
import os
import glob
import re


a=1
dir_path = "/mnt/c/Users/hp/OneDrive/GMU_Notes/Sem4/611/Project/two_core/Results/"
file_list = glob.glob(dir_path+'*') #use standard *NIX wildcards to get your file names, in this case, all the files with a .txt extension
file_list.sort()
#print(file_list)
b=1
#c=filename.split('Results/')[1]
m=0
for filename in file_list:
	c=filename.split('Results/')[1]+".csv"
	if re.match("(.*).A0", filename) or re.match("(.*).A1", filename):
		continue
	#elif (re.match("(*).A1", filename)):
	#	continue
	print(filename)
	with open(filename, 'r') as in_file:
	#print in_file.readline(10)
		print(c)
		with open(c, 'w') as out_file:
			b=b+1
			
			c="test"+str(b)+".csv"
			print(b)
			if a:
				out_file.write(filename.split('Results/')[1]+'\n \n')
			else:
				out_file.write(filename.split('Results/')[1]+'\n \n')
			for line in in_file:		
				if re.match("(.*), ipc(.*)", line):
				#print line,
					out_file.write(line.split('ipc :')[1])
					a=0
					m=m+1
					if m >1000:
						break
			out_file.close()
			m=0
			
					
