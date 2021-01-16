/*
 * adlist.h  A generic doubly list implementation
 * @author Elio Yang
 * @email  jluelioyang2001@gamil.com
 * @date 2021/1/15
 */

#ifndef MY_REDIS_ADLIST_H
#define MY_REDIS_ADLIST_H

/*list node*/
typedef struct listNode{
        struct listNode *prev;
        struct listNode *next;
        /*generic value*/
        void *value;
}listNode;

/*list iterator*/
typedef struct listIter{
        listNode *next;
        int direction;
}listIter;

/*
 * structure of list
 * OOP implemented
 */
typedef struct list{
        /*head and tail node*/
        listNode *head;
        listNode *tail;
        /*copy a node*/
        void * (*dup)(void *ptr);
        /*free a node*/
        void (*free)(void *ptr);
        /*matching function*/
        int (*match)(void *ptr,void *key);
        /*node number*/
        unsigned long len;
}list;


/*
 * Macros as functions
 */

#define listLength(l) ((l)->len)
#define listFirst(l) ((l)->head)
#define listLast(l) ((l)->last)

#define listPrevNode(n) ((n)->prev)
#define listNextNode(n) ((n)->next)
#define listNodeValue(n)((n)->value)

/*set the dup function pointer*/
#define listSetDupMethod(l,m) ((l)->dup=(m))
/*set the free function pointer*/
#define listSetFreeMethod(l,m) ((l)->free=(m))
/*set the matching function pointer*/
#define listSetMatchMethod(l,m) ((l)->match=(m))

/*return function pointer method of list*/
#define listGetDupMethod(l) ((l)->dup)
#define listGetFreeMethod(l) ((l)->free)
#define listGetMatchMethod(l) ((l)->match)

/*
 * API
 */

list *listCreate(void);
void listRelease(list *list);
list *listAddNodeHead(list *list, void *value);
list *listAddNodLeTail(list *list, void *value);
list *listInsertNode(list *list, listNode *old_node, void *value, int after);
void listDelNode(list *list, listNode *node);
listIter *listGetIterator(list *list, int direction);
listNode *listNext(listIter *iter);
void listReleaseIterator(listIter *iter);
list *listDup(list *orig);
listNode *listSearchKey(list *list, void *key);
listNode *listIndex(list *list, long index);
void listRewind(list *list, listIter *li);
void listRewindTail(list *list, listIter *li);
void listRotate(list *list);

/*
 * directions for iterators
 */

/*from head to tail*/
#define AL_START_HEAD 0
/*from tail to head*/
#define AL_START_TAIL 1

#endif //MY_REDIS_ADLIST_H
