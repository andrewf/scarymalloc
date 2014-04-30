#include "stdlib.h"
#include "stdio.h"

void use(void* buf, size_t s) {
    size_t i;
    for(i=0; i<s; ++i) {
        ((char*)buf)[i] = (char)(i%256);
    }
}

int main(int argc, char** args) {
    void* ptrs[10];
    int i;
    printf("allocating.\n");
    for(i=0; i<10; ++i) {
        ptrs[i] = malloc(32);
        use(ptrs[i], 32);
    }
    printf("freeing.\n");
    for(i=0; i<10; ++i) {
        free(ptrs[i]);
    }
    printf("done. cool!\n");
}
