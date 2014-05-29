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

#ifndef _XENBUS_CACHE_H
#define _XENBUS_CACHE_H

#include <ntddk.h>
#include <store_interface.h>

typedef struct _XENBUS_CACHE XENBUS_CACHE, *PXENBUS_CACHE;

extern NTSTATUS
CacheInitialize(
    IN  PXENBUS_STORE_INTERFACE StoreInterface,
    IN  const CHAR              *Name,
    IN  ULONG                   Size,
    IN  ULONG                   Reservation,
    IN  NTSTATUS                (*Ctor)(PVOID, PVOID),
    IN  VOID                    (*Dtor)(PVOID, PVOID),
    IN  VOID                    (*AcquireLock)(PVOID),
    IN  VOID                    (*ReleaseLock)(PVOID),
    IN  PVOID                   Argument,
    OUT PXENBUS_CACHE           *Cache
    );

extern VOID
CacheTeardown(
    IN  PXENBUS_CACHE   Cache
    );

extern PVOID
CacheGet(
    IN  PXENBUS_CACHE   Cache,
    IN  BOOLEAN         Locked
    );

extern VOID
CachePut(
    IN  PXENBUS_CACHE   Cache,
    IN  PVOID           Object,
    IN  BOOLEAN         Locked
    );

typedef struct _XENBUS_CACHE_STATISTICS {
    ULONG   Allocated;
    ULONG   MaximumAllocated;
    ULONG   Population;
    ULONG   MinimumPopulation;
} XENBUS_CACHE_STATISTICS, *PXENBUS_CACHE_STATISTICS;

extern VOID
CacheGetStatistics(
    IN  PXENBUS_CACHE            Cache,
    OUT PXENBUS_CACHE_STATISTICS Statistics
    );

#endif  // _XENBUS_CACHE_H
