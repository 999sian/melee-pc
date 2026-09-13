#define __assert libc_assert
#include <assert.h>
#undef __assert
#include <stdio.h>
#include "../src/melee/gr/grhomerun.c"
int main(void)
{
    /* Home-Run's map object 10 must update collision joint 0 each frame. */
    GrJoint* joints = (GrJoint*) grHr_804D4998;
    assert(joints[0].x == 0);
    assert(joints[0].y == 10);
    assert(joints[0].z == 0);
    puts("PASS: Home-Run collision joint 0 belongs to map object 10");
}
