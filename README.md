XenBus - The XenServer Windows Paravitual Bus Device Driver
==========================================

XenBus consists of three device drivers:

*    XenBus.Sys is a bus driver which attaches to a virtual device on the PCI
     bus and provides child devices for the other XenServer device drivers to
     attach to.

*    Xen.Sys is a support library which provides interfaces for communicating
     with the XenServer host and hypervisor

*    XenFile is a filter driver which is used to mask emulated devices (such
     as disk and network devices) from Windows, when paravitulized devices
     are available 

Quick Start
===========

Prerequisites to build
----------------------

*   Visual Studio 2012 or later 
*   Windows Driver Kit 8 or later
*   Python 3 or later 

Environment variables used in building driver
-----------------------------

MAJOR\_VERSION Major version number

MINOR\_VERSION Minor version number

MICRO\_VERSION Micro version number

BUILD\_NUMBER Build number

SYMBOL\_SERVER location of a writable symbol server directory

KIT location of the Windows driver kit

PROCESSOR\_ARCHITECTURE x86 or x64

VS location of visual studio

Commands to build
-----------------

    git clone http://github.com/xenserver/win-xenbus
    cd win-xenbus
    .\build.py [checked | free]

Device tree diagram
-------------------


                                                             |     |
                    XenBus--(Xen.Sys)               [====XenFilt===]
                      |                             |  |  |  |  |  |
    PCI Bus :---------+-----------------------------+--+--+--+--+--+
               VEN_5853&DEV_0002&SUBSYS_00025853     Other Devices        
