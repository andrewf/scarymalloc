#include "stdio.h"
#include "unistd.h"

#define NUMBUCKETS 32ul // can't go too far above bit size... be pessimistic

const int ALLOCATED_MASK = 1;  // & this with something to find if block is allocated

const int ALIGNMENT = 8;

typedef struct blockHeader_t {
    void* logicalPrev;  // previous in size-bucket free-list
    void* logicalNext;  // next in,.. you know
    unsigned size;
    char data[1];
} blockHeader; // end of data is the size footer

typedef struct memoryChunk_t {
    blockHeader* firstBlock;
    size_t size;
    struct memoryChunk_t* older; // put highest-address/latest-allocated chunks at end, so they're
                        // easy to muck with if next sbrk is contiguous
} memoryChunk;

const int FIRSTBUCKETCEILING = (8); //1*ALIGNMENT;

blockHeader* physicalBlocks = 0;

/*
   Buckets are allocated like so: first bucket/freelist is
   for blocks of payload size 0 to FIRSTBUCKETCEILING. next is
   from (FBC, 2*FBC], (2*FBC, 4*FBC], etc., except the last one is
   just whatever won't fit in previous buckets.
*/
blockHeader* buckets[NUMBUCKETS] = {0};

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


void* malloc(size_t s) {
    s = s;
    // worst malloc ever
    printf("mallocing %lu bytes\n", s);
    void* ret = sbrk(s);
    if(ret == (void*)-1) {
        return 0;
    }
    return ret;
}

void free(void* p) {
    printf("free? ha! are you kidding?\n");
    p = p;
}

#ifdef TESTIT

#include <stdio.h>

int main() {
    printf("sizeof(size_t) %lu\n", sizeof(size_t));
    printf("log2(9) = %lu\n", mylog2(9));
    for(int i=67; i<130; ++i) {
        printf("s=%d --> b=%d\n", i, getBucket(i));
    }
}

#endif


