/**
 *   Copyright 2015 Garrick Toubassi
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <iostream>

#include <mach-o/getsect.h>
#include <mach/mach_vm.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <unordered_map>

#include "gc.h"


static void debug_printf(const char *format, ...);
static inline uint64_t get_stack_pointer();
static void get_registers(void **buffer);

// Track the stack segment.  This is part of the "root set" that
// we scan during collections.
static void **stack_start;
static mach_vm_size_t stack_length;

// Track the data segment (e.g. initialized and uninitialized globals.
// This is part of the "root set" that we scan during collections.
static void **data_segment_start;
static mach_vm_size_t data_segment_length;

// Track every "managed" block we've allocated.  Maps the pointer to the
// size of the block.
typedef std::unordered_map<void *, size_t> heapmap;
static heapmap *allocations;

// Debugging constant to enforce an arbitrary heap size
static size_t max_heap_size = 0;
static size_t current_allocated = 0;

// Debugging constant to control whether we log verbosely during collections
static bool verbose_logging = false;

// Debugging constant to control whether we overwrite reclaimed blocks with 0xab bytes
static bool overwrite_reclaimed_blocks = false;


/**
 *  The main job of gc_init is to establish the "root set" used
 *  for our mark/sweep process.  We need to scan the live part
 *  of the stack, and thus we need to know where the stack ends,
 *  and we need to scan any global variables, so we need to know
 *  where the data segment lives.
 */
static void gc_init(void) {
  static bool gc_initialized = false;
  
  if (gc_initialized) {
    return;
  }
  gc_initialized = true;

  // Find where the stack starts/ends
  
  uint64_t rsp = get_stack_pointer();

  mach_msg_type_number_t info_cnt = sizeof (vm_region_basic_info_data_64_t);
  mach_port_t object_name;
  mach_vm_size_t size_info;
  mach_vm_address_t address_info = rsp;
  vm_region_basic_info_data_64_t info;
  kern_return_t kr = mach_vm_region(mach_task_self(), &address_info, &size_info, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &info_cnt, &object_name);
  
  if (kr) {
    std::cerr << "Error determining stack location " << kr;
    exit(1);
  }
  
  stack_start = (void **)address_info;
  stack_length = size_info;
  
  // Find where the data segment starts/ends
  
  // NOTE Release builds have data segments "slid" by a random amount
  // to prevent buffer overflow attacks (google ASLR randomization).
  // So the actual location of the data segment is that reported
  // in the segment->vmaddr + the "slide" of the image.
  
  const struct segment_command_64 *dataSeg = getsegbyname("__DATA");
  data_segment_start = (void **)(dataSeg->vmaddr + _dyld_get_image_vmaddr_slide(0));
  data_segment_length = dataSeg->vmsize;
  
  allocations = new heapmap;
  
  debug_printf("GC Stack: %p %lld\n", stack_start, stack_length);
  debug_printf("GC Data:  %p %lld\n", data_segment_start, data_segment_length);
}

void *internal_alloc(size_t size) {
  // Enforce max_heap_size
  if (max_heap_size > 0 && current_allocated + size > max_heap_size) {
    return 0;
  }
  return calloc(1, size);
}

void *gc_alloc(size_t size) {
  gc_init();
  
  void *ptr = internal_alloc(size);
  if (!ptr) {
    gc_collect();
    ptr = internal_alloc(size);
  }
  
  if (ptr) {
    (*allocations)[(void**)ptr] = size;
    current_allocated += size;
  }
  return ptr;
}

static void gc_collect_scan_block(void *start, size_t length, heapmap *marked) {
  // We scan the block assumming all pointers are pointer (8 byte) aligned.

  void **end = (void **)(((uint64_t)start) + length);
  for (void** p = (void **)start; p < end; p++) {
    // Check if this looks like a pointer that we've allocated
    auto is_valid_allocation = allocations->find((void **)*p);
    if (is_valid_allocation != allocations->end()) {
      // We have a valid allocation, scan this block

      debug_printf("GC Valid block at %p (@%p) (%lld bytes)\n", is_valid_allocation->first, p, is_valid_allocation->second);

      auto has_visited = marked->find((void **)*p);
      if (has_visited == marked->end()) {
        
        debug_printf("GC Valid, unmarked block at %p (@%p) (%lld bytes)\n", is_valid_allocation->first, p, is_valid_allocation->second);
        
        // We haven't visited this block yet, so lets "mark" it and
        // recursively scan its ocntent;s
        marked->insert(*is_valid_allocation);
        gc_collect_scan_block(is_valid_allocation->first, is_valid_allocation->second, marked);
      }
    }
  }
}

/**
 * Implements a simple conservative mark and sweep over the set of blocks stored in
 * the allocations map.  We start the trace from the root set which is made up of three
 * sets: registers, active stack, and data segment.  We scan each of those areas for
 * anything that matches a block in our allocations map.  If found we "mark" that block
 * by adding it to the "marked" map, and then we recursively scan that block.
 */
void gc_collect(void) {
  gc_init();
  
  // Mark
  debug_printf("GC START\n");
  
  heapmap *marked = new heapmap;

  // Make sure all the registers get reified onto the stack so if they
  // are pointing to any memory we get them.
  debug_printf("GC Marking registers\n");
  void **registers = (void **)calloc(sizeof(void *), 15);
  get_registers(registers);
  gc_collect_scan_block(registers, sizeof(void *) * 15, marked);
  free(registers);

  debug_printf("GC Marking stack\n");
  uint64_t curr_stack = get_stack_pointer();
  // We don't scan the entire stack, just the part in use.
  gc_collect_scan_block((void **)curr_stack, (size_t)((int64_t)stack_start + stack_length - curr_stack), marked);
  
  debug_printf("GC Marking data segment\n");
  gc_collect_scan_block(data_segment_start, data_segment_length, marked);
  
  // Sweep - free everything in allocations that has not been "marked" (is not in the marked map)
  debug_printf("GC Sweeping garbage\n");
  size_t total_swept = 0;
  for (const auto &allocation : *allocations) {
    if (marked->find(allocation.first) == marked->end()) {
      
      debug_printf("GC Sweeping %p (%lld bytes)\n", allocation.first, allocation.second);
      
      // For debugging
      if (overwrite_reclaimed_blocks) {
        memset(allocation.first, 0xab, allocation.second);
      }
      
      free(allocation.first);
      total_swept += allocation.second;
    }
  }
  
  current_allocated -= total_swept;
  
  debug_printf("GC Swept %lld bytes\n", total_swept);
  
  delete allocations;
  allocations = marked;
  
  debug_printf("GC DONE\n");
}

void gc_debug_set_max_heap(size_t size) {
  max_heap_size = size;
}

void gc_debug_enable_verbose_logging(bool flag) {
  verbose_logging = flag;
}

void gc_debug_overwrite_reclaimed_blocks(bool flag) {
  overwrite_reclaimed_blocks = flag;
}

static void debug_printf(const char *format, ...) {
  if (!verbose_logging) {
    return;
  }
  va_list args;
  va_start (args, format);
  vprintf (format, args);
  va_end (args);
}


static inline uint64_t get_stack_pointer() {
  uint64_t rsp;
  __asm__ __volatile__("movq %%rsp, %0\n\t" : "=r"(rsp));
  return rsp;
}

static void get_registers(void **buffer) {
  __asm__ __volatile__("movq %%rax, %0\n\t" : "=r"(buffer[0]));
  __asm__ __volatile__("movq %%rbx, %0\n\t" : "=r"(buffer[1]));
  __asm__ __volatile__("movq %%rcx, %0\n\t" : "=r"(buffer[2]));
  __asm__ __volatile__("movq %%rdx, %0\n\t" : "=r"(buffer[3]));
  __asm__ __volatile__("movq %%rsi, %0\n\t" : "=r"(buffer[4]));
  __asm__ __volatile__("movq %%rdi, %0\n\t" : "=r"(buffer[5]));
  __asm__ __volatile__("movq %%r8, %0\n\t"  : "=r"(buffer[6]));
  __asm__ __volatile__("movq %%r9, %0\n\t"  : "=r"(buffer[7]));
  __asm__ __volatile__("movq %%r10, %0\n\t" : "=r"(buffer[8]));
  __asm__ __volatile__("movq %%r11, %0\n\t" : "=r"(buffer[9]));
  __asm__ __volatile__("movq %%r12, %0\n\t" : "=r"(buffer[10]));
  __asm__ __volatile__("movq %%r13, %0\n\t" : "=r"(buffer[11]));
  __asm__ __volatile__("movq %%r14, %0\n\t" : "=r"(buffer[12]));
  __asm__ __volatile__("movq %%r15, %0\n\t" : "=r"(buffer[13]));
}

