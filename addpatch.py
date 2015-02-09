import sys
import patchqueue
import xspatchq.gittool as xsgittool

def addpatch(newfilename):
    newqueue = open ('patchqueue.py','w')
    
    print("remoterepo = r\""+patchqueue.remoterepo+"\"", file=newqueue)
    print("baserepo = r\""+patchqueue.baserepo+"\"", file=newqueue)
    print("basetag = r\""+patchqueue.basetag+"\"", file=newqueue)
    print("package = r\""+patchqueue.package+"\"", file=newqueue)
    
    print("components = [", file=newqueue)
    for component in patchqueue.components:
        print("\tr\""+component+"\",", file=newqueue)
    print("\t]", file=newqueue)
    print("sdv_components = [", file=newqueue)
    for component in patchqueue.sdv_components:
        print("\tr\""+component+"\",", file=newqueue)
    print("\t]", file=newqueue)
    print("patchlist = [", file=newqueue)
    
    newlist = patchqueue.patchlist
    
    newlist.append(newfilename)
    
    for patch in patchqueue.patchlist:
        print("\tr\""+patch+"\",", file=newqueue)
    print("\t]", file=newqueue)
    xsgittool.add(newfilename)
    xsgittool.add("patchqueue.py")
    
if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage:  addpatch <patchfilename>")
        exit(1)
    addpatch(sys.argv[1])
