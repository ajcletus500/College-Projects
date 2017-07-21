import os;


fil1= open("comopare.txt",'w');
fil_reqd=[]
for f in os.listdir('.'):
	fileExt = os.path.splitext(f)[-1]
    	if '' == fileExt:
        	fil_reqd.append(f)
for f in fil_reqd:
	output_saver=""
	find_str = "App scheduled IPC:"
	fil = open(f,'r')
	f_lines = fil.readlines()
	for line_str in f_lines:
		if(line_str.find(find_str)==2):
			output_saver = line_str[find_str.__len__()+5:line_str.__len__()-2]
	fil.close()
	fil1.writelines(f+" : "+output_saver+"\n")


fil1.close()
