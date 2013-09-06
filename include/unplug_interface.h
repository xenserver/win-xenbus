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

#ifndef _XENFILT_UNPLUG_INTERFACE_H
#define _XENFILT_UNPLUG_INTERFACE_H

#define DEFINE_UNPLUG_OPERATIONS                                \
        UNPLUG_OPERATION(VOID,                                  \
                         Acquire,                               \
                         (                                      \
                         IN  PXENFILT_UNPLUG_CONTEXT Context    \
                         )                                      \
                         )                                      \
        UNPLUG_OPERATION(VOID,                                  \
                         Release,                               \
                         (                                      \
                         IN  PXENFILT_UNPLUG_CONTEXT Context    \
                         )                                      \
                         )                                      \
        UNPLUG_OPERATION(VOID,                                  \
                         Replay,                                \
                         (                                      \
                         IN  PXENFILT_UNPLUG_CONTEXT Context    \
                         )                                      \
                         )

typedef struct _XENFILT_UNPLUG_CONTEXT  XENFILT_UNPLUG_CONTEXT, *PXENFILT_UNPLUG_CONTEXT;

#define UNPLUG_OPERATION(_Type, _Name, _Arguments) \
        _Type (*UNPLUG_ ## _Name) _Arguments;

typedef struct _XENFILT_UNPLUG_OPERATIONS {
    DEFINE_UNPLUG_OPERATIONS
} XENFILT_UNPLUG_OPERATIONS, *PXENFILT_UNPLUG_OPERATIONS;

#undef UNPLUG_OPERATION

typedef struct _XENFILT_UNPLUG_INTERFACE    XENFILT_UNPLUG_INTERFACE, *PXENFILT_UNPLUG_INTERFACE;

// {201A139A-AD4D-4ECE-BA0B-7F7AAEA46029}
DEFINE_GUID(GUID_UNPLUG_INTERFACE, 
            0x201a139a,
            0xad4d,
            0x4ece,
            0xba,
            0xb,
            0x7f,
            0x7a,
            0xae,
            0xa4,
            0x60,
            0x29);

#define UNPLUG_INTERFACE_VERSION    1

#define UNPLUG_OPERATIONS(_Interface) \
        (PXENFILT_UNPLUG_OPERATIONS *)((ULONG_PTR)(_Interface))

#define UNPLUG_CONTEXT(_Interface) \
        (PXENFILT_UNPLUG_CONTEXT *)((ULONG_PTR)(_Interface) + sizeof (PVOID))

#define UNPLUG(_Operation, _Interface, ...) \
        (*UNPLUG_OPERATIONS(_Interface))->UNPLUG_ ## _Operation((*UNPLUG_CONTEXT(_Interface)), __VA_ARGS__)

#endif  // _XENFILT_UNPLUG_INTERFACE_H

