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
#ifndef GC_H
#define GC_H

#include <cstddef>


/**
 *  Moral equivalent of malloc.  No need to call free!  Invokes
 *  gc_collect automatically if necessary but if not enough
 *  memory can be reclaimed will return 0.
 */
void *gc_alloc(size_t size);

/**
 *  You shouldn't need to call this, it is here for debugging/testing purposes.
 */
void gc_collect(void);

/**
 *  For debugging/testing garbage collection.  Will act
 *  like the underlying heap is only size large.  Will thus
 *  force more GC and potential returning of null from gc_alloc.
 */
void gc_debug_set_max_heap(size_t size);

/**
 *  If set to true, will dump to stdout vebose info on the mark/sweep
 *  collection process, as well as location of the data and stack segments
 */
void gc_debug_enable_verbose_logging(bool flag);

/**
 *  Writes 0xab over all reclaimed (swept, freed) blocks to ease debugging
 *  issues where blocks are being unexpectedly freed
 */
void gc_debug_overwrite_reclaimed_blocks(bool flag);


#endif
