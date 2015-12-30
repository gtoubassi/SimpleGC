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
#include <iostream>
#include <cstdarg>
#include "gc.h"

#define TEST_MAX_HEAP 8*1024*1024

// Scramble pointers so we can test that GC collects these blocks by later
// unscrambling them and checking that they were overwritten with 0xab.
#define SCRAMBLE(p) ((typeof(p))(((uint64_t)(p)) + 1));
#define UNSCRAMBLE(p) ((typeof(p))(((uint64_t)(p)) - 1));

void *gc_alloc_or_die(size_t size) {
  void *p = gc_alloc(size);
  if (!p) {
    fprintf(stderr, "Failed to allocate %ld bytes\n", size);
  }
  return p;
}

static int testPassed = 0, testFailed = 0;
void assertTrue(int value, int line, const char *format, ...) {
  if (!value) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    
    fprintf(stderr, " (line %d)\n", line);
    testFailed++;
  }
  else {
    testPassed++;
  }
}

void *testGCNotCollectingLocallyReferencedBlock() {
  // Allocate a block, and make sure it doesn't get garbage collected
  void *p = gc_alloc_or_die(1024);
  printf("Allocated %p (@%p) (line %d)\n", p, &p, __LINE__);
  gc_collect();
  assertTrue('\0' == *(char *)p, __LINE__, "Block %p was unexpectedly collected", p);
  return SCRAMBLE(p);
}

void testGCCollectsLocallyUnreferencedBlock(void *scrambled_p) {
  void *p = gc_alloc_or_die(1024);
  printf("Allocated %p (@%p) (line %d)\n", p, &p, __LINE__);
  gc_collect();
  void *unscrambled_p = UNSCRAMBLE(scrambled_p);
  assertTrue('\xab' == *(char *)unscrambled_p, __LINE__, "Block %p unexpectedly NOT collected", unscrambled_p);
  assertTrue('\x0' == *(char *)p, __LINE__, "Block %p was unexpectedly collected", p);
}

static void *globalPtr;
void testGCNotCollectingGloballyReferencedBlock() {
  // Allocate a block, and make sure it doesn't get garbage collected
  globalPtr = gc_alloc_or_die(1024);
  printf("Allocated %p (@%p) (line %d)\n", globalPtr, &globalPtr, __LINE__);
  gc_collect();
  assertTrue('\0' == *(char *)globalPtr, __LINE__, "Block %p was unexpectedly collected", globalPtr);
  globalPtr = SCRAMBLE(globalPtr);
}

void testGCCollectsGloballyUnreferencedBlock() {
  gc_collect();
  void *unscrambled_p = UNSCRAMBLE(globalPtr);
  assertTrue('\xab' == *(char *)unscrambled_p, __LINE__, "Block %p unexpectedly NOT collected", unscrambled_p);
}

void testLinkList() {
  void **head = (void **)gc_alloc_or_die(sizeof(void *));
  *head = gc_alloc_or_die(sizeof(void *));
  gc_collect();
  assertTrue('\xab' != *(char *)head, __LINE__, "Block %p unexpectedly collected", head);
  assertTrue('\xab' != *(char *)*head, __LINE__, "Block %p unexpectedly collected", *head);
}

void testChurnBeyondHeap() {
  for (int i = 0; i < TEST_MAX_HEAP/1024 + 1024*10; i++) {
    gc_alloc_or_die(1024);
  }
  assertTrue(1, __LINE__, "Will crash if it fails");
}

/**
 * In order to get reproducible test results we need to zero out the stack.
 * Quite often pointers that we expect to be collected end up in temporary variables
 * that end up on the stack.  They would eventually get collected but to have a
 * reproducible set of test cases we avoid such ambiguity.
 */
void clearStack() {
  memset(alloca(1024), 0, 1024);
}

int main(int argc, const char * argv[]) {
  gc_debug_overwrite_reclaimed_blocks(true);
  gc_debug_enable_verbose_logging(true);
  gc_debug_set_max_heap(TEST_MAX_HEAP); // 8mb

  void *scrambled_p = testGCNotCollectingLocallyReferencedBlock();
  clearStack();
  
  // Allocate another one and make sure the first one got collected
  testGCCollectsLocallyUnreferencedBlock(scrambled_p);
  clearStack();
  
  testGCNotCollectingGloballyReferencedBlock();
  clearStack();
  
  testGCCollectsGloballyUnreferencedBlock();
  clearStack();
  
  testLinkList();
  clearStack();
  
  // Allocate way more then the 8mb heap
  testChurnBeyondHeap();
  clearStack();

  printf("%d passed, %d failed\n", testPassed, testFailed);
  
  return 0;
}

