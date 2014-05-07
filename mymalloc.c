#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <inttypes.h>

/////////////////////////
// CONSTANTS/TYPES
/////////////////////////

#define NUMBUCKETS 32 // can't go too far above bit size... be pessimistic
#define MIN_PHYSICAL_BLOCK 0x100   // whatever

const unsigned long LOWESTBIT = (1ul);  // & this with something to find if block is allocated
const unsigned long HIGHBITS = (~(1ul));

#define MASKED_VALUE(n) (n & HIGHBITS)
#define MASKED_PTR(p) ( (blockHeader*)((uintptr_t)p & HIGHBITS) )

const int ALIGNMENT = 16u;  // for 64-bit, apparently

/*
   Header for a memory block.
   Whether block is allocated is lowest bit of logicalPrev
   Whether there's a previous physical block to merge with is
   stored as the lowest bit of size.
   Whether there's a next physical block is stored as lowest bit
   of footer size.
   For all these values, you have to & them with HIGHBITS to get the 
   value to do actual math with.
*/
typedef struct blockHeader_t {
    struct blockHeader_t* logicalPrev;  // previous in size-bucket free-list
    struct blockHeader_t* logicalNext;  // next in,.. you know
    size_t size; // payload size!
    size_t pad;    // for 16-byte alignment
} blockHeader; // end of data is the size footer, also size_t

typedef struct blockFooter_t {
    size_t pad; // we want this struct's size to be a multiple of ALIGNMENT (16),
                // and we may as well have this otherwise useless eight bytes at the
                // front as a shield against heap corruption
    size_t size;
} blockFooter;

#define BLOCK_OVERHEAD (sizeof(blockHeader) + sizeof(blockFooter))

typedef struct memoryChunk_t {
    size_t size; // size of memory AFTER this struct. again, payload
    struct memoryChunk_t* older; // put highest-address/latest-allocated chunks at end, so they're
                                 // easy to muck with if next sbrk is contiguous
} memoryChunk;

#define CHUNK_OVERHEAD (sizeof(memoryChunk))

const int FIRSTBUCKETCEILING = (16); //1*ALIGNMENT;

/////////////////////////
// GLOBAL VARS
/////////////////////////

/*
   Linked list of physical memory chunks, with embedded metadata like
   the memory blocks.
*/
memoryChunk* latestPhysicalChunk = 0;

/*
   Buckets are allocated like so: first bucket/freelist is
   for blocks of payload size 0 to FIRSTBUCKETCEILING. next is
   from (FBC, 2*FBC], (2*FBC, 4*FBC], etc., except the last one is
   just whatever won't fit in previous buckets. Just to repeat,
   use the payload size of the object

   Array is of bucket objects so when first object wants to unlink itself
   it doesn't have to check special case of being at front. 

   CRITICAL that everything is 0-initialized
*/
blockHeader buckets[NUMBUCKETS] = {{0,0,0,0}};


/////////////////////////
// FUNCTIONS
/////////////////////////

size_t mylog2(size_t foo) {
    int n = 0;
    for(; foo; n++) {
        foo = foo >> 1;
    }
    return n-1;
}

size_t pow2(size_t foo) {
    return 1 << foo;
}

size_t next_aligned_value(size_t n) {
    // the smallest value x >= n so x & ALIGNMENT = 0
    if(n % ALIGNMENT) {
        // there are low bits
        return ((n+ALIGNMENT)/ALIGNMENT)*ALIGNMENT;
    }
    return n;
}

int getBucket(size_t s) {
    // think of this as a hash function
    size_t lastCeiling = FIRSTBUCKETCEILING*pow2(NUMBUCKETS - 2);
    if(s > lastCeiling) {
        return NUMBUCKETS-1;
    }
    size_t prelog = 2*(s-1)/FIRSTBUCKETCEILING;
    if(prelog == 0) {
        return 0;
    }
    return mylog2(prelog);
}

blockHeader* initNewBlock(void* blockPosition) {
    blockHeader* b = (blockHeader*)blockPosition;
    b->logicalPrev = 0;
    b->logicalNext = 0;
    b->size = 0;
    // caller must set footer, bit flags
    return b;
}

void* getBlockPayload(blockHeader* block) {
    return (void*)block + sizeof(blockHeader);
}

blockFooter* getFooter(blockHeader* block) {
    // only assumes header size is valid
    return (blockFooter*)(getBlockPayload(block) + MASKED_VALUE(block->size));
}

// accessors for bitfields of blockHeader
void setHasPhysicalPrev(blockHeader* block, int has){
    if(has) {
        block->size = block->size | LOWESTBIT;
    } else {
        // unset
        block->size = block->size & HIGHBITS; // only keep high bits
    }
}
int getHasPhysicalPrev(blockHeader* block) {
    return !!(block->size & LOWESTBIT);
}

void setHasPhysicalNext(blockHeader* block, int has){
    blockFooter* foot = getFooter(block);
    if(has) {
        foot->size = foot->size | LOWESTBIT;
    } else {
        foot->size = foot->size & HIGHBITS;
    }
}
int getHasPhysicalNext(blockHeader* block) {
    blockFooter* foot = getFooter(block);
    return !!(foot->size & LOWESTBIT);
}

void setAllocated(blockHeader* block, int allocated) {
    if(allocated) {
        block->logicalPrev = (blockHeader*)((uintptr_t)block->logicalPrev | LOWESTBIT);
    } else {
        block->logicalPrev = (blockHeader*)((uintptr_t)block->logicalPrev & HIGHBITS);
    }
}
int isAllocated(blockHeader* block) {
    return !!((uintptr_t)block->logicalPrev & LOWESTBIT);
}


void setBlockSize(blockHeader* block, size_t s) {
    // make darn sure there's room for size_t + sizeof(blockFooter)
    // bytes after the block header
    // preserves hasPhysicalPrev bit, does nothing for physNext
    int hadPhysPrev = getHasPhysicalPrev(block);
    block->size = s;
    setHasPhysicalPrev(block, hadPhysPrev);
    blockFooter* footer = getFooter(block);
    footer->size = s;
    // we don't know how to set hasPhysicalNext in this fn, must be done by caller
}

void* getChunkPayload(memoryChunk* chunk) {
    return (void*)chunk + sizeof(memoryChunk);
}

blockHeader* newMemoryChunk(size_t minSize) {
    // return a block with payload size at least minSize
    // returns null if failed to allocate
    // leave new physical chunk in global list
    // note that minSize is a payload size.
    // create new block with sbrk
    void* chunkStart = 0;    // the return from sbrk
    // make sure minsize is aligned, and will still be big enough if
    // the sbrk address has to be re-aligned
    minSize = next_aligned_value(minSize) + ALIGNMENT;
    minSize += BLOCK_OVERHEAD + CHUNK_OVERHEAD;
    size_t allocationSize;
    // try the largest of MIN_PHYSICAL_BLOCK and minSize
    if(minSize < MIN_PHYSICAL_BLOCK) {
        allocationSize = MIN_PHYSICAL_BLOCK;
    } else {
        // minSize >= MIN_PHYSICAL_BLOCK
        allocationSize = minSize;
    }
    chunkStart = sbrk(allocationSize);
    if(!chunkStart) {
        // crap, failed to alloc
        if(allocationSize > minSize) {
            // we might be able to salvage by only allocating min size
            chunkStart = sbrk(minSize);
            allocationSize = minSize;
            if(!chunkStart) {
                // that's it
                return 0;
            }
        } else {
            // no recourse, we already tried the smallest possible chunk
            return 0;
        }
    };
    // chunkStart != 0, with length allocationSize
    // don't assume sbrk is aligned (can move chunkStart up by <= ALIGNMENT - 1,
    // so that's why we inflate the minSize
    printf("sbrk'd %p with size %#lx\n", chunkStart, allocationSize);
    void* chunkEnd = chunkStart + allocationSize; // this one we'll round down to alignment
    chunkStart = (void*)next_aligned_value((uintptr_t)chunkStart);
    chunkEnd = (void*)( ((uintptr_t)chunkEnd)/ALIGNMENT*ALIGNMENT ); // round down to next *lowest* multiple of ALIGNMENT
    allocationSize = chunkEnd - chunkStart; // real, usable, aligned space
    assert(allocationSize >= minSize);
    assert(allocationSize % ALIGNMENT == 0);
    // coallesce with previously allocated chunk if possible
    if(latestPhysicalChunk &&
            (getChunkPayload(latestPhysicalChunk) + latestPhysicalChunk->size) == chunkStart) {
        // to extend the old block with new data, we'll need to save it
        // footer is at last ALIGNMENT bytes of latestPhysicalChunk
        printf("  merging with last phys chunk\n");
        blockFooter* oldFooter = getChunkPayload(latestPhysicalChunk) +
                            latestPhysicalChunk->size -
                            sizeof(blockFooter);
        blockHeader* oldBlock = (blockHeader*)((void*)oldFooter
                                              - MASKED_VALUE(oldFooter->size)
                                              - sizeof(blockHeader));
        // extend the physical chunk
        latestPhysicalChunk->size += allocationSize; // new memory is pure payload
        // now extend the block if free, otherwise create a new one
        if(isAllocated(oldBlock)) {
            // we need a new block anyway
            // it does have a physical prev, no physical next
            blockHeader* newBlock = initNewBlock(chunkStart);
            // set the footer
            setBlockSize(newBlock, allocationSize - CHUNK_OVERHEAD - BLOCK_OVERHEAD);
            // physical adjacency stuff
            setHasPhysicalPrev(newBlock, 1);
            setHasPhysicalNext(newBlock, 0);
            setHasPhysicalNext(oldBlock, 1); // now it does, anyway
            // that's all, it's ready to split (but not logically linked)
            return newBlock;
        } else {
            // addition preserves low bits (including phys prev flag)
            oldBlock->size += allocationSize;
            getFooter(oldBlock)->size = oldBlock->size;
            // oldBlock doesn't have physical next, so we're done
            return oldBlock;
        }
    } else {
        // we've got to do it all fresh
        printf("  making new phys chunk\n");
        memoryChunk* newChunk = (memoryChunk*)chunkStart;
        newChunk->older = latestPhysicalChunk;
        newChunk->size = allocationSize - CHUNK_OVERHEAD;
        latestPhysicalChunk = newChunk;
        blockHeader* newBlock = initNewBlock(getChunkPayload(newChunk));
        setBlockSize(newBlock, newChunk->size - BLOCK_OVERHEAD);
        return newBlock;
    }
    // otherwise, put it on free list
}

void logicalUnlinkBlock(blockHeader* block) {
    // okay that this wipes out allocation bit, since this is only
    // called on unallocated blocks
    // logicalPrev is always valid, since we start with anchor elements in
    // buckets array, and we never try to unlink those
    assert(MASKED_PTR(block->logicalPrev));
    MASKED_PTR(block->logicalPrev)->logicalNext = block->logicalNext;
    if(block->logicalNext) {
        block->logicalNext->logicalPrev = MASKED_PTR(block->logicalPrev);
    }
}

void logicalLinkBlock(blockHeader* newPrev, blockHeader* block) {
    // insert block after newPrev. newPrev -> block -> newPrev's old next
    block->logicalPrev = newPrev;
    block->logicalNext = newPrev->logicalNext;
    if(block->logicalNext) {
        block->logicalNext->logicalPrev = block;
    }
    newPrev->logicalNext = block;
}

void reBucketBlock(blockHeader* block) {
    // must already be unlinked
    // most likely, block has been newly split (or coallesced)
    int destBucket = getBucket(MASKED_VALUE(block->size));
    // go through destBucket and find correct location
    blockHeader* currBlock = &buckets[destBucket];
    while(currBlock->logicalNext &&
          (MASKED_VALUE(currBlock->logicalNext->size) < MASKED_VALUE(block->size)) ) {
        currBlock = currBlock->logicalNext;
    }
    // currBlock is non-null by construction
    // insert as currBlock->logicalNext
    logicalLinkBlock(currBlock, block);

    // hypothetically nicer algorithm
    //if(currBucketIndex != destBucket) {
    //    // unlink it
    //    // go through destBucket and insert in correct location
    //} else {
    //    // in correct bucket
    //    // move up or down appropriately
    // might require size of bucket heads to be initialized to floor-1
    // so size comparisons don't need special cases at ends
}

void splitBlock(blockHeader* block, size_t s) {
    assert(MASKED_VALUE(block->size) >= s);
    assert(s % ALIGNMENT == 0);
    // TODO: actually do this, and rebucket leftovers, assuming it's actually
    // possible
    // after carving out a new payload, there needs to be enough left
    if(MASKED_VALUE(block->size) < (s + BLOCK_OVERHEAD)) {
        // don't even bother
        return;
    }
    size_t leftover_bytes = MASKED_VALUE(block->size) - s - BLOCK_OVERHEAD; // amount that would actually be left for payload
    // leftover_bytes is at least ALIGNMENT, so proceed
    int hadPhysNext = getHasPhysicalNext(block);
    setBlockSize(block, s);
    blockHeader* newBlock = initNewBlock((void*)block + s + BLOCK_OVERHEAD);
    setBlockSize(newBlock, leftover_bytes);  // set up the footer
    // physical adjacency
    setHasPhysicalNext(block, 1);    // definitely does now
    setHasPhysicalPrev(newBlock, 1);
    setHasPhysicalNext(newBlock, hadPhysNext); // pass on original hasPhysNext-ness
                                               // to latter block
    reBucketBlock(newBlock);
    // don't rebucket (the original) block, since we're about to fill it
}
    

void* malloc(size_t s) {
    void* ret = 0;
    int i;
    s = next_aligned_value(s); // we only want to allocate aligned-size blocks, to keep
                               // all the headers and payloads aligned
    if(!s) { return 0; }
    //printf("mallocing %lu bytes\n", s);
    // this is where you start searching through the freelists
    int bucket = getBucket(s);
    for(i=bucket; i<NUMBUCKETS; ++i) {
        // traverse bucket, looking for blocks with size large enough
        // all the blocks here are free
        blockHeader* currBlock = buckets[i].logicalNext; // buckets[i] is an anchor
        while(currBlock && currBlock->size < s) {
            currBlock = currBlock->logicalNext;
        }
        // if we found a block
        if(currBlock) {
            // split, etc
            // unlink if necessary
            splitBlock(currBlock, s);
            setAllocated(currBlock, 1);
            logicalUnlinkBlock(currBlock);
            ret = getBlockPayload(currBlock);
        }
    }
    if(!ret) {
        // no free blocks suitable, must go for new memory
        blockHeader* newBlock = newMemoryChunk(s);
        // split the block
        // here the block will not be in a free list yet
        splitBlock(newBlock, s);
        setAllocated(newBlock, 1);
        // bucket the leftovers, if they're big enough
        ret = getBlockPayload(newBlock);
    }
    printf("malloc returning %p for size %lu\n", ret, s);
    return ret;
}

void free(void* p) {
    if(p)
        printf("freeing %p\n", p);
}

#ifdef TESTIT

#include <stdio.h>

int main() {
    // make sure the compiler isn't doing any weird padding
    // and everything will align right
    assert(sizeof(blockHeader) == (2*sizeof(void*) + 2*sizeof(size_t)));
    assert(sizeof(blockFooter) == (2*sizeof(size_t)));
    assert(sizeof(blockHeader) % ALIGNMENT == 0);
    assert(sizeof(blockFooter) % ALIGNMENT == 0);
    assert(sizeof(BLOCK_OVERHEAD) % ALIGNMENT == 0);
    assert(sizeof(memoryChunk) == (sizeof(size_t) + sizeof(memoryChunk*)));
    assert(sizeof(memoryChunk) % ALIGNMENT == 0);
    printf("10 -align-> %lu\n", next_aligned_value(10));
    printf("16 -align-> %lu\n", next_aligned_value(16));
    printf("17 -align-> %lu\n", next_aligned_value(17));
    printf("sizeof(size_t) %lu\n", sizeof(size_t));
    printf("sizeof(void*) %lu\n", sizeof(void*));
    printf("sizeof(uintptr_t) %lu\n", sizeof(uintptr_t));
    printf("log2(9) = %lu\n", mylog2(9));
    printf("s=%d --> b=%d\n", 7, getBucket(7));
    printf("s=%d --> b=%d\n", 8, getBucket(8));
    printf("s=%d --> b=%d\n", 9, getBucket(9));
    printf("s=%d --> b=%d\n", 15, getBucket(15));
    printf("s=%d --> b=%d\n", 16, getBucket(16));
    printf("s=%d --> b=%d\n", 17, getBucket(17));
    printf("s=%d --> b=%d\n", 31, getBucket(31));
    printf("s=%d --> b=%d\n", 32, getBucket(32));
    printf("s=%d --> b=%d\n", 33, getBucket(33));
    printf("s=%d --> b=%d\n", 63, getBucket(63));
    printf("s=%d --> b=%d\n", 64, getBucket(64));
    printf("s=%d --> b=%d\n", 65, getBucket(65));
}

#endif


