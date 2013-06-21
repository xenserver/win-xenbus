To install the XenServer Paravitual Bus Device Driver onto a XenServer Windows 
guest VM:

*    Copy xenbus.sys, xen.sys, xenfilt.sys, xenbus_coinst.dll and xenbus.inf 
     onto the guest VM 
*    Copy dpinst.exe from the Windows driver kit into the same folder as
     xenbus.sys, xen.sys, xenfilt.sys, xenbus_coinst.dll and xeniface.inf on 
     the guest vm, ensuring the version of dpinst.exe matches the architecture 
     of the verison of Windows installed on your VM
*    As administrator, run dpinst.exe on the guest vm
*    If any warnings arise about unknown certificates, accept them

