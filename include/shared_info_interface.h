/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

#ifndef _XENBUS_SHARED_INFO_INTERFACE_H
#define _XENBUS_SHARED_INFO_INTERFACE_H

#define DEFINE_SHARED_INFO_OPERATIONS                                                    \
        SHARED_INFO_OPERATION(VOID,                                                      \
                              Acquire,                                                   \
                              (                                                          \
                              IN  PXENBUS_SHARED_INFO_CONTEXT Context                    \
                              )                                                          \
                              )                                                          \
        SHARED_INFO_OPERATION(VOID,                                                      \
                              Release,                                                   \
                              (                                                          \
                              IN  PXENBUS_SHARED_INFO_CONTEXT Context                    \
                              )                                                          \
                              )                                                          \
        SHARED_INFO_OPERATION(BOOLEAN,                                                   \
                              EvtchnPoll,                                                \
                              (                                                          \
                              IN  PXENBUS_SHARED_INFO_CONTEXT Context,                   \
                              IN  BOOLEAN                     (*Function)(PVOID, ULONG), \
                              IN  PVOID                       Argument                   \
                              )                                                          \
                              )                                                          \
        SHARED_INFO_OPERATION(VOID,                                                      \
                              EvtchnAck,                                                 \
                              (                                                          \
                              IN  PXENBUS_SHARED_INFO_CONTEXT Context,                   \
                              IN  ULONG                       Port                       \
                              )                                                          \
                              )                                                          \
        SHARED_INFO_OPERATION(VOID,                                                      \
                              EvtchnMask,                                                \
                              (                                                          \
                              IN  PXENBUS_SHARED_INFO_CONTEXT Context,                   \
                              IN  ULONG                       Port                       \
                              )                                                          \
                              )                                                          \
        SHARED_INFO_OPERATION(BOOLEAN,                                                   \
                              EvtchnUnmask,                                              \
                              (                                                          \
                              IN  PXENBUS_SHARED_INFO_CONTEXT Context,                   \
                              IN  ULONG                       Port                       \
                              )                                                          \
                              )                                                          \
        SHARED_INFO_OPERATION(LARGE_INTEGER,                                             \
                              GetTime,                                                   \
                              (                                                          \
                              IN  PXENBUS_SHARED_INFO_CONTEXT Context                    \
                              )                                                          \
                              )

typedef struct _XENBUS_SHARED_INFO_CONTEXT  XENBUS_SHARED_INFO_CONTEXT, *PXENBUS_SHARED_INFO_CONTEXT;

#define SHARED_INFO_OPERATION(_Type, _Name, _Arguments) \
        _Type (*SHARED_INFO_ ## _Name) _Arguments;

typedef struct _XENBUS_SHARED_INFO_OPERATIONS {
    DEFINE_SHARED_INFO_OPERATIONS
} XENBUS_SHARED_INFO_OPERATIONS, *PXENBUS_SHARED_INFO_OPERATIONS;

#undef SHARED_INFO_OPERATION

typedef struct _XENBUS_SHARED_INFO_INTERFACE XENBUS_SHARED_INFO_INTERFACE, *PXENBUS_SHARED_INFO_INTERFACE;

// {05DC267C-36CA-44a3-A124-B9BA9FE3780B}
DEFINE_GUID(GUID_SHARED_INFO_INTERFACE, 
            0x5dc267c,
            0x36ca,
            0x44a3,
            0xa1,
            0x24,
            0xb9,
            0xba,
            0x9f,
            0xe3,
            0x78,
            0xb);

#define SHARED_INFO_INTERFACE_VERSION   4

#define SHARED_INFO_OPERATIONS(_Interface) \
        (PXENBUS_SHARED_INFO_OPERATIONS *)((ULONG_PTR)(_Interface))

#define SHARED_INFO_CONTEXT(_Interface) \
        (PXENBUS_SHARED_INFO_CONTEXT *)((ULONG_PTR)(_Interface) + sizeof (PVOID))

#define SHARED_INFO(_Operation, _Interface, ...) \
        (*SHARED_INFO_OPERATIONS(_Interface))->SHARED_INFO_ ## _Operation((*SHARED_INFO_CONTEXT(_Interface)), __VA_ARGS__)

#endif  // _XENBUS_SHARED_INFO_H
