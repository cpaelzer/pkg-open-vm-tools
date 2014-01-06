/*********************************************************
 * Copyright (C) 2007 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation version 2 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 *********************************************************/

/*
 * vmciKernelIf.c -- 
 * 
 *      This file implements defines and helper functions for VMCI
 *      host _and_ guest kernel code. This is the linux specific
 *      implementation.
 */ 

/* Must come before any kernel header file */
#include "driver-config.h"

#if !defined(linux) || defined(VMKERNEL)
#error "Wrong platform."
#endif

#define EXPORT_SYMTAB
#define __NO_VERSION__
#include "compat_module.h"

#include "compat_version.h"
#include "compat_wait.h"
#include "compat_interrupt.h"
#include "compat_spinlock.h"
#include "compat_slab.h"
#include "compat_semaphore.h"
#include "compat_page.h"
#include "compat_mm.h"
#include "compat_highmem.h"
#include "vm_basic_types.h"
#include "pgtbl.h"
#include <linux/vmalloc.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
#  include <linux/mm.h>         /* For vmalloc_to_page() */
#endif
#include <linux/socket.h>       /* For memcpy_{to,from}iovec(). */
#include "vm_assert.h"
#include "vmci_kernel_if.h"
#include "vmci_queue_pair.h"

/*
 * In Linux 2.6.25 kernels and onwards, the symbol init_mm is no
 * longer exported. This affects the function PgtblKVa2MPN, as it
 * calls pgd_offset_k which in turn is a macro referencing init_mm.
 * 
 * We can avoid using PgtblKVa2MPN on more recent kernels by instead
 * using the function vmalloc_to_page followed by
 * page_to_pfn. vmalloc_to_page was introduced in the 2.5 kernels and
 * backported to some 2.4.x kernels. We use vmalloc_to_page on all
 * 2.6.x kernels, where it is present for sure, and use PgtblKVa2MPN
 * on older kernels where it works just fine.
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
#  define VMCIKVaToMPN(__VA) page_to_pfn(vmalloc_to_page((void *)(__VA)))
#else
#  define VMCIKVaToMPN(__VA) PgtblKVa2MPN(__VA)
#endif

/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_InitLock
 *
 *      Initializes the lock. Must be called before use.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Thread can block.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCI_InitLock(VMCILock *lock,    // IN:
              char *name,        // IN: Unused on Linux
              VMCILockRank rank) // IN: Unused on Linux
{
   spin_lock_init(lock);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_CleanupLock
 *
 *      Cleanup the lock. Must be called before deallocating lock.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Deletes kernel lock state
 *
 *-----------------------------------------------------------------------------
 */

void
VMCI_CleanupLock(VMCILock *lock)
{
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_GrabLock
 *
 *      Grabs the given lock. XXX Fill in specific lock requirements. XXX Move
 *      locking code into hostif if VMCI stays in vmmon.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Thread can block.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCI_GrabLock(VMCILock *lock,       // IN
              VMCILockFlags *flags) // OUT: used to restore irql on windows
{
   spin_lock(lock);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_ReleaseLock
 *
 *      Releases the given lock. XXX Move locking code into hostif if VMCI
 *      stays in vmmon.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      A thread blocked on this lock may wake up.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCI_ReleaseLock(VMCILock *lock,      // IN
                 VMCILockFlags flags) // IN
{
   spin_unlock(lock);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_GrabLock_BH
 *
 *      Grabs the given lock and for linux kernels disables bottom half execution.
 * .    This should be used with locks accessed both from bottom half/tasklet
 *      contexts, ie. guestcall handlers, and from process contexts to avoid
 *      deadlocks where the process has the lock and gets descheduled due to a
 *      bh/tasklet coming in.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCI_GrabLock_BH(VMCILock *lock,        // IN
                 VMCILockFlags *flags)  // OUT: used to restore
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 3, 4)
   spin_lock_bh(lock);
#else

   /* 
    * Before 2.3.4 linux kernels spin_unlock_bh didn't exist so we are using 
    * spin_lock_irqsave/restore instead. I wanted to define spin_[un]lock_bh
    * functions in compat_spinlock.h as local_bh_disable;spin_lock(lock) and
    * so on, but local_bh_disable/enable does not exist on 2.2.26.
    */
   spin_lock_irqsave(lock, *flags);
#endif // LINUX_VERSION_CODE
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_ReleaseLock_BH
 *
 *      Releases the given lock and for linux kernels reenables bottom half 
 *      execution.
 * .    This should be used with locks accessed both from bottom half/tasklet
 *      contexts, ie. guestcall handlers, and from process contexts to avoid
 *      deadlocks where the process has the lock and get descheduled due to a
 *      bh/tasklet coming in.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCI_ReleaseLock_BH(VMCILock *lock,        // IN
                    VMCILockFlags flags)   // IN
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 3, 4)
   spin_unlock_bh(lock);
#else

   /* 
    * Before 2.3.4 linux kernels spin_unlock_bh didn't exist so we are using 
    * spin_lock_irqsave/restore instead. I wanted to define spin_[un]lock_bh
    * functions in compat_spinlock.h as local_bh_disable;spin_lock(lock) and
     * so on, but local_bh_disable/enable does not exist on 2.2.26.
     */

   spin_unlock_irqrestore(lock, flags);
#endif // LINUX_VERSION_CODE
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIHost_InitContext --
 *
 *      Host-specific initialization of VMCI context state.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
VMCIHost_InitContext(VMCIHost *hostContext, // IN
                     uintptr_t eventHnd)    // IN
{
   init_waitqueue_head(&hostContext->waitQueue);
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIHost_ReleaseContext --
 *
 *      Host-specific release of state allocated by
 *      VMCIHost_InitContext.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
VMCIHost_ReleaseContext(VMCIHost *hostContext) // IN
{
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIHost_SignalCall --
 *
 *      Signal to userlevel that a VMCI call is waiting.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
VMCIHost_SignalCall(VMCIHost *hostContext)     // IN
{
   wake_up(&hostContext->waitQueue);
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIHost_WaitForCallLocked --
 *
 *      Wait until a VMCI call is pending or the waiting thread is
 *      interrupted. It is assumed that a lock is held prior to
 *      calling this function. The lock will be released during the
 *      wait. The correctnes of this funtion depends on that the same
 *      lock is held when the call is signalled.
 *
 * Results:
 *      TRUE on success
 *      FALSE if the wait was interrupted.
 *
 * Side effects:
 *      The call may block.
 *
 *----------------------------------------------------------------------
 */

Bool
VMCIHost_WaitForCallLocked(VMCIHost *hostContext, // IN
                           VMCILock *lock,        // IN
                           VMCILockFlags *flags,  // IN
                           Bool useBH)            // IN

{
   DECLARE_WAITQUEUE(wait, current);

   /* 
    * The thread must be added to the wait queue and have its state
    * changed while holding the lock - otherwise a signal may change
    * the state in between and have it overwritten causing a loss of
    * the event.
    */      

   add_wait_queue(&hostContext->waitQueue, &wait);
   current->state = TASK_INTERRUPTIBLE;

   if (useBH) {
      VMCI_ReleaseLock_BH(lock, *flags);
   } else {
      VMCI_ReleaseLock(lock, *flags);
   }

   schedule();

   if (useBH) {
      VMCI_GrabLock_BH(lock, flags);
   } else {
      VMCI_GrabLock(lock, flags);
   }

   current->state = TASK_RUNNING;

   remove_wait_queue(&hostContext->waitQueue, &wait);

   if (signal_pending(current)) {
      return FALSE;
   }

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIHost_ClearCall --
 *
 *      Clear the pending call signal.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
VMCIHost_ClearCall(VMCIHost *hostContext)     // IN
{
}

/*
 *----------------------------------------------------------------------
 *
 * VMCI_AllocKernelMem
 *
 *      Allocate some kernel memory for the VMCI driver. 
 *
 * Results:
 *      The address allocated or NULL on error. 
 *      
 *
 * Side effects:
 *      memory is malloced
 *----------------------------------------------------------------------
 */

void *
VMCI_AllocKernelMem(size_t size, int flags)
{
   void *ptr;

   if ((flags & VMCI_MEMORY_ATOMIC) != 0) {
      ptr = kmalloc(size, GFP_ATOMIC);
   } else {
      ptr = kmalloc(size, GFP_KERNEL);
   }

   return ptr;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCI_FreeKernelMem
 *
 *      Free kernel memory allocated for the VMCI driver. 
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      memory is freed.
 *----------------------------------------------------------------------
 */

void
VMCI_FreeKernelMem(void *ptr,   // IN:
                   size_t size) // IN: Unused on Linux
{
   kfree(ptr);
}


/*
 *----------------------------------------------------------------------
 *
 * VMCI_AllocBuffer
 *
 *      Allocate some kernel memory for the VMCI driver. The memory is
 *      not guaranteed to have a mapping in the virtual address
 *      space. Use VMCI_MapBuffer to get a VA mapping for the memory.
 *      
 * Results:
 *      A reference to the allocated memory or VMCI_BUFFER_INVALID
 *      on error.
 *      
 *
 * Side effects:
 *      memory is allocated.
 *----------------------------------------------------------------------
 */

VMCIBuffer
VMCI_AllocBuffer(size_t size, int flags)
{
   return VMCI_AllocKernelMem(size, flags);
}


/*
 *----------------------------------------------------------------------
 *
 * VMCI_MapBuffer
 *
 *      Ensures that the kernel memory allocated with VMCI_AllocBuffer
 *      has a mapping in the virtual address space.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      virtual address mapping of kernel memory is established.
 *----------------------------------------------------------------------
 */

void *
VMCI_MapBuffer(VMCIBuffer buf)
{
   return buf;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCI_ReleaseBuffer
 *
 *      Releases the VA mapping of kernel memory allocated with
 *      VMCI_AllocBuffer.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      virtual address mapping of kernel memory is released.
 *----------------------------------------------------------------------
 */

void
VMCI_ReleaseBuffer(void *ptr) // IN: The VA of the mapped memory
{
}


/*
 *----------------------------------------------------------------------
 *
 * VMCI_FreeBuffer
 *
 *      Free temporary kernel memory allocated for the VMCI driver. 
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      memory is freed.
 *----------------------------------------------------------------------
 */

void
VMCI_FreeBuffer(VMCIBuffer buf, // IN:
                size_t size)    // IN: Unused on Linux
{
   VMCI_FreeKernelMem(buf, size);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_CopyToUser --
 *
 *      Copy memory to the user application from a kernel buffer. This
 *      function may block, so don't call it while holding any kind of
 *      lock.
 *
 * Results:
 *      0 on success.
 *      Nonzero on failure.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

int
VMCI_CopyToUser(void *dst,        // OUT
                const void *src,  // IN
                unsigned int len) // IN
{
   return copy_to_user(dst, src, len) ? -EFAULT : 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_CopyFromUser --
 *
 *      Copy memory from the user application to a kernel buffer. This
 *      function may block, so don't call it while holding any kind of
 *      lock.
 *
 * Results:
 *      0 on success.
 *      Nonzero on failure.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

int
VMCI_CopyFromUser(void *dst,        // OUT
                  const void *src,  // IN
                  size_t len)       // IN
{
   return copy_from_user(dst, src, len);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_CreateEvent --
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCI_CreateEvent(VMCIEvent *event)  // IN:
{
   init_waitqueue_head(event);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_DestroyEvent --
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCI_DestroyEvent(VMCIEvent *event)  // IN:
{
   /* Nothing to do. */
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_SignalEvent --
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCI_SignalEvent(VMCIEvent *event)  // IN:
{
   wake_up(event);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_WaitOnEvent --
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCI_WaitOnEvent(VMCIEvent *event,              // IN:
		 VMCIEventReleaseCB releaseCB,  // IN:
		 void *clientData)              // IN:
{
   DECLARE_WAITQUEUE(wait, current);

   if (event == NULL || releaseCB == NULL) {
      return;
   }

   add_wait_queue(event, &wait);
   current->state = TASK_INTERRUPTIBLE;

   /* 
    * Release the lock or other primitive that makes it possible for us to 
    * put the current thread on the wait queue without missing the signal. 
    * Ie. on Linux we need to put ourselves on the wait queue and set our
    * stateto TASK_INTERRUPTIBLE without another thread signalling us.
    * The releaseCB is used to synchronize this.
    */
   releaseCB(clientData);
   
   schedule();
   current->state = TASK_RUNNING;
   remove_wait_queue(event, &wait);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIMutex_Init --
 *
 *      Initializes the mutex. Must be called before use.
 *
 * Results:
 *      Success.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

int
VMCIMutex_Init(VMCIMutex *mutex) // IN:
{
   sema_init(mutex, 1);
   return VMCI_SUCCESS;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIMutex_Destroy --
 *
 *      Destroys the mutex.  Does nothing on Linux.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCIMutex_Destroy(VMCIMutex *mutex) // IN: Unused
{
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIMutex_Acquire --
 *
 *      Acquires the mutex.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Thread may block.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCIMutex_Acquire(VMCIMutex *mutex) // IN:
{
   down(mutex);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIMutex_Release --
 *
 *      Releases the mutex.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May wake up the thread blocking on this mutex.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCIMutex_Release(VMCIMutex *mutex) // IN:
{
   up(mutex);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_AllocQueueKVA --
 *
 *      Allocates kernel memory for the queue header (1 page) plus the
 *      translation structure for offset -> page mappings.  Allocates physical
 *      pages for the queue (buffer area), and initializes the translation
 *      structure.
 *
 * Results:
 *      The VA on success, NULL otherwise.
 *
 * Side effects:
 *      Memory is allocated.
 *
 *-----------------------------------------------------------------------------
 */

VA
VMCI_AllocQueueKVA(uint64 size) // IN: size of queue (not including header)
{
   const uint64 numPages = CEILING(size, PAGE_SIZE);
   VMCIQueue *queue;

   queue = vmalloc(sizeof *queue + numPages * sizeof queue->page[0]);
   if (queue) {
      uint64 i;

      /*
       * Allocate physical pages, they will be mapped/unmapped on demand.
       */
      for (i = 0; i < numPages; i++) {
         queue->page[i] = alloc_pages(GFP_KERNEL, 0); /* One page. */
         if (!queue->page[i]) {
            /*
             * Free all pages allocated.
             */
            while (i) {
               __free_page(queue->page[--i]);
            }
            vfree(queue);
            queue = NULL;
            break;
         }
      }
   }
   return (VA)queue;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_FreeQueueKVA --
 *
 *      Frees kernel memory for a given queue (header plus translation
 *      structure).  Frees all physical pages that held the buffers for this
 *      queue.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Memory is freed.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCI_FreeQueueKVA(VA va,       // IN:
                  uint64 size) // IN: size of queue (not including header)
{
   VMCIQueue *queue = (VMCIQueue *)va;

   if (queue) {
      uint64 i;

      for (i = 0; i < CEILING(size, PAGE_SIZE); i++) {
         __free_page(queue->page[i]);
      }
      vfree(queue);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_AllocPPNSet --
 *
 *      Allocates two list of PPNs --- one for the pages in the produce queue,
 *      and the other for the pages in the consume queue. Intializes the list
 *      of PPNs with the page frame numbers of the KVA for the two queues (and
 *      the queue headers).
 *
 * Results:
 *      Success or failure.
 *
 * Side effects:
 *      Memory may be allocated.
 *
 *-----------------------------------------------------------------------------
 */

int
VMCI_AllocPPNSet(VA produceVA,           // IN:
                 uint64 numProducePages, // IN: for queue plus header
                 VA consumeVA,           // IN:
                 uint64 numConsumePages, // IN: for queue plus header
                 PPNSet *ppnSet)         // OUT:
{
   VMCIPpnList producePPNs;
   VMCIPpnList consumePPNs;
   uint64 i;
   VMCIQueue *produceQ = (VMCIQueue *)produceVA;
   VMCIQueue *consumeQ = (VMCIQueue *)consumeVA;

   if (!produceVA || !numProducePages || !consumeVA || !numConsumePages ||
       !ppnSet) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   if (ppnSet->initialized) {
      return VMCI_ERROR_ALREADY_EXISTS;
   }

   producePPNs =
      VMCI_AllocKernelMem(numProducePages * sizeof *producePPNs,
                          VMCI_MEMORY_NORMAL);
   if (!producePPNs) {
      return VMCI_ERROR_NO_MEM;
   }

   consumePPNs =
      VMCI_AllocKernelMem(numConsumePages * sizeof *consumePPNs,
                          VMCI_MEMORY_NORMAL);
   if (!consumePPNs) {
      VMCI_FreeKernelMem(producePPNs, numProducePages * sizeof *producePPNs);
      return VMCI_ERROR_NO_MEM;
   }

   producePPNs[0] = VMCIKVaToMPN(produceVA);
   for (i = 1; i < numProducePages; i++) {
      unsigned long pfn;

      producePPNs[i] = pfn = page_to_pfn(produceQ->page[i - 1]);

      /*
       * Fail allocation if PFN isn't supported by hypervisor.
       */

      if (sizeof pfn > sizeof *producePPNs &&
          pfn != producePPNs[i]) {
         goto ppnError;
      }
   }
   consumePPNs[0] = VMCIKVaToMPN(consumeVA);
   for (i = 1; i < numConsumePages; i++) {
      unsigned long pfn;

      consumePPNs[i] = pfn = page_to_pfn(consumeQ->page[i - 1]);

      /*
       * Fail allocation if PFN isn't supported by hypervisor.
       */

      if (sizeof pfn > sizeof *consumePPNs &&
          pfn != consumePPNs[i]) {
         goto ppnError;
      }
   }

   ppnSet->numProducePages = numProducePages;
   ppnSet->numConsumePages = numConsumePages;
   ppnSet->producePPNs = producePPNs;
   ppnSet->consumePPNs = consumePPNs;
   ppnSet->initialized = TRUE;
   return VMCI_SUCCESS;

ppnError:
   VMCI_FreeKernelMem(producePPNs, numProducePages * sizeof *producePPNs);
   VMCI_FreeKernelMem(consumePPNs, numConsumePages * sizeof *consumePPNs);
   return VMCI_ERROR_INVALID_ARGS;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_FreePPNSet --
 *
 *      Frees the two list of PPNs for a queue pair.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCI_FreePPNSet(PPNSet *ppnSet) // IN:
{
   ASSERT(ppnSet);
   if (ppnSet->initialized) {
      /* Do not call these functions on NULL inputs. */
      ASSERT(ppnSet->producePPNs && ppnSet->consumePPNs);
      VMCI_FreeKernelMem(ppnSet->producePPNs,
                         ppnSet->numProducePages * sizeof *ppnSet->producePPNs);
      VMCI_FreeKernelMem(ppnSet->consumePPNs,
                         ppnSet->numConsumePages * sizeof *ppnSet->consumePPNs);
   }
   memset(ppnSet, 0, sizeof *ppnSet);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCI_PopulatePPNList --
 *
 *      Populates the list of PPNs in the hypercall structure with the PPNS
 *      of the produce queue and the consume queue.
 *
 * Results:
 *      VMCI_SUCCESS.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

int
VMCI_PopulatePPNList(uint8 *callBuf,       // OUT:
                     const PPNSet *ppnSet) // IN:
{
   ASSERT(callBuf && ppnSet && ppnSet->initialized);
   memcpy(callBuf, ppnSet->producePPNs,
          ppnSet->numProducePages * sizeof *ppnSet->producePPNs);
   memcpy(callBuf + ppnSet->numProducePages * sizeof *ppnSet->producePPNs,
          ppnSet->consumePPNs,
          ppnSet->numConsumePages * sizeof *ppnSet->consumePPNs);

   return VMCI_SUCCESS;
}


#ifdef __KERNEL__


/*
 *-----------------------------------------------------------------------------
 *
 * __VMCIMemcpyToQueue --
 *
 *      Copies from a given buffer or iovector to a VMCI Queue.  Uses
 *      kmap()/kunmap() to dynamically map/unmap required portions of the queue
 *      by traversing the offset -> page translation structure for the queue.
 *      Assumes that offset + size does not wrap around in the queue.
 *
 * Results:
 *      Zero on success, negative error code on failure.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

int
__VMCIMemcpyToQueue(VMCIQueue *queue,   // OUT:
                    uint64 queueOffset, // IN:
                    const void *src,    // IN:
                    size_t size,        // IN:
                    Bool isIovec)       // IN: if src is a struct iovec *
{
   size_t bytesCopied = 0;

   while (bytesCopied < size) {
      uint64 pageIndex = (queueOffset + bytesCopied) / PAGE_SIZE;
      size_t pageOffset = (queueOffset + bytesCopied) & (PAGE_SIZE - 1);
      void *va = kmap(queue->page[pageIndex]);
      size_t toCopy;

      ASSERT(va);
      if (size - bytesCopied > PAGE_SIZE - pageOffset) {
         /* Enough payload to fill up from this page. */
         toCopy = PAGE_SIZE - pageOffset;
      } else {
         toCopy = size - bytesCopied;
      }

      if (isIovec) {
         struct iovec *iov = (struct iovec *)src;
         int err;

         /* The iovec will track bytesCopied internally. */
         err = memcpy_fromiovec((uint8 *)va + pageOffset, iov, toCopy);
         if (err != 0) {
            kunmap(queue->page[pageIndex]);
            return err;
         }
      } else {
         memcpy((uint8 *)va + pageOffset, (uint8 *)src + bytesCopied, toCopy);
      }

      bytesCopied += toCopy;
      kunmap(queue->page[pageIndex]);
   }

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * __VMCIMemcpyFromQueue --
 *
 *      Copies to a given buffer or iovector from a VMCI Queue.  Uses
 *      kmap()/kunmap() to dynamically map/unmap required portions of the queue
 *      by traversing the offset -> page translation structure for the queue.
 *      Assumes that offset + size does not wrap around in the queue.
 *
 * Results:
 *      Zero on success, negative error code on failure.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

int
__VMCIMemcpyFromQueue(void *dest,             // OUT:
                      const VMCIQueue *queue, // IN:
                      uint64 queueOffset,     // IN:
                      size_t size,            // IN:
                      Bool isIovec)           // IN: if dest is a struct iovec *
{
   size_t bytesCopied = 0;

   while (bytesCopied < size) {
      uint64 pageIndex = (queueOffset + bytesCopied) / PAGE_SIZE;
      size_t pageOffset = (queueOffset + bytesCopied) & (PAGE_SIZE - 1);
      void *va = kmap(queue->page[pageIndex]);
      size_t toCopy;

      ASSERT(va);
      if (size - bytesCopied > PAGE_SIZE - pageOffset) {
         /* Enough payload to fill up this page. */
         toCopy = PAGE_SIZE - pageOffset;
      } else {
         toCopy = size - bytesCopied;
      }

      if (isIovec) {
         struct iovec *iov = (struct iovec *)dest;
         int err;

         /* The iovec will track bytesCopied internally. */
         err = memcpy_toiovec(iov, (uint8 *)va + pageOffset, toCopy);
         if (err != 0) {
            kunmap(queue->page[pageIndex]);
            return err;
         }
      } else {
         memcpy((uint8 *)dest + bytesCopied, (uint8 *)va + pageOffset, toCopy);
      }

      bytesCopied += toCopy;
      kunmap(queue->page[pageIndex]);
   }

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIMemcpyToQueue --
 *
 *      Copies from a given buffer to a VMCI Queue.
 *
 * Results:
 *      Zero on success, negative error code on failure.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

EXPORT_SYMBOL(VMCIMemcpyToQueue);

int
VMCIMemcpyToQueue(VMCIQueue *queue,   // OUT:
                  uint64 queueOffset, // IN:
                  const void *src,    // IN:
                  size_t srcOffset,   // IN:
                  size_t size)        // IN:
{
   return __VMCIMemcpyToQueue(queue, queueOffset,
                              (uint8 *)src + srcOffset, size, FALSE);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIMemcpyFromQueue --
 *
 *      Copies to a given buffer from a VMCI Queue.
 *
 * Results:
 *      Zero on success, negative error code on failure.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

EXPORT_SYMBOL(VMCIMemcpyFromQueue);

int
VMCIMemcpyFromQueue(void *dest,             // OUT:
                    size_t destOffset,      // IN:
                    const VMCIQueue *queue, // IN:
                    uint64 queueOffset,     // IN:
                    size_t size)            // IN:
{
   return __VMCIMemcpyFromQueue((uint8 *)dest + destOffset,
                                queue, queueOffset, size, FALSE);
}


/*
 *----------------------------------------------------------------------------
 *
 * VMCIMemcpyToQueueV --
 *
 *      Copies from a given iovec from a VMCI Queue.
 *
 * Results:
 *      Zero on success, negative error code on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

EXPORT_SYMBOL(VMCIMemcpyToQueueV);

int
VMCIMemcpyToQueueV(VMCIQueue *queue,      // OUT:
                   uint64 queueOffset,    // IN:
                   const void *src,       // IN: iovec
                   size_t srcOffset,      // IN: ignored
                   size_t size)           // IN:
{

   /*
    * We ignore srcOffset because src is really a struct iovec * and will
    * maintain offset internally.
    */
   return __VMCIMemcpyToQueue(queue, queueOffset, src, size, TRUE);
}


/*
 *----------------------------------------------------------------------------
 *
 * VMCIMemcpyFromQueueV --
 *
 *      Copies to a given iovec from a VMCI Queue.
 *
 * Results:
 *      Zero on success, negative error code on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

EXPORT_SYMBOL(VMCIMemcpyFromQueueV);

int
VMCIMemcpyFromQueueV(void *dest,              // OUT: iovec
                     size_t destOffset,       // IN: ignored
                     const VMCIQueue *queue,  // IN:
                     uint64 queueOffset,      // IN:
                     size_t size)             // IN:
{
   /*
    * We ignore destOffset because dest is really a struct iovec * and will
    * maintain offset internally.
    */
   return __VMCIMemcpyFromQueue(dest, queue, queueOffset, size, TRUE);
}

#endif


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIWellKnownID_AllowMap --
 *
 *      Checks whether the calling context is allowed to register for the given
 *      well known service ID.  Currently returns FALSE if the service ID is
 *      within the reserved range and VMCI_PRIVILEGE_FLAG_TRUSTED is not
 *      provided as the input privilege flags.  Otherwise returns TRUE.
 *      XXX TODO access control based on host configuration information; this
 *      will be platform specific implementation.
 *
 * Results:
 *      Boolean value indicating access granted or denied.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
VMCIWellKnownID_AllowMap(VMCIId wellKnownID,           // IN:
                         VMCIPrivilegeFlags privFlags) // IN:
{
   if (wellKnownID < VMCI_RESERVED_RESOURCE_ID_MAX &&
       !(privFlags & VMCI_PRIVILEGE_FLAG_TRUSTED)) {
      return FALSE;
   }
   return TRUE;
}