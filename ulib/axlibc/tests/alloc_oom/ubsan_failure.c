/* Negative control: the shared UBSan configuration must reject this program. */
#include <limits.h>

int main(void)
{
    volatile int value = INT_MAX;
    value += 1;
    return 0;
}
