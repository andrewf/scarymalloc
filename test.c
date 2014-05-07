#include "stdlib.h"
#include "stdio.h"

#ifndef NUMPTRS
#define NUMPTRS 10
#endif

void use(char* buf, size_t s) {
    size_t i;
    for(i=0; i<s; ++i) {
        buf[i] = (char)(i%256);
    }
}

void check(char* buf, size_t s) {
    size_t i;
    for(i=0; i<s; ++i) {
        if( buf[i] != (char)(i%256) ) {
            printf("memory corruption at %p, base addr %p\n", buf + i, buf);
        }
    }
}

int main(int argc, char** args) {
    void* ptrs[NUMPTRS];
    size_t sizes[NUMPTRS];
    int i = 0;
    size_t n = 0;
    for(i=0; i<NUMPTRS; ++i) {
        ptrs[i] = 0;
        sizes[i] = 0;
    }
    // read integers off stdin
    //printf("allocating.\n");
    i = 0;
    while(!feof(stdin) && fscanf(stdin, " %lu ", &n)) {
        //printf("allocating %d\n", n);
        if(ptrs[i]) {
            check(ptrs[i], sizes[i]);
            free(ptrs[i]);
        }
        sizes[i] = n;
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
    //printf("done. cool!\n");
    return 0;
}
