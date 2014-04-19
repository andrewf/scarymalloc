#include "stdio.h"
#include "unistd.h"

void* malloc(size_t s) {
    // worst malloc ever
    printf("mallocing %d bytes\n", s);
    void* ret = sbrk(s);
    if(ret == (void*)-1) {
        return 0;
    }
    return ret;
}

void free(void* p) {
    printf("free? ha! are you kidding?\n");
}

