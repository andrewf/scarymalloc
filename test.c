#include "stdlib.h"
#include "stdio.h"

int main(int argc, char** args) {
    void* ptrs[10];
    int i;
    printf("allocating.\n");
    for(i=0; i<10; ++i) {
        ptrs[i] = malloc(32);
    }
    /*printf("freeing.\n");
    for(i=0; i<10; ++i) {
        free(ptrs[i]);
    }*/
    printf("done. cool!\n");
}
