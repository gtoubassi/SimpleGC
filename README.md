# SimpleGC 

SimpleGC is a simple, conservative garbage collector for the C language which is hacked up quickly merely to satisfy the curiosity of the author.  It shouldn't be used to do any real work.

It ended up being largely an exercise in learning how to locate the in memory data and stack segments of the executable.  This took many hours of stumbling around mainly because what worked fine in a Debug build failed to work on a Release build due to the fact that the memory address reported for the data segment is actually "slid" by a random amount for security purposes (see [Address Space Layout Randomization](https://en.wikipedia.org/wiki/Address_space_layout_randomization)).  I figured out the key OSX api for determining the "slide" by looking at the Boehm-Demers-Weiser garbage collector [source](https://github.com/ivmai/bdwgc/blob/aeaa599a73601ec5c29d900f0f5f111dfd8ae06d/dyn_load.c).  Thanks bdwgc!

### How to use it

Instead of using `malloc(3)/free(3)`, use `gc_alloc(size_t)` and you are done!

### How it works

This is a simple mark/sweep collector.  Calls to gc_alloc more or less pass through to malloc(3), and are stored in an internal "heap map" hashtable before being returned.  If malloc return 0 (implying we are out of heap), we trigger a collection (gc_collect()) before trying again.

gc_collect does a simple graph traversal of the heap starting with the "root set".  The root set are all pieces of memory that can reference into the heap.  In other words, any reference to a piece of allocated memory that isn't sitting inside another piece of allocated memory.  This collector uses three things as the root set: registers, the active stack, and the __DATA segment (globals).  Those areas are scanned looking for anything that *could* be a pointer to a heap block (by checking if it is in the hash), and if so considers it a valid reference, marks the block by adding it to a second hashtable, and continues scanning inside that block recursively.  If a piece of memory coincidentally looks like a valid pointer we will assume it is a valid reference; this is the definition of a conservative collector.

After the mark phase (heap traversal), we perform the "sweep", i.e. we iterate through the "heap map" containing all allocated blocks, and free the ones that were not "marked" (put into the secondary hashtable).  We then consider the secondary hashtable the new "heap map", freeing the old "heap map".

### Whats wrong with this collector

To name a few things:

   1. Not multi threaded
   2. Not at all sure if my root set is complete
   3. Doesn't work for shared libraries.  The gc code must be statically linked.
   4. Will collect any block of memory that is referenced only by an internal pointer.  Not sure if collectors like the Boehm collector handle this case but I can imagine some C programs getting tricky and keeping pointers to blocks that are a fixed offset into the actual allocated block (which can be inverted for purposes of freeing).
   5. Each collection touches ALL live memory, which is generally slow, and also interacts poorly with the virtual memory system (basically none of your active heap can be effectively paged to disk!).
   6. Many other things I'm sure...

### How might a real conservative collector work?

Our basic challenge is figuring out how to not have to traverse all live memory on every collection.  Assuming we are stuck with allocating blocks that we can't later remove (as might be done in a copying-compacting collector), the approach I would take would be to:

   1. Instead of allocating memory out of one big underlying heap, allocate out of many "mini heaps".  Each mini heap would be 1 or a small number of virtual memory pages.  As mini heaps are exhausted, new ones are created and allocated from.  Any allocation is made out of the most recently created mini heap with available space.  When no available space exists in any mini heap, we must collect (see below).

   2. "Full" collection is basically the same as it is in this prototype.  Do a full mark/sweep starting from the root set.  In addition to the above described mark/sweep process, we would also track "inter heap" references between each mini heap.  Meaning anything that looks like a pointer from one mini heap to another mini heap would be recorded as an "outgoing" reference from the first mini heap.  Essentially each mini heap has a list of outgoing references as associated metadata.

   3. The first collection of the heap would be a full collection as described in #2.  In addition at the end of the collection we'd mark all virtual memory pages for all mini heaps as "read only" (via mprotect(2) or similar), and install a signal handler to catch any attempt to modify said memory (via a SIGSEGV).  The signal handler will check any SIGSEGV associated with an address that is in a mini heap, and if it is a valid access it will simply change the memory protections of the page to be read/write, and return, which will reexecute the offending instruction.  In this way we will effectively be keeping track of which mini heaps were "dirtied" since the last collection.

   4. Subsequent collections will be "incremental" and proceed roughly as follows.  Look for all mini heaps that are "dirty", meaning they have writable pages.  For those mini heaps, rebuild the inter heap references list, taking special note of any inter heap references that were removed from the last list (meaning mini heap A refered to min heap B but no longer refers to it).  Consider any mini heap that had a reference to it "lost" also "dirty", though in this case we don't need to rebuild the inter heap references since the page itself hasn't changed.  Now, perform a collection using the default root set of registers/stack/globals, but also add any references pointing *into* a dirty mini heap.  During the mark phase any reference that is traversed into a non dirty page can be ignored.  Meaning we will only traverse into dirty pages.

What the above should do is allow us to only mark/sweep the most recent mini heap (or a small number of mini heaps) with the cost of some added bookkeeping.  Note all mini heap metadata is still touched on every collection but that should be small relative to the mini heap itself.  For example if the app is in a tight loop churning objects it should only have to scan and collect out of the most recent mini heap because it should be the only dirty heap to consider.  Although the mark phase starts with a *larger* root set, most traversals will "short out" because they will point to non dirty mini heaps and thus do not need to be followed.

I have no idea to what degree the above scheme is practical, but the exercise of implementing the naive collector in this project got me thinking and it certainly represents my best guess of where I'd expect to go if I was trying to make an industrial strength conservative collector (and assuming I couldn't avail myself of the excellent prior art such as the Boehm-Demers-Weiser collector).





