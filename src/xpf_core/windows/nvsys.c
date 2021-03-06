/*
  NoirVisor - Hardware-Accelerated Hypervisor solution

  Copyright 2018-2020, Zero Tang. All rights reserved.

  This file is the NoirVisor's System Function assets of XPF-Core.
  Facilities are implemented in this file, including:
  Debugging facilities...
  Memory Management...
  Processor Management...
  Multithreading Management...

  This program is distributed in the hope that it will be useful, but 
  without any warranty (no matter implied warranty or merchantability
  or fitness for a particular purpose, etc.).

  File Location: ./xpf_core/windows/nvsys.c
*/

#include <ntifs.h>
#include <windef.h>
#include <stdlib.h>
#include <stdarg.h>
#include "nvsys.h"

void __cdecl NoirDebugPrint(const char* Format,...)
{
	va_list arg_list;
	va_start(arg_list,Format);
	vDbgPrintExWithPrefix("[NoirVisor - Driver] ",DPFLTR_IHVDRIVER_ID,DPFLTR_INFO_LEVEL,Format,arg_list);
	va_end(arg_list);
}

void __cdecl nvci_tracef(const char* format,...)
{
	va_list arg_list;
	va_start(arg_list,format);
	vDbgPrintExWithPrefix("[NoirVisor - CI Log] ",DPFLTR_IHVDRIVER_ID,DPFLTR_TRACE_LEVEL,format,arg_list);
	va_end(arg_list);
}

void __cdecl nvci_panicf(const char* format,...)
{
	va_list arg_list;
	va_start(arg_list,format);
	vDbgPrintExWithPrefix("[NoirVisor - CI Panic] ",DPFLTR_IHVDRIVER_ID,DPFLTR_ERROR_LEVEL,format,arg_list);
	va_end(arg_list);
}

void __cdecl nv_dprintf(const char* format,...)
{
	va_list arg_list;
	va_start(arg_list,format);
	vDbgPrintExWithPrefix("[NoirVisor] ",DPFLTR_IHVDRIVER_ID,DPFLTR_INFO_LEVEL,format,arg_list);
	va_end(arg_list);
}

void __cdecl nv_tracef(const char* format,...)
{
	va_list arg_list;
	va_start(arg_list,format);
	vDbgPrintExWithPrefix("[NoirVisor - Trace] ",DPFLTR_IHVDRIVER_ID,DPFLTR_TRACE_LEVEL,format,arg_list);
	va_end(arg_list);
}

void __cdecl nv_panicf(const char* format,...)
{
	va_list arg_list;
	va_start(arg_list,format);
	vDbgPrintExWithPrefix("[NoirVisor - Panic] ",DPFLTR_IHVDRIVER_ID,DPFLTR_ERROR_LEVEL,format,arg_list);
	va_end(arg_list);
}

ULONG32 noir_get_processor_count()
{
	KAFFINITY af;
	return KeQueryActiveProcessorCount(&af);
}

ULONG32 noir_get_current_processor()
{
	return KeGetCurrentProcessorNumber();
}

void static NoirDpcRT(IN PKDPC Dpc,IN PVOID DeferedContext OPTIONAL,IN PVOID SystemArgument1 OPTIONAL,IN PVOID SystemArgument2 OPTIONAL)
{
	noir_broadcast_worker worker=(noir_broadcast_worker)SystemArgument1;
	PLONG32 volatile GlobalOperatingNumber=(PLONG32)SystemArgument2;
	ULONG Pn=KeGetCurrentProcessorNumber();
	worker(DeferedContext,Pn);
	InterlockedDecrement(GlobalOperatingNumber);
}

void noir_generic_call(noir_broadcast_worker worker,void* context)
{
	ULONG32 Num=noir_get_processor_count();
	PVOID IpbBuffer=ExAllocatePool(NonPagedPool,Num*sizeof(KDPC)+4);
	if(IpbBuffer)
	{
		PLONG32 volatile GlobalOperatingNumber=(PULONG32)((ULONG_PTR)IpbBuffer+Num*sizeof(KDPC));
		PKDPC pDpc=(PKDPC)IpbBuffer;
		ULONG i=0;
		*GlobalOperatingNumber=Num;
		for(;i<Num;i++)
		{
			KeInitializeDpc(&pDpc[i],NoirDpcRT,context);
			KeSetTargetProcessorDpc(&pDpc[i],(BYTE)i);
			KeSetImportanceDpc(&pDpc[i],HighImportance);
			KeInsertQueueDpc(&pDpc[i],(PVOID)worker,(PVOID)GlobalOperatingNumber);
		}
		// Use TTAS-Spinning Semaphore here for better performance.
		while(InterlockedCompareExchange(GlobalOperatingNumber,0,0))
			_mm_pause();		// Optimized Processor Relax.
		ExFreePool(IpbBuffer);
	}
}

ULONG32 noir_get_instruction_length(IN PVOID code,IN BOOLEAN LongMode)
{
	ULONG arch=LongMode?64:0;
	return LDE(code,arch);
}

void* noir_alloc_contd_memory(size_t length)
{
	// PHYSICAL_ADDRESS L={0};
	PHYSICAL_ADDRESS H={0xFFFFFFFFFFFFFFFF};
	// PHYSICAL_ADDRESS B={0};
	// PVOID p=MmAllocateContiguousMemorySpecifyCacheNode(length,L,H,B,MmCached,MM_ANY_NODE_OK);
	PVOID p=MmAllocateContiguousMemory(length,H);
	if(p)RtlZeroMemory(p,length);
	return p;
}

void* noir_alloc_nonpg_memory(size_t length)
{
	PVOID p=ExAllocatePoolWithTag(NonPagedPool,length,'pNvN');
	if(p)RtlZeroMemory(p,length);
	return p;
}

void* noir_alloc_paged_memory(size_t length)
{
	PVOID p=ExAllocatePoolWithTag(PagedPool,length,'gPvN');
	if(p)RtlZeroMemory(p,length);
	return p;
}

void noir_free_contd_memory(void* virtual_address)
{
#if defined(_WINNT5)
	//It is recommended to release contiguous memory at APC level on NT5.
	KIRQL f_oldirql;
	KeRaiseIrql(APC_LEVEL,&f_oldirql);
#endif
	MmFreeContiguousMemory(virtual_address);
#if defined(_WINNT5)
	KeLowerIrql(f_oldirql);
#endif
}

void noir_free_nonpg_memory(void* virtual_address)
{
	ExFreePoolWithTag(virtual_address,'pNvN');
}

void noir_free_paged_memory(void* virtual_address)
{
	ExFreePoolWithTag(virtual_address,'gPvN');
}

ULONG64 noir_get_physical_address(void* virtual_address)
{
	PHYSICAL_ADDRESS pa;
	pa=MmGetPhysicalAddress(virtual_address);
	return pa.QuadPart;
}

//We need to map physical memory in nesting virtualization.
void* noir_map_physical_memory(ULONG64 physical_address,size_t length)
{
	PHYSICAL_ADDRESS pa;
	pa.QuadPart=physical_address;
	return MmMapIoSpace(pa,length,MmCached);
}

void noir_unmap_physical_memory(void* virtual_address,size_t length)
{
	MmUnmapIoSpace(virtual_address,length);
}

void noir_copy_memory(void* dest,void* src,size_t cch)
{
	RtlCopyMemory(dest,src,cch);
}

void* noir_find_virt_by_phys(ULONG64 physical_address)
{
	PHYSICAL_ADDRESS pa;
	pa.QuadPart=physical_address;
	/*
	  The WDK Document claims that MmGetVirtualForPhysical
	  is a routine reserved for system use.
	  However, I think this is the perfect solution to resolve
	  the physical address issue in virtualization nesting.

	  As described in WRK v1.2, MmGetVirtualForPhysical could run
	  at any IRQL, given that the address is in system space.
	*/
	return MmGetVirtualForPhysical(pa);
}

void* noir_alloc_2mb_page()
{
	PHYSICAL_ADDRESS L={0};
	PHYSICAL_ADDRESS H={0xFFFFFFFFFFFFFFFF};
	PHYSICAL_ADDRESS B={0x200000};
	PVOID p=MmAllocateContiguousMemorySpecifyCache(0x200000,L,H,B,MmCached);
	if(p)RtlZeroMemory(p,0x200000);
	return p;
}

void noir_free_2mb_page(void* virtual_address)
{
	MmFreeContiguousMemorySpecifyCache(virtual_address,0x200000,MmCached);
}

//Some Additional repetitive functions
ULONG64 NoirGetPhysicalAddress(IN PVOID VirtualAddress)
{
	PHYSICAL_ADDRESS pa=MmGetPhysicalAddress(VirtualAddress);
	return pa.QuadPart;
}

PVOID NoirAllocateContiguousMemory(IN ULONG Length)
{
	PHYSICAL_ADDRESS MaxAddr={0xFFFFFFFFFFFFFFFF};
	return MmAllocateContiguousMemory(Length,MaxAddr);
}

// Essential Multi-Threading Facility.
HANDLE noir_create_thread(IN PKSTART_ROUTINE StartRoutine,IN PVOID Context)
{
	OBJECT_ATTRIBUTES oa;
	HANDLE hThread=NULL;
	InitializeObjectAttributes(&oa,NULL,OBJ_KERNEL_HANDLE,NULL,NULL);
	PsCreateSystemThread(&hThread,SYNCHRONIZE,&oa,NULL,NULL,StartRoutine,Context);
	return hThread;
}

void noir_exit_thread(IN NTSTATUS Status)
{
	PsTerminateSystemThread(Status);
}

BOOLEAN noir_join_thread(IN HANDLE ThreadHandle)
{
	NTSTATUS st=ZwWaitForSingleObject(ThreadHandle,FALSE,NULL);
	if(st==STATUS_SUCCESS)
	{
		ZwClose(ThreadHandle);
		return TRUE;
	}
	return FALSE;
}

BOOLEAN noir_alert_thread(IN HANDLE ThreadHandle)
{
	NTSTATUS st=ZwAlertThread(ThreadHandle);
	return st==STATUS_SUCCESS;
}

// Sleep
void noir_sleep(IN ULONG64 ms)
{
	LARGE_INTEGER Time;
	Time.QuadPart=ms*(-10000);
	KeDelayExecutionThread(KernelMode,TRUE,&Time);
}

// Resource Lock (R/W Lock)
PERESOURCE noir_initialize_reslock()
{
	PERESOURCE Res=ExAllocatePool(NonPagedPool,sizeof(ERESOURCE));
	if(Res)
	{
		NTSTATUS st=ExInitializeResourceLite(Res);
		if(NT_ERROR(st))
		{
			ExFreePool(Res);
			Res=NULL;
		}
	}
	return Res;
}

void noir_finalize_reslock(IN PERESOURCE Resource)
{
	if(Resource)
	{
		ExDeleteResourceLite(Resource);
		ExFreePool(Resource);
	}
}

void noir_acquire_reslock_shared(IN PERESOURCE Resource)
{
	KeEnterCriticalRegion();
	ExAcquireResourceSharedLite(Resource,TRUE);
}

void noir_acquire_reslock_shared_ex(IN PERESOURCE Resource)
{
	KeEnterCriticalRegion();
	ExAcquireSharedStarveExclusive(Resource,TRUE);
}

void noir_acquire_reslock_exclusive(IN PERESOURCE Resource)
{
	KeEnterCriticalRegion();
	ExAcquireResourceExclusiveLite(Resource,TRUE);
}

void noir_release_reslock(IN PERESOURCE Resource)
{
	ExReleaseResourceLite(Resource);
	KeLeaveCriticalRegion();
}

// Standard I/O
void noir_qsort(IN PVOID base,IN ULONG num,IN ULONG width,IN noir_sorting_comparator comparator)
{
	qsort(base,num,width,comparator);
}