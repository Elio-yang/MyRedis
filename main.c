#include <stdio.h>
#include <windows.h>
char map[16][5]={
        "0000",
        "0001",
        "0010",
        "0011",
        "0100",
        "0101",
        "0110",
        "0111",
        "1000",
        "1001",
        "1010",
        "1011",
        "1100",
        "1101",
        "1110",
        "1111"
};

#define himask 0xf0
#define lomask 0x0f

void printbychar(void *key,unsigned size)
{
        byte *ptr=(byte*)key;
        for (int i = (int)size-1; i >=0; i--) {
                byte *tmp=ptr+i;
                byte hi=((*tmp)&(himask))>>4;
                byte low=(*tmp)&(lomask);
                printf("%s ",map[hi]);
                printf("%s ",map[low]);
        }
        printf("\n");
}
static unsigned long _dictNextPower(unsigned long size)
{
        unsigned long i = 4;
        while(1) {
                if (i >= size)
                        return i;
                i *= 2;
        }
}
static unsigned long _dictNextPower2(unsigned long size)
{
        --size;
        size|=size>>1;
        size|=size>>2;
        size|=size>>4;
        size|=size>>8;
        size|=size>>16;
        return ++size;
}
int main()
{

        unsigned int key=1;

        printf("%lu\n",_dictNextPower(key));
        printf("%lu",_dictNextPower2(key));
        return 0;
}
