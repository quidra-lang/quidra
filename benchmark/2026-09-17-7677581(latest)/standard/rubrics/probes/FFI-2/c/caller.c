/* FFI-2 C caller. Identical for all 10 languages. */
#include <stdio.h>
extern int ffi2_add(int a, int b);
int main(void) {
    printf("ffi2_add=%d\n", ffi2_add(20, 22));
    return 0;
}
