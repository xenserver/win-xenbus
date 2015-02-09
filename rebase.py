import xspatchq.gittool as xsgittool
import patchqueue
import os
import sys
import posixpath
import shutil

def rebase():

    xsgittool.clone(patchqueue.baserepo, 'rebasequeue')

    pregitdir = os.getcwd()
    os.chdir('rebasequeue')

    if ( xsgittool.needsrebase(patchqueue.basetag)):
        print ("I SHOULD REBASE")
        xsgittool.pull(patchqueue.remoterepo,'master:master')
        xsgittool.resethard(patchqueue.basetag)
        print("and push")
        xsgittool.pushforce(patchqueue.baserepo)
        if os.path.exists(patchqueue.package):
            count = 0
            rebasename=""
            while True:
                rebasename = patchqueue.package+".rebase."+count
                if (not os.path.exists(rebasename)):
                    break;
                count+=1
            os.rename(patchqueue.package,rebasename)
    os.chdir(pregitdir)

if __name__ == '__main__':
    rebase()
    if os.path.exists(patchqueue.package):
        print("Package")
        count = 0
        rebasename=""
        while True:
            rebasename = patchqueue.package+".rebase."+str(count)
            if (not os.path.exists(rebasename)):
                break;
            count+=1
        os.rename(patchqueue.package,rebasename)

