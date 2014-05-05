#include <stdio.h>
#include <unistd.h>
#include <assert.h>

/////////////////////////
// CONSTANTS/TYPES
/////////////////////////

#define NUMBUCKETS 32 // can't go too far above bit size... be pessimistic
#define MIN_PHYSICAL_BLOCK 0x10000   // whatever

const int ALLOCATED_MASK = 1;  // & this with something to find if block is allocated

const int ALIGNMENT = 16;  // for 64-bit, apparently

typedef struct blockHeader_t {
    struct blockHeader_t* logicalPrev;  // previous in size-bucket free-list
    struct blockHeader_t* logicalNext;  // next in,.. you know
    size_t size; // payload size!
    size_t pad;    // for 16-byte alignment
} blockHeader; // end of data is the size footer, also size_t

#define BLOCK_OVERHEAD (sizeof(blockHeader) + sizeof(size_t))

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
memoryChunk* latestPhysicalBlock = 0;

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

blockHeader* newBlock(void* blockPosition) {
    blockHeader* b = (blockHeader*)blockPosition;
    b->logicalPrev = 0;
    b->logicalNext = 0;
    b->size = 0;
    // take size, also set end size?
    // bit flags?
    return b;
}

void* getBlockPayload(blockHeader* block) {
    return (void*)block + sizeof(blockHeader);
}

size_t* getFooter(blockHeader* block) {
    // only assumes header size is valid
    return (size_t*)((void*)block + block->size);
}

void setBlockSize(blockHeader* block, size_t s) {
    // make darn sure there's room for size_t + sizeof(size_t)
    // bytes after the block header
    block->size = s;
    size_t* footer = getFooter(block);
    *footer = s;
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
    void* newBreak = 0;    // this is a terrible name
    minSize += BLOCK_OVERHEAD + CHUNK_OVERHEAD;
    size_t allocationSize;
    // try the largest of MIN_PHYSICAL_BLOCK and minSize
    if(minSize < MIN_PHYSICAL_BLOCK) {
        allocationSize = MIN_PHYSICAL_BLOCK;
        newBreak = sbrk(MIN_PHYSICAL_BLOCK);
    } else {
        allocationSize = minSize;
        newBreak = sbrk(minSize);
    }
    if(!newBreak) {
        // crap, failed to alloc
        if(allocationSize > minSize) {
            // we might be able to salvage by only allocating min size
            newBreak = sbrk(minSize);
            if(!newBreak) {
                // that's it
                return 0;
            }
        } else {
            // no recourse
            return 0;
        }
    };
    // newBreak != 0, with length allocationSize
    // TODO don't assume sbrk is aligned
    // coallesce with previous block if possible
    if(latestPhysicalBlock &&
            (latestPhysicalBlock + CHUNK_OVERHEAD + latestPhysicalBlock->size) == newBreak) {
        // to extend the old block with new data, we'll need to save it
        // footer is at last eight bytes of latestPhysicalBlock
        size_t* oldFooter = getChunkPayload(latestPhysicalBlock) +
                            latestPhysicalBlock->size -
                            sizeof(size_t);
        blockHeader* oldBlock =
            (blockHeader*)((void*)oldFooter - *oldFooter - sizeof(blockHeader));
        latestPhysicalBlock->size += allocationSize; // new memory is pure payload
        // now extend the block
        oldBlock->size += allocationSize;
        *getFooter(oldBlock) = oldBlock->size;
        return oldBlock;
    } else {
        // we've got to do it all fresh
        memoryChunk* newChunk = (memoryChunk*)newBreak;
        newChunk->older = latestPhysicalBlock;
        newChunk->size = allocationSize - CHUNK_OVERHEAD;
        blockHeader* newBlock = (blockHeader*)getChunkPayload(newChunk);
        setBlockSize(newBlock, newChunk->size - BLOCK_OVERHEAD);
        return newBlock;
    }
    // otherwise, put it on free list
}

void logicalUnlinkBlock(blockHeader* block) {
    block->logicalPrev->logicalNext = block->logicalNext;
    block->logicalNext->logicalPrev = block->logicalPrev;
}

void logicalLinkBlock(blockHeader* newPrev, blockHeader* block) {
    block->logicalPrev = newPrev;
    block->logicalNext = newPrev->logicalNext; // maybe null, we don't care
    newPrev->logicalNext = block;
}

void reBucketBlock(blockHeader* block) {
    // most likely, block has been newly split (or coallesced)
    int destBucket = getBucket(block->size);
    // unlink it from freelist chain
    logicalUnlinkBlock(block);
    // go through destBucket and find correct location
    blockHeader* currBlock = &buckets[destBucket];
    while(currBlock->logicalNext && (currBlock->logicalNext->size < block->size) ) {
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
    assert(block->size >= s);
    // TODO: actually do this, and rebucket leftovers, assuming it's actually
    // possible
}
    

void* malloc(size_t s) {
    void* ret = 0;
    int i;
    // worst malloc ever
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
            splitBlock(currBlock, s);
            ret = getBlockPayload(currBlock);
        }
    }
    if(!ret) {
        // no free blocks suitable, must go for new memory
        blockHeader* newBlock = newMemoryChunk(s);
        // split the block
        splitBlock(newBlock, s);
        // bucket the leftovers, if they're big enough
        ret = getBlockPayload(newBlock);
    }
    return ret;
}

void free(void* p) {
    if(p)
        printf("free? ha! are you kidding?\n");
}

#ifdef TESTIT

#include <stdio.h>

int main() {
    assert(sizeof(blockHeader) == (2*sizeof(void*) + 2*sizeof(size_t)));
    assert(sizeof(blockHeader) % ALIGNMENT == 0);
    assert(sizeof(memoryChunk) == (sizeof(size_t) + sizeof(memoryChunk*)));
    assert(sizeof(memoryChunk) % ALIGNMENT == 0);
    // figure out alignment somehow?
    printf("sizeof(size_t) %lu\n", sizeof(size_t));
    printf("sizeof(void*) %lu\n", sizeof(void*));
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


