#!python -u

import os, sys
import datetime
import re
import glob
import tarfile
import subprocess

def shell(command):
    print(command)
    sys.stdout.flush()

    pipe = os.popen(command, 'r', 1)

    for line in pipe:
        print(line.rstrip())

    return pipe.close()


class msbuild_failure(Exception):
    def __init__(self, value):
        self.value = value
    def __str__(self):
        return repr(self.value)

def msbuild(batfile, projdir, name, sdv_arg):
    cwd = os.getcwd()

    os.environ['CONFIGURATION'] = 'Windows Developer Preview Release'
    os.environ['SDV_PROJ'] = name
    os.environ['SDV_ARG'] = sdv_arg

    os.chdir('proj')
    os.chdir(projdir)
    status = shell(batfile)
    os.chdir(cwd)

#    if (status != None):
#        raise msbuild_failure(sdv_arg)


if __name__ == '__main__':
    msbuild('..\msbuild_sdv.bat', 'xenbus', 'xenbus.vcxproj', '/clean')

    msbuild('..\msbuild_sdv.bat', 'xenbus', 'xenbus.vcxproj', '/check:default.sdv')

    msbuild('..\msbuild_dvl.bat', 'xenbus', 'xenbus.vcxproj', '')
