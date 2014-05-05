#include "stdlib.h"
#include "stdio.h"

#ifndef NUMPTRS
#define NUMPTRS 10
#endif

void use(void* buf, size_t s) {
    size_t i;
    for(i=0; i<s; ++i) {
        ((char*)buf)[i] = (char)(i%256);
    }
}

int main(int argc, char** args) {
    void* ptrs[NUMPTRS];
    int wrapped = 0;
    int i = 0;
    size_t n = 0;
    for(i=0; i<NUMPTRS; ++i) {
        ptrs[i] = 0;
    }
    // read integers off stdin
    //printf("allocating.\n");
    i = 0;
    while(!feof(stdin) && fscanf(stdin, " %lu ", &n)) {
        //printf("allocating %d\n", n);
        free(ptrs[i]); // no op on 0, so unconditional is fine
        ptrs[i] = malloc(n);
        use(ptrs[i], n);  // twiddle the bits
        // increment i
        //printf("incrementing\n");
        i = (i + 1) % NUMPTRS;
    }
    //printf("freeing.\n");
    for(i=0; i<NUMPTRS; ++i) {
        free(ptrs[i]);
    }
    printf("done. cool!\n");
}
