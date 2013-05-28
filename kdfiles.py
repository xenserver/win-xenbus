#!python -u

import os, sys
import subprocess
import glob
from pprint import pprint

def regenerate_kdfiles(filename, arch, pkg, source):
	cwd = os.getcwd()
	file = open(filename, 'w')
	os.chdir(pkg + '/' + arch)
	drivers = glob.glob('*.sys')
	pprint(drivers)
	for driver in drivers:
		file.write("map\n")
		file.write('\SystemRoot\System32\drivers\\' + driver + '\n')
		file.write(source + '\\' + pkg + '\\' + arch + '\\' + driver + '\n')
		file.write('\n')
	os.chdir(cwd)
	file.close()

if __name__ == '__main__':
	pkg = 'xenbus'
	source = os.getcwd()
	regenerate_kdfiles('kdfiles32.txt', 'x86', pkg, source)
	regenerate_kdfiles('kdfiles64.txt', 'x64', pkg, source)
