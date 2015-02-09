
import os
import shutil
import xspatchq.buildtool as xsbuildtool
import stat

def needsrebase(tag):
    if (not contains(tag)):
        return True
    return False

def remove_readonly(func, path, execinfo):
    os.chmod(path, stat.S_IWRITE)
    os.unlink(path)

def apply(patchname):
    res = xsbuildtool.shell(['git', 'am', patchname], None)
    if res != 0 :
        raise Exception("git am "+patchname+" returned ", res)

def clone(repo, destination):
    if os.path.exists(destination):
        print ("removing "+destination)
        shutil.rmtree(destination, onerror=remove_readonly)
    res = xsbuildtool.shell(['git','clone',repo, destination], None)
    if res != 0:
        raise Exception("git clone "+repo+" "+destination+" returned :", res)

def clone_to(repo, tag, destination):
    if os.path.exists(destination):
        shutil.rmtree(destination, onerror=remove_readonly)
    res = xsbuildtool.shell(['git','clone',repo, destination], None)
    if res != 0:
        raise Exception("git clone "+repo+" "+destination+" returned :", res)
    res = xsbuildtool.shell(['git','checkout',tag], destination)
    if res != 0:
        raise Exception("git checkout "+tag+" returned :", res)

def add(filename):
    xsbuildtool.shell(['git','add',filename], None)

def resethard(tag):
    xsbuildtool.shell(['git','reset','--hard', tag], None)

def pull(repo,spec):
    xsbuildtool.shell(['git','pull',repo,spec],None)

def push(repo):
	xsbuildtool.shell(['git','push',repo,"master:master"],None)

def pushforce(repo):
	xsbuildtool.shell(['git','push','-f',repo,"master:master"],None)

def contains(tag):
    res = xsbuildtool.shell(['git','merge-base','--is-ancestor',tag,'HEAD'],None)
    return (res == 0) 
