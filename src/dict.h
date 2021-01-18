/* dict.c Hash tables implementation
 * @author Elio Yang
 * @email  jluelioyang2001@gamil.com
 * @date 2021/1/17
 * @description: in-memory hash tables with insert/del/replace/find/get-random
 *               will auto resize and collosions are handled by chaining
 */

#include <stdint.h>
#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0
#define DICT_ERR 1




/*
                        Dict结构


                                     dictEntry
                                     +-------+	    +-------+
                 dict                |   K   |      |   K   |
        +----------------------+     |   V   |      |   V   |
        | dictType *type       |     |  next-|----> |  next-|---->
        +----------------------+     +-------+	    +-------+
        | void *privdata       |         |
        +----------------------+         |
        | dictEntry**table ----|------->[|][][][][...][]
        | unsigned long size   |
ht[0]     | unsigned sizemask  |
        | unsigned long used   |
        +----------------------+
        | dictEntry**table     |
        | unsigned long size   |
ht[1]     | unsigned sizemask  |
        | unsigned long used   |
        +----------------------+
        | int rehashindex      |
        +----------------------+
        | int iterators        |
        +----------------------+

*/



/*
 * 哈希表节点
 */
typedef struct dictEntry {
        /*键*/
        void *key;
        /*值*/
        union {
                void *val;
                uint64_t u64;
                int64_t s64;
        }v;
        /*指向下一个节点*/
        struct dictEntry *next;
}dictEntry;

typedef struct dictType{
        /*计算hash值*/
        unsigned int (*hashFunction)(const void *key);

        /*复制键*/
        void *(*keyDup)(void *privdata,const void *key);

        /*复制值*/
        void *(*valDup)(void *privdata,const void *obj);

        /*对比键的函数*/
        int (*keyCompare)(void *privdata,void *key1, const void *key2);

        /*销毁键*/
        void (*keyDestructor)(void *privdata,void *key);

        /*销毁值*/
        void (*valDestructor)(void *privdata,void *obj);
}dictType;

/*
 * 哈希表
 */
typedef struct dictht{
        /*哈希表数组*/
        dictEntry **table;

        unsigned long size;

        /*掩码:size-1*/
        unsigned long sizemask;

        /*已有的节点数量*/
        unsigned long used;
}dictht;

typedef struct dict{
        /*操作特定类型键值对的函数*/
        dictType *type;
        /*特定类型函数的可选参数*/
        void *privdata;
        /*ht[1]在rehash时用*/
        dictht ht[2];
        /*rehash不再进行时值为-1*/
        int rehashidx;
        /*正在运行的迭代器数量*/
        int iterators;
}dict;

/*
 * 字典迭代器
 * 如果 safe 属性的值为 1 ，那么在迭代进行的过程中，
 * 程序仍然可以执行 dictAdd 、 dictFind 和其他函数，对字典进行修改。
 * 如果 safe 不为 1 ，那么程序只会调用 dictNext 对字典进行迭代，
 * 而不对字典进行修改。
 */
typedef struct dictIterator {

        /*被迭代的字典*/
        dict *d;

        /*
         * table ：正在被迭代的哈希表号码，值可以是 0 或 1 。
         * index ：迭代器当前所指向的哈希表索引位置。
         * safe ：标识这个迭代器是否安全
         */
        int table, index, safe;

        /* entry ：当前迭代到的节点的指针
         * nextEntry ：当前迭代节点的下一个节点
         *             因为在安全迭代器运作时， entry 所指向的节点可能会被修改，
         *             所以需要一个额外的指针来保存下一节点的位置，
         *             从而防止指针丢失
         */
        dictEntry *entry, *nextEntry;

        long long fingerprint; /* unsafe iterator fingerprint for misuse detection */
} dictIterator;

typedef void (dictScanFunction)(void *privdata,const dictEntry *de);
#define DICT_HT_INITIAL_SIZE 4

/*-------------------------------Macros--------------------------------*/
// 释放给定字典节点的值
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d)->privdata, (entry)->v.val)

// 设置给定字典节点的值
#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        entry->v.val = (d)->type->valDup((d)->privdata, _val_); \
    else \
        entry->v.val = (_val_); \
} while(0)

// 将一个有符号整数设为节点的值
#define dictSetSignedIntegerVal(entry, _val_) \
    do { entry->v.s64 = _val_; } while(0)

// 将一个无符号整数设为节点的值
#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { entry->v.u64 = _val_; } while(0)

// 释放给定字典节点的键
#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d)->privdata, (entry)->key)

// 设置给定字典节点的键
#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        entry->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        entry->key = (_key_); \
} while(0)

// 比对两个键
#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

// 计算给定键的哈希值
#define dictHashKey(d, key) (d)->type->hashFunction(key)
// 返回获取给定节点的键
#define dictGetKey(he) ((he)->key)
// 返回获取给定节点的值
#define dictGetVal(he) ((he)->v.val)
// 返回获取给定节点的有符号整数值
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
// 返回给定节点的无符号整数值
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
// 返回给定字典的大小
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size)
// 返回字典的已有节点数量
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)
// 查看字典是否正在 rehash
#define dictIsRehashing(d) ((d)->rehashidx != -1)


/*---------------------------------API---------------------------------*/
dict *dictCreate(dictType *type, void *privDataPtr);
int dictExpand(dict *d, unsigned long size);
int dictAdd(dict *d, void *key, void *val);
dictEntry *dictAddRaw(dict *d, void *key);
int dictReplace(dict *d, void *key, void *val);
dictEntry *dictReplaceRaw(dict *d, void *key);
int dictDelete(dict *d, const void *key);
int dictDeleteNoFree(dict *d, const void *key);
void dictRelease(dict *d);
dictEntry * dictFind(dict *d, const void *key);
void *dictFetchValue(dict *d, const void *key);
int dictResize(dict *d);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
dictEntry *dictNext(dictIterator *iter);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
int dictGetRandomKeys(dict *d, dictEntry **des, int count);
void dictPrintStats(dict *d);
unsigned int dictGenHashFunction(const void *key, int len);
unsigned int dictGenCaseHashFunction(const unsigned char *buf, int len);
void dictEmpty(dict *d, void(callback)(void*));
void dictEnableResize(void);
void dictDisableResize(void);
int dictRehash(dict *d, int n);
int dictRehashMilliseconds(dict *d, int ms);
void dictSetHashFunctionSeed(unsigned int initval);
unsigned int dictGetHashFunctionSeed(void);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, void *privdata);

/* Hash table types */
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;


#endif //__DICT_H
