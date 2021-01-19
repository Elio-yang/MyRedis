#include "adlist.h"
typedef struct student{
        int id;
        int score;
}stu;
int main()
{
        list *l=listCreate();

        stu S1={12,34};
        listAddNodeHead(l,&S1);
        stu S2={13,55};
        listAddNodeHead(l,&S2);

        return 0;
}