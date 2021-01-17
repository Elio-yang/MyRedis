#include <stdio.h>
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(long long))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif


int main()
{
        printf("%u\n", PREFIX_SIZE);
        printf("%u\n", sizeof(long long ));
        return 0;
}
