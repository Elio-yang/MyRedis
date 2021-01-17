/*
 * @author Elio Yang
 * @email  jluelioyang2001@gamil.com
 * @date 2021/1/15
 */

#include "adlist.h"
#include "zmalloc.h"
#include <stdlib.h>

#define nullptr NULL
#define ZMALLOC(n, type) (zmalloc((sizeof(type))*(n)))

/*create a list*/
list *listCreate(void)
{
        list *list;
        list = ZMALLOC(1, list);
        if (list == nullptr) { return nullptr; }

        /*初始化属性*/
        list->head = list->tail = nullptr;
        list->len = 0;
        list->free = nullptr;
        list->dup = nullptr;
        list->match = nullptr;

        return list;
}

/*release a list*/
void listRelease(list *list)
{
        unsigned long len;
        listNode *current, *next;

        current = list->head;
        len = list->len;
        while (len--) {
                next = current->next;
                /*如果value需要有free函数*/
                if (list->free) {
                        list->free(current->value);
                }
                zfree(current);
                current = next;
        }
        zfree(list);
}

/*add a node with value to the head*/
list *listAddNodeHead(list *list, void *value)
{
        listNode *node;
        node = ZMALLOC(1, listNode);
        if (node == nullptr) { return nullptr; }
        node->value = value;

        /*空链表*/
        if (list->len == 0) {
                list->head = list->tail = node;
        } else {
                node->next = list->head;
                list->head->prev = node;
                node->prev = nullptr;
                /*node作为新的头节点*/
                list->head = node;
        }
        list->len++;
        return list;
}

/*add a node with value to the tail*/
list *listAddNodeTail(list *list, void *value)
{
        listNode *node;
        node = ZMALLOC(1, listNode);
        if (node == nullptr) { return nullptr; }

        if (list->len == 0) {
                list->head = list->tail = nullptr;
        } else {
                node->next = nullptr;
                node->prev = list->tail;
                list->tail->next = node;
                list->tail = node;
        }
        list->len++;
        return list;
}

/*
 * insert node after/before old_node
 * direction 0 :before old_node
 * direction 1 :after old_node
 */
list *listInsertNode(list *list, listNode *old_node, void *value, int direction)
{
        listNode *node;
        node = ZMALLOC(1, listNode);
        if (node == nullptr) { return nullptr; }
        node->value = value;

        /*插入到之后*/
        if (direction) {
                node->next = old_node->next;
                node->prev = old_node;
                if (old_node == list->tail) {
                        list->tail = node;
                }
        } else {
                node->next = old_node;
                node->prev = old_node->prev;
                if (old_node == list->head) {
                        list->head = node;
                }
        }
        if (node->prev != nullptr) {
                node->prev->next = node;
        }
        if (node->next != nullptr) {
                node->next->prev = node;
        }
        list->len++;
        return list;
}

/*delete given node*/
void listDelNode(list *list, listNode *node)
{
        if (node->prev) {
                node->prev->next = node->next;
        } else {
                list->head = node->next;
        }
        if (node->next) {
                node->next->prev = node->prev;
        } else {
                list->tail = node->prev;
        }
        if (list->free) {
                list->free(node->value);
        }
        zfree(node);
        list->len--;
}

/*
 * create a iterator
 * direction决定迭代的方向
 * AL_START_HEAD ：从头到尾
 * AL_START_TAIL ：从尾到头
 */
listIter *listGetIterator(list *list, int direction)
{
        listIter *iter;
        iter = ZMALLOC(1, listIter);
        if (iter == nullptr) { return nullptr; }

        if (direction == AL_START_HEAD) {
                iter->next = list->head;
        } else {
                iter->next = list->tail;
        }
        iter->direction = direction;
        return iter;
}

void listReleaseIterator(listIter *iter)
{
        zfree(iter);
}

void listRewind(list *list, listIter *iter)
{
        iter->next = list->head;
        iter->direction = AL_START_HEAD;
}

void listRewindTail(list *list, listIter *iter)
{
        iter->next = list->tail;
        iter->direction = AL_START_TAIL;
}

/* return the next element of an iterator
 * remove the currently returned element using listDelNode() is OK
 * but not to remove other elements
 */
listNode *listNext(listIter *iter)
{
        listNode *current = iter->next;
        if (current != nullptr) {
                if (iter->direction == AL_START_HEAD) {
                        iter->next = current->next;
                } else {
                        iter->next = current->prev;
                }
        }
        return current;
}

/*copy a whole list*/
list *listDup(list *orig)
{
        list *copy;
        listIter *iter;
        listNode *node;

        copy = listCreate();
        if (copy == nullptr) { return nullptr; }

        copy->free = orig->free;
        copy->match = orig->match;
        copy->dup = orig->dup;

        iter = listGetIterator(orig, AL_START_HEAD);
        /*迭代整个链表*/
        while ((node = listNext(iter)) != nullptr) {
                void *value;
                if (copy->dup) {
                        value = copy->dup(node->value);
                        /*内存错误*/
                        if (value == nullptr) {
                                listRelease(copy);
                                listReleaseIterator(iter);
                                return nullptr;
                        }
                } else {
                        value = node->value;
                }
                if (listAddNodeTail(copy, value) == nullptr) {
                        listRelease(copy);
                        listReleaseIterator(iter);
                }
                return nullptr;
        }
        listReleaseIterator(iter);
        return copy;
}

/*search list with key*/
listNode *searchKey(list *list, void *key)
{
        listIter *iter;
        listNode *node;
        iter = listGetIterator(list, AL_START_HEAD);
        while ((node = listNext(iter)) != nullptr) {
                if (list->match) {
                        if (list->match(node->value, key)) {
                                listReleaseIterator(iter);
                                return node;
                        }
                } else {
                        if (key == node->value) {
                                listReleaseIterator(iter);
                                return node;
                        }
                }
        }
        listReleaseIterator(iter);
        return nullptr;
}

/*return node at index position*/
listNode *listIndex(list *list, long index)
{
        listNode *node;
        /*从尾部找*/
        if (index < 0) {
                index = (-index) - 1;
                node = list->tail;
                while (index-- && node) {
                        node = node->prev;
                }
        } else {
                node = list->head;
                while (index-- && node) {
                        node = node->next;
                }
        }
        return node;
}

/*make the tail head*/
void listRotate(list *list)
{
        listNode *tail = list->tail;
        if (listLength(list) <= 1) { return; }

        list->tail = tail->prev;
        list->tail->next = nullptr;

        list->head->prev = tail;
        tail->prev = nullptr;
        tail->next = list->head;
        list->head = tail;
}
