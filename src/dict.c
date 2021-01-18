/* dict.c Hash tables implementation
 * @author Elio Yang
 * @email  jluelioyang2001@gamil.com
 * @date 2021/1/17
 */

#include "dict.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>
#include <ctype.h>
#include "macros.h"
#include "zmalloc.h"
#include "redis_assert.h"



/*
 * 通过 dictEnableResize() 和 dictDisableResize() 两个函数，
 * 程序可以手动地允许或阻止哈希表进行 rehash ，
 * 这在 Redis 使用子进程进行保存操作时，可以有效地利用 copy-on-write 机制。
 *
 * 需要注意的是，并非所有 rehash 都会被 dictDisableResize 阻止：
 * 如果已使用节点的数量和字典大小之间的比率，
 * 大于字典强制 rehash 比率 dict_force_resize_ratio ，
 * 那么 rehash 仍然会（强制）进行。
 */

/*是否开启rehash*/
static int dict_can_resize = 1;
/*强制rehash的标志*/
static unsigned int dict_force_resize_ratio = 5;

/* -------------------------- private prototypes ----------------------------*/

static int _dictExpandIfNeeded(dict *ht);

static unsigned long _dictNextPower(unsigned long size);

static int _dictKeyIndex(dict *ht, const void *key);

static int _dictInit(dict *ht, dictType *type, void *privDataPtr);

/*----------------------------hash functions---------------------------------*/

/*
 * Thomas Wang's 32 bit mix function
 * see <https://gist.github.com/badboy/6267743>
 */
unsigned int dictIntHashFunction(unsigned int key)
{
        key += ~(key << 15);
        key ^= (key >> 10);
        key += (key << 3);
        key ^= (key >> 6);
        key += ~(key << 11);
        key ^= (key >> 16);
        return key;
}

/*Identity hash functions*/
unsigned int dictIndentityHashFunction(unsigned int key)
{
        return key;
}

/*seed : a global static variable*/
static uint32_t dict_hash_function_seed = 5381;

void dictSetHashFunctionSeed(uint32_t seed)
{
        dict_hash_function_seed = seed;
}

uint32_t dictGetHashFunctionSeed(void)
{
        return dict_hash_function_seed;
}

/* MurmurHash2, by Austin Appleby
 * Note - This code makes a few assumptions about how your machine behaves -
 * 1. We can read a 4-byte value from any address without crashing
 * 2. sizeof(int) == 4
 *
 * And it has a few limitations -
 *
 * 1. It will not work incrementally.
 * 2. It will not produce the same results on little-endian and big-endian
 *    machines.
 */
unsigned int dictGenHashFunction(const void *key, int len)
{
        /* 'm' and 'r' are mixing constants generated offline.
         They're not really 'magic', they just happen to work well.  */
        uint32_t seed = dict_hash_function_seed;
        const uint32_t m = 0x5bd1e995;
        const int r = 24;

        /* Initialize the hash to a 'random' value */
        uint32_t h = seed ^len;

        /* Mix 4 bytes at a time into the hash */
        const unsigned char *data = (const unsigned char *) key;

        while (len >= 4) {
                uint32_t k = *(uint32_t *) data;

                k *= m;
                k ^= k >> r;
                k *= m;

                h *= m;
                h ^= k;

                data += 4;
                len -= 4;
        }

        /* Handle the last few bytes of the input array  */
        switch (len) {
                case 3:
                        h ^= data[2] << 16;
                case 2:
                        h ^= data[1] << 8;
                case 1:
                        h ^= data[0];
                        h *= m;
        };

        /* Do a few final mixes of the hash to ensure the last few
         * bytes are well-incorporated. */
        h ^= h >> 13;
        h *= m;
        h ^= h >> 15;

        return (unsigned int) h;
}

/* And a case insensitive hash function (based on djb hash) */
unsigned int dictGenCaseHashFunction(const unsigned char *buf, int len)
{
        unsigned int hash = (unsigned int) dict_hash_function_seed;
        while (len--)
                hash = ((hash << 5) + hash) + (tolower(*buf++)); /* hash * 33 + c */
        return hash;
}

/*------------------------------API implementation---------------------------*/

/*重置*/
static void _dictReset(dictht *ht)
{
        ht->table = nullptr;
        ht->size = 0;
        ht->sizemask = 0;
        ht->used = 0;
}

/*创建新的字典*/
dict *dictCreate(dictType *type, void *privDataPtr)
{
        dict *d = ZMALLOC(1, dict);
        _dictInit(d, type, privDataPtr);
        return d;
}

/*初始化*/
int _dictInit(dict *d, dictType *type, void *privDataPtr)
{
        /*暂时不分配哈希表*/
        _dictReset(&d->ht[0]);
        _dictReset(&d->ht[1]);

        d->type = type;
        d->privdata = privDataPtr;
        d->rehashidx = -1;
        d->iterators = 0;

        return DICT_OK;
}

int dictResize(dict *d)
{
        int minimal;
        /*不能再关闭rehash得时候 和 正在rehash的时候调用*/
        if (!dict_can_resize || dictIsRehashing(d)) {
                return DICT_ERR;
        }
        minimal = d->ht[0].used;
        if (minimal < DICT_HT_INITIAL_SIZE) {
                minimal = DICT_HT_INITIAL_SIZE;
        }
        return dictExpand(d, minimal);
}

int dictExpand(dict *d, unsigned long size)
{
        /*新的hash表*/
        dictht n;
        unsigned long realsize = _dictNextPower(size);
        /*
         * 不能在字典正在 rehash 时进行
         * size 的值也不能小于 0 号哈希表的当前已使用节点
         */
        if (dictIsRehashing(d) || d->ht[0].used > size) {
                return DICT_ERR;
        }

        /* Allocate the new hash table and initialize all pointers to NULL */
        n.size = realsize;
        n.sizemask = realsize - 1;
        n.table = zcalloc(realsize * sizeof(dictEntry *));
        n.used = 0;

        /*
         * 如果 0 号哈希表为空，那么这是一次初始化：
         * 程序将新哈希表赋给 0 号哈希表的指针，然后字典就可以开始处理键值对了。
         */
        if (d->ht[0].table == NULL) {
                d->ht[0] = n;
                return DICT_OK;
        }
        /*准备rehash*/
        d->ht[1] = n;
        d->rehashidx = 0;
        return DICT_OK;

}


/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 *
 * 执行 N 步渐进式 rehash 。
 *
 * 返回 1 表示仍有键需要从 0 号哈希表移动到 1 号哈希表，
 * 返回 0 则表示所有键都已经迁移完毕。
 *
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table.
 *
 * 注意，每步 rehash 都是以一个哈希表索引（桶）作为单位的，
 * 一个桶里可能会有多个节点，
 * 被 rehash 的桶里的所有节点都会被移动到新哈希表。
 *
 * T = O(N)
 *
 * index=key%(size-1)
 */
int dictRehash(dict *d, int n)
{

        // 只可以在 rehash 进行中时执行
        if (!dictIsRehashing(d)) return 0;
        // 进行 N 步迁移
        // T = O(N)
        while (n--) {
                dictEntry *de, *nextde;
                /*如果 0 号哈希表为空，那么表示 rehash 执行完毕*/
                if (d->ht[0].used == 0) {
                        // 释放 0 号哈希表
                        zfree(d->ht[0].table);
                        // 将原来的 1 号哈希表设置为新的 0 号哈希表
                        d->ht[0] = d->ht[1];
                        // 重置旧的 1 号哈希表
                        _dictReset(&d->ht[1]);
                        // 关闭 rehash 标识
                        d->rehashidx = -1;
                        // 返回 0 ，向调用者表示 rehash 已经完成
                        return 0;
                }

                /* Note that rehashidx can't overflow as we are sure there are more
                 * elements because ht[0].used != 0 */
                /*确保 rehashidx 没有越界*/
                redis_assert(d->ht[0].size > (unsigned) d->rehashidx);

                /*略过数组中为空的索引，找到下一个非空索引*/
                while (d->ht[0].table[d->rehashidx] == NULL) d->rehashidx++;

                /*指向该索引的链表表头节点*/
                de = d->ht[0].table[d->rehashidx];
                /* Move all the keys in this bucket from the old to the new hash HT */
                while (de) {
                        unsigned int h;
                        // 保存下个节点的指针
                        nextde = de->next;
                        /* Get the index in the new hash table */
                        h = dictHashKey(d, de->key) & d->ht[1].sizemask;

                        // 插入节点到新哈希表
                        de->next = d->ht[1].table[h];
                        d->ht[1].table[h] = de;

                        d->ht[0].used--;
                        d->ht[1].used++;

                        // 继续处理下个节点
                        de = nextde;
                }
                // 将刚迁移完的哈希表索引的指针设为空
                d->ht[0].table[d->rehashidx] = NULL;
                // 更新 rehash 索引
                d->rehashidx++;
        }
}

/*获得以毫秒为单位的UNIX时间戳*/
long long timeInMilliseconds(void )
{
        struct timeval tv;
        gettimeofday(&tv,NULL);
        return (((long long )tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

/*在给定的毫秒时间内，以100步为单位rehash*/
int dictRehashMilliseconds(dict *d,int ms)
{
        long long start=timeInMilliseconds();
        int rehashes=0;
        while (dictRehash(d,100)){
                rehashes+=100;
                if (timeInMilliseconds()-start>ms){
                        break;
                }
        }
        return rehashes;
}

/*
 * 在字典不存在安全迭代器的情况下，对字典进行单步 rehash 。
 * 字典有安全迭代器的情况下不能进行 rehash ，
 * 因为两种不同的迭代和修改操作可能会弄乱字典。
 * 这个函数被多个通用的查找、更新操作调用，
 * 它可以让字典在被使用的同时进行 rehash 。
 */
static void _dictRehashStep(dict *d) {
        if (d->iterators == 0) dictRehash(d,1);
}




/*---------------------private functions-----------------------------------------*/
/*向上取到2的整数次方*/
static unsigned long _dictNextPower(unsigned long size)
{
        if (size < DICT_HT_INITIAL_SIZE) {
                return DICT_HT_INITIAL_SIZE;
        } else if (size > UINT32_MAX) {
                return UINT32_MAX;
        }
        --size;
        size |= size >> 1;
        size |= size >> 2;
        size |= size >> 4;
        size |= size >> 8;
        size |= size >> 16;
        return ++size;
}

/*根据需要，初始化或者扩展哈希表*/
static int _dictExpandIfNeeded(dict *d)
{
        /*正在渐进式rehash*/
        if (dictIsRehashing(d)){
                return DICT_OK;
        }
        /*初始化哈希表*/
        if (d->ht[0].size==0){
                return dictExpand(d,DICT_HT_INITIAL_SIZE);
        }
        /*
         * 1. 已经使用的/字典大小 比率接近1:1 且dict_can_resize为真
         * 2. 超过dict_force_resize_ratio
         */
        if ((d->ht[0].used>=d->ht[0].size && dict_can_resize)||
        (d->ht[0].used/d->ht[0].size>dict_force_resize_ratio)){
                return dictExpand(d,2*d->ht[0].used);
        }
        return DICT_OK;
}

/*
 * 返回可以将 key 插入到哈希表的索引位置
 * 如果 key 已经存在于哈希表，那么返回 -1
 * 注意，如果字典正在进行 rehash ，那么总是返回 1 号哈希表的索引。
 * 因为在字典进行 rehash 时，新节点总是插入到 1 号哈希表。
 */
static int _dictKeyIndex(dict *d, const void *key)
{
        unsigned int h, idx, table;
        dictEntry *he;
        /*出问题了*/
        if (_dictExpandIfNeeded(d) == DICT_ERR) {
                return -1;
        }
        h=dictHashKey(d,key);
        for (table = 0; table <=1 ; table++) {
                idx=h&d->ht[table].sizemask;
                he=d->ht[table].table[idx];
                while (he){
                        /*key已经存在了*/
                        if (dictCompareKeys(d,key,he->key)){
                                return -1;
                        }
                        he=he->next;
                }
                /*
                 * 第一次到此处说明0表中无key
                 * 看是否在rehash
                 */
                if (!dictIsRehashing(d)){
                        break;
                }
        }
        return idx;
}




