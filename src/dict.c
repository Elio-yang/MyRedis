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
long long timeInMilliseconds(void)
{
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (((long long) tv.tv_sec) * 1000) + (tv.tv_usec / 1000);
}

/*在给定的毫秒时间内，以100步为单位rehash*/
int dictRehashMilliseconds(dict *d, int ms)
{
        long long start = timeInMilliseconds();
        int rehashes = 0;
        while (dictRehash(d, 100)) {
                rehashes += 100;
                if (timeInMilliseconds() - start > ms) {
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
static void _dictRehashStep(dict *d)
{
        if (d->iterators == 0) dictRehash(d, 1);
}

/*
 * 尝试将键插入到字典中
 * 如果键已经在字典存在，那么返回 NULL
 * 如果键不存在，那么程序创建新的哈希节点，
 * 将节点和键关联，并插入到字典，然后返回节点本身。
 */

dictEntry *dictAddRaw(dict *d, void *key)
{
        int index;
        dictEntry *entry;
        dictht *ht;

        if (dictIsRehashing(d)) {
                _dictRehashStep(d);
        }
        if ((index = _dictKeyIndex(d, key)) == -1) {
                return nullptr;
        }
        /*如果在rehash就插入1号哈希表*/
        ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
        entry = ZMALLOC(1, dictEntry);
        /*头插法*/
        entry->next = ht->table[index];
        ht->table[index]->next = entry;
        ht->used++;
        dictSetKey(d, entry, key);
        return entry;
}

/*
 * 尝试将给定键值对添加到字典中
 * 只有给定键 key 不存在于字典时，添加操作才会成功
 * 添加成功返回 DICT_OK ，失败返回 DICT_ERR
 */
int dictAdd(dict *d, void *key, void *val)
{
        dictEntry *entry = dictAddRaw(d, key);
        if (!entry) {
                return DICT_ERR;
        }
        dictSetVal(d, entry, val);
        return DICT_OK;
}

/*
 * 将给定的键值对添加到字典中，如果键已经存在，那么删除旧有的键值对。
 * 如果键值对为全新添加，那么返回 1 。
 * 如果键值对是通过对原有的键值对更新得来的，那么返回 0 。
 */
int dictReplace(dict *d, void *key, void *val)
{
        dictEntry *entry, auxentry;
        if (dictAddRaw(d, key) == DICT_OK) {
                return 1;
        }

        entry = dictFind(d, key);
        auxentry = *entry;
        dictSetVal(d, entry, val);
        dictFreeVal(d, &auxentry);
        return 0;
}

/*
 * dictAddRaw() 根据给定 key 释放存在，执行以下动作：
 * 1) key 已经存在，返回包含该 key 的字典节点
 * 2) key 不存在，那么将 key 添加到字典
 * 不论发生以上的哪一种情况，
 * dictAddRaw() 都总是返回包含给定 key 的字典节点。
 */
dictEntry *dictPeplaceRaw(dict *d, void *key)
{
        dictEntry *entry = dictFind(d, key);
        return entry ? entry : dictAddRaw(d, key);
}


/* Search and remove an element */
/*
 * 查找并删除包含给定键的节点
 *
 * 参数 nofree 决定是否调用键和值的释放函数
 * 0 表示调用，1 表示不调用
 *
 * 找到并成功删除返回 DICT_OK ，没找到则返回 DICT_ERR
 *
 * T = O(1)
 */
static int dictGenericDelete(dict *d, const void *key, int nofree)
{
        unsigned int h, idx;
        dictEntry *he, *prevHe;
        int table;

        // 字典（的哈希表）为空
        if (d->ht[0].size == 0) return DICT_ERR; /* d->ht[0].table is NULL */

        // 进行单步 rehash ，T = O(1)
        if (dictIsRehashing(d)) _dictRehashStep(d);

        // 计算哈希值
        h = dictHashKey(d, key);

        // 遍历哈希表
        // T = O(1)
        for (table = 0; table <= 1; table++) {

                // 计算索引值
                idx = h & d->ht[table].sizemask;
                // 指向该索引上的链表
                he = d->ht[table].table[idx];
                prevHe = NULL;
                // 遍历链表上的所有节点
                // T = O(1)
                while (he) {

                        if (dictCompareKeys(d, key, he->key)) {
                                // 超找目标节点

                                /* Unlink the element from the list */
                                // 从链表中删除
                                if (prevHe)
                                        prevHe->next = he->next;
                                else
                                        d->ht[table].table[idx] = he->next;

                                // 释放调用键和值的释放函数？
                                if (!nofree) {
                                        dictFreeKey(d, he);
                                        dictFreeVal(d, he);
                                }

                                // 释放节点本身
                                zfree(he);

                                // 更新已使用节点数量
                                d->ht[table].used--;

                                // 返回已找到信号
                                return DICT_OK;
                        }

                        prevHe = he;
                        he = he->next;
                }

                // 如果执行到这里，说明在 0 号哈希表中找不到给定键
                // 那么根据字典是否正在进行 rehash ，决定要不要查找 1 号哈希表
                if (!dictIsRehashing(d)) break;
        }

        // 没找到
        return DICT_ERR; /* not found */
}

/*
 * 从字典中删除包含给定键的节点
 *
 * 并且调用键值的释放函数来删除键值
 *
 * 找到并成功删除返回 DICT_OK ，没找到则返回 DICT_ERR
 * T = O(1)
 */
int dictDelete(dict *ht, const void *key)
{
        return dictGenericDelete(ht, key, 0);
}

/*
 * 从字典中删除包含给定键的节点
 *
 * 但不调用键值的释放函数来删除键值
 *
 * 找到并成功删除返回 DICT_OK ，没找到则返回 DICT_ERR
 * T = O(1)
 */
int dictDeleteNoFree(dict *ht, const void *key)
{
        return dictGenericDelete(ht, key, 1);
}

/* Destroy an entire dictionary */
/*
 * 删除哈希表上的所有节点，并重置哈希表的各项属性
 *
 * T = O(N)
 */






/*删除所有的节点，重置各个属性*/
int _dictClear(dict *d, dictht *ht, void (callback)(void *))
{
        unsigned long i;
        for (i = 0; i < ht->size && ht->used > 0; i++) {
                dictEntry *he, *nextHe;
                /*i是k·2^16时 且传入了回调函数时*/
                if (callback && (i & 65535) == 0) {
                        callback(d->privdata);
                }
                /*跳过空的索引*/
                if ((he = ht->table[i]) == nullptr) { continue; }
                while (he) {
                        nextHe = he->next;
                        dictFreeKey(d, he);
                        dictFreeVal(d, he);
                        zfree(he);
                        ht->used--;
                        he = nextHe;
                }
        }
        zfree(ht->table);
        _dictReset(ht);
        return DICT_OK;
}

void dictRelease(dict *d)
{
        // 删除并清空两个哈希表
        _dictClear(d,&d->ht[0],NULL);
        _dictClear(d,&d->ht[1],NULL);
        // 释放节点结构
        zfree(d);
}

/*
 * 返回字典中包含键 key 的节点
 *
 * 找到返回节点，找不到返回 NULL
 *
 * T = O(1)
 */
dictEntry *dictFind(dict *d, const void *key)
{
        dictEntry *he;
        unsigned int h, idx, table;

        // 字典（的哈希表）为空
        if (d->ht[0].size == 0) return NULL; /* We don't have a table at all */

        // 如果条件允许的话，进行单步 rehash
        if (dictIsRehashing(d)) _dictRehashStep(d);

        // 计算键的哈希值
        h = dictHashKey(d, key);
        // 在字典的哈希表中查找这个键
        // T = O(1)
        for (table = 0; table <= 1; table++) {

                // 计算索引值
                idx = h & d->ht[table].sizemask;

                // 遍历给定索引上的链表的所有节点，查找 key
                he = d->ht[table].table[idx];
                // T = O(1)
                while(he) {

                        if (dictCompareKeys(d, key, he->key))
                                return he;

                        he = he->next;
                }

                // 如果程序遍历完 0 号哈希表，仍然没找到指定的键的节点
                // 那么程序会检查字典是否在进行 rehash ，
                // 然后才决定是直接返回 NULL ，还是继续查找 1 号哈希表
                if (!dictIsRehashing(d)) return NULL;
        }

        // 进行到这里时，说明两个哈希表都没找到
        return NULL;
}

/*
 * 获取包含给定键的节点的值
 *
 * 如果节点不为空，返回节点的值
 * 否则返回 NULL
 *
 * T = O(1)
 */
void *dictFetchValue(dict *d, const void *key) {
        dictEntry *he;

        // T = O(1)
        he = dictFind(d,key);

        return he ? dictGetVal(he) : NULL;
}

/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */
long long dictFingerprint(dict *d) {
        long long integers[6], hash = 0;
        int j;

        integers[0] = (long) d->ht[0].table;
        integers[1] = d->ht[0].size;
        integers[2] = d->ht[0].used;
        integers[3] = (long) d->ht[1].table;
        integers[4] = d->ht[1].size;
        integers[5] = d->ht[1].used;

        /* We hash N integers by summing every successive integer with the integer
         * hashing of the previous sum. Basically:
         *
         * Result = hash(hash(hash(int1)+int2)+int3) ...
         *
         * This way the same set of integers in a different order will (likely) hash
         * to a different number. */
        for (j = 0; j < 6; j++) {
                hash += integers[j];
                /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
                hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
                hash = hash ^ (hash >> 24);
                hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
                hash = hash ^ (hash >> 14);
                hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
                hash = hash ^ (hash >> 28);
                hash = hash + (hash << 31);
        }
        return hash;
}

/*
 * 创建并返回给定字典的不安全迭代器
 *
 * T = O(1)
 */
dictIterator *dictGetIterator(dict *d)
{
        dictIterator *iter = zmalloc(sizeof(*iter));

        iter->d = d;
        iter->table = 0;
        iter->index = -1;
        iter->safe = 0;
        iter->entry = NULL;
        iter->nextEntry = NULL;

        return iter;
}


/*
 * 创建并返回给定节点的安全迭代器
 *
 * T = O(1)
 */
dictIterator *dictGetSafeIterator(dict *d) {
        dictIterator *i = dictGetIterator(d);

        // 设置安全迭代器标识
        i->safe = 1;

        return i;
}

/*
 * 返回迭代器指向的当前节点
 *
 * 字典迭代完毕时，返回 NULL
 *
 * T = O(1)
 */
dictEntry *dictNext(dictIterator *iter)
{
        while (1) {

                // 进入这个循环有两种可能：
                // 1) 这是迭代器第一次运行
                // 2) 当前索引链表中的节点已经迭代完（NULL 为链表的表尾）
                if (iter->entry == NULL) {

                        // 指向被迭代的哈希表
                        dictht *ht = &iter->d->ht[iter->table];

                        // 初次迭代时执行
                        if (iter->index == -1 && iter->table == 0) {
                                // 如果是安全迭代器，那么更新安全迭代器计数器
                                if (iter->safe)
                                        iter->d->iterators++;
                                        // 如果是不安全迭代器，那么计算指纹
                                else
                                        iter->fingerprint = dictFingerprint(iter->d);
                        }
                        // 更新索引
                        iter->index++;

                        // 如果迭代器的当前索引大于当前被迭代的哈希表的大小
                        // 那么说明这个哈希表已经迭代完毕
                        if (iter->index >= (signed) ht->size) {
                                // 如果正在 rehash 的话，那么说明 1 号哈希表也正在使用中
                                // 那么继续对 1 号哈希表进行迭代
                                if (dictIsRehashing(iter->d) && iter->table == 0) {
                                        iter->table++;
                                        iter->index = 0;
                                        ht = &iter->d->ht[1];
                                        // 如果没有 rehash ，那么说明迭代已经完成
                                } else {
                                        break;
                                }
                        }

                        // 如果进行到这里，说明这个哈希表并未迭代完
                        // 更新节点指针，指向下个索引链表的表头节点
                        iter->entry = ht->table[iter->index];
                } else {
                        // 执行到这里，说明程序正在迭代某个链表
                        // 将节点指针指向链表的下个节点
                        iter->entry = iter->nextEntry;
                }

                // 如果当前节点不为空，那么也记录下该节点的下个节点
                // 因为安全迭代器有可能会将迭代器返回的当前节点删除
                if (iter->entry) {
                        /* We need to save the 'next' here, the iterator user
                         * may delete the entry we are returning. */
                        iter->nextEntry = iter->entry->next;
                        return iter->entry;
                }
        }

        // 迭代完毕
        return NULL;
}

/*
 * 释放给定字典迭代器
 *
 * T = O(1)
 */
void dictReleaseIterator(dictIterator *iter)
{

        if (!(iter->index == -1 && iter->table == 0)) {
                // 释放安全迭代器时，安全迭代器计数器减一
                if (iter->safe)
                        iter->d->iterators--;
                        // 释放不安全迭代器时，验证指纹是否有变化
                else
                        redis_assert(iter->fingerprint == dictFingerprint(iter->d));
        }
        zfree(iter);
}

/*
 * 随机返回字典中任意一个节点。
 * 可用于实现随机化算法。
 * 如果字典为空，返回 NULL 。
 */
dictEntry *dictGetRandomKey(dict *d)
{
        dictEntry *he, *orighe;
        unsigned int h;
        int listlen, listele;

        // 字典为空
        if (dictSize(d) == 0) return NULL;

        // 进行单步 rehash
        if (dictIsRehashing(d)) _dictRehashStep(d);

        // 如果正在 rehash ，那么将 1 号哈希表也作为随机查找的目标
        if (dictIsRehashing(d)) {
                // T = O(N)
                do {
                        h = random() % (d->ht[0].size+d->ht[1].size);
                        he = (h >= d->ht[0].size) ? d->ht[1].table[h - d->ht[0].size] :
                             d->ht[0].table[h];
                } while(he == NULL);
                // 否则，只从 0 号哈希表中查找节点
        } else {
                // T = O(N)
                do {
                        h = random() & d->ht[0].sizemask;
                        he = d->ht[0].table[h];
                } while(he == NULL);
        }

        /* Now we found a non empty bucket, but it is a linked
         * list and we need to get a random element from the list.
         * The only sane way to do so is counting the elements and
         * select a random index. */
        // 目前 he 已经指向一个非空的节点链表
        // 程序将从这个链表随机返回一个节点
        listlen = 0;
        orighe = he;
        // 计算节点数量, T = O(1)
        while(he) {
                he = he->next;
                listlen++;
        }
        // 取模，得出随机节点的索引
        listele = random() % listlen;
        he = orighe;
        // 按索引查找节点
        // T = O(1)
        while(listele--) he = he->next;

        // 返回随机节点
        return he;
}

/* This is a version of dictGetRandomKey() that is modified in order to
 * return multiple entries by jumping at a random place of the hash table
 * and scanning linearly for entries.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements, and the elements are guaranteed to be non
 * repeating. */
int dictGetRandomKeys(dict *d, dictEntry **des, int count) {
        int j; /* internal hash table id, 0 or 1. */
        int stored = 0;

        if (dictSize(d) < count) count = dictSize(d);
        while(stored < count) {
                for (j = 0; j < 2; j++) {
                        /* Pick a random point inside the hash table 0 or 1. */
                        unsigned int i = random() & d->ht[j].sizemask;
                        int size = d->ht[j].size;

                        /* Make sure to visit every bucket by iterating 'size' times. */
                        while(size--) {
                                dictEntry *he = d->ht[j].table[i];
                                while (he) {
                                        /* Collect all the elements of the buckets found non
                                         * empty while iterating. */
                                        *des = he;
                                        des++;
                                        he = he->next;
                                        stored++;
                                        if (stored == count) return stored;
                                }
                                i = (i+1) & d->ht[j].sizemask;
                        }
                        /* If there is only one table and we iterated it all, we should
                         * already have 'count' elements. Assert this condition. */
                        redis_assert(dictIsRehashing(d) != 0);
                }
        }
        return stored; /* Never reached. */
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
static unsigned long rev(unsigned long v) {
        unsigned long s = 8 * sizeof(v); // bit size; must be power of 2
        unsigned long mask = ~0;
        while ((s >>= 1) > 0) {
                mask ^= (mask << s);
                v = ((v >> s) & mask) | ((v << s) & ~mask);
        }
        return v;
}

/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * dictScan() 函数用于迭代给定字典中的元素。
 *
 * Iterating works in the following way:
 *
 * 迭代按以下方式执行：
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 *    一开始，你使用 0 作为游标来调用函数。
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value that you must use in the next call.
 *    函数执行一步迭代操作，
 *    并返回一个下次迭代时使用的新游标。
 * 3) When the returned cursor is 0, the iteration is complete.
 *    当函数返回的游标为 0 时，迭代完成。
 *
 * The function guarantees that all the elements that are present in the
 * dictionary from the start to the end of the iteration are returned.
 * However it is possible that some element is returned multiple time.
 *
 * 函数保证，在迭代从开始到结束期间，一直存在于字典的元素肯定会被迭代到，
 * 但一个元素可能会被返回多次。
 *
 * For every element returned, the callback 'fn' passed as argument is
 * called, with 'privdata' as first argument and the dictionar entry
 * 'de' as second argument.
 *
 * 每当一个元素被返回时，回调函数 fn 就会被执行，
 * fn 函数的第一个参数是 privdata ，而第二个参数则是字典节点 de 。
 *
 * HOW IT WORKS.
 * 工作原理
 *
 * The algorithm used in the iteration was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits, that is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * 迭代所使用的算法是由 Pieter Noordhuis 设计的，
 * 算法的主要思路是在二进制高位上对游标进行加法计算
 * 也即是说，不是按正常的办法来对游标进行加法计算，
 * 而是首先将游标的二进制位翻转（reverse）过来，
 * 然后对翻转后的值进行加法计算，
 * 最后再次对加法计算之后的结果进行翻转。
 *
 * This strategy is needed because the hash table may be resized from one
 * call to the other call of the same iteration.
 *
 * 这一策略是必要的，因为在一次完整的迭代过程中，
 * 哈希表的大小有可能在两次迭代之间发生改变。
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * always by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * 哈希表的大小总是 2 的某个次方，并且哈希表使用链表来解决冲突，
 * 因此一个给定元素在一个给定表的位置总可以通过 Hash(key) & SIZE-1
 * 公式来计算得出，
 * 其中 SIZE-1 是哈希表的最大索引值，
 * 这个最大索引值就是哈希表的 mask （掩码）。
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will be always
 * the last four bits of the hash output, and so forth.
 *
 * 举个例子，如果当前哈希表的大小为 16 ，
 * 那么它的掩码就是二进制值 1111 ，
 * 这个哈希表的所有位置都可以使用哈希值的最后四个二进制位来记录。
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 * 如果哈希表的大小改变了怎么办？
 *
 * If the hash table grows, elements can go anyway in one multiple of
 * the old bucket: for example let's say that we already iterated with
 * a 4 bit cursor 1100, since the mask is 1111 (hash table size = 16).
 *
 * 当对哈希表进行扩展时，元素可能会从一个槽移动到另一个槽，
 * 举个例子，假设我们刚好迭代至 4 位游标 1100 ，
 * 而哈希表的 mask 为 1111 （哈希表的大小为 16 ）。
 *
 * If the hash table will be resized to 64 elements, and the new mask will
 * be 111111, the new buckets that you obtain substituting in ??1100
 * either 0 or 1, can be targeted only by keys that we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * 如果这时哈希表将大小改为 64 ，那么哈希表的 mask 将变为 111111 ，
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger, and will
 * just continue iterating with cursors that don't have '1100' at the end,
 * nor any other combination of final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, If a combination of the lower three bits (the mask for size 8
 * is 111) was already completely explored, it will not be visited again
 * as we are sure that, we tried for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 * 等等。。。在 rehash 的时候可是会出现两个哈希表的阿！
 *
 * Yes, this is true, but we always iterate the smaller one of the tables,
 * testing also all the expansions of the current cursor into the larger
 * table. So for example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 * 限制
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 * 这个迭代器是完全无状态的，这是一个巨大的优势，
 * 因为迭代可以在不使用任何额外内存的情况下进行。
 *
 * The disadvantages resulting from this design are:
 * 这个设计的缺陷在于：
 *
 * 1) It is possible that we return duplicated elements. However this is usually
 *    easy to deal with in the application level.
 *    函数可能会返回重复的元素，不过这个问题可以很容易在应用层解决。
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving.
 *    为了不错过任何元素，
 *    迭代器需要返回给定桶上的所有键，
 *    以及因为扩展哈希表而产生出来的新表，
 *    所以迭代器必须在一次迭代中返回多个元素。
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 *    对游标进行翻转（reverse）的原因初看上去比较难以理解，
 *    不过阅读这份注释应该会有所帮助。
 */
unsigned long dictScan(dict *d,
                       unsigned long v,
                       dictScanFunction *fn,
                       void *privdata)
{
        dictht *t0, *t1;
        const dictEntry *de;
        unsigned long m0, m1;

        // 跳过空字典
        if (dictSize(d) == 0) return 0;

        // 迭代只有一个哈希表的字典
        if (!dictIsRehashing(d)) {

                // 指向哈希表
                t0 = &(d->ht[0]);

                // 记录 mask
                m0 = t0->sizemask;

                /* Emit entries at cursor */
                // 指向哈希桶
                de = t0->table[v & m0];
                // 遍历桶中的所有节点
                while (de) {
                        fn(privdata, de);
                        de = de->next;
                }

                // 迭代有两个哈希表的字典
        } else {

                // 指向两个哈希表
                t0 = &d->ht[0];
                t1 = &d->ht[1];

                /* Make sure t0 is the smaller and t1 is the bigger table */
                // 确保 t0 比 t1 要小
                if (t0->size > t1->size) {
                        t0 = &d->ht[1];
                        t1 = &d->ht[0];
                }

                // 记录掩码
                m0 = t0->sizemask;
                m1 = t1->sizemask;

                /* Emit entries at cursor */
                // 指向桶，并迭代桶中的所有节点
                de = t0->table[v & m0];
                while (de) {
                        fn(privdata, de);
                        de = de->next;
                }

                /* Iterate over indices in larger table that are the expansion
                 * of the index pointed to by the cursor in the smaller table */
                // Iterate over indices in larger table             // 迭代大表中的桶
                // that are the expansion of the index pointed to   // 这些桶被索引的 expansion 所指向
                // by the cursor in the smaller table               //
                do {
                        /* Emit entries at cursor */
                        // 指向桶，并迭代桶中的所有节点
                        de = t1->table[v & m1];
                        while (de) {
                                fn(privdata, de);
                                de = de->next;
                        }

                        /* Increment bits not covered by the smaller mask */
                        v = (((v | m0) + 1) & ~m0) | (v & m0);

                        /* Continue while bits covered by mask difference is non-zero */
                } while (v & (m0 ^ m1));
        }

        /* Set unmasked bits so incrementing the reversed cursor
         * operates on the masked bits of the smaller table */
        v |= ~m0;

        /* Increment the reverse cursor */
        v = rev(v);
        v++;
        v = rev(v);

        return v;
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
        if (dictIsRehashing(d)) {
                return DICT_OK;
        }
        /*初始化哈希表*/
        if (d->ht[0].size == 0) {
                return dictExpand(d, DICT_HT_INITIAL_SIZE);
        }
        /*
         * 1. 已经使用的/字典大小 比率接近1:1 且dict_can_resize为真
         * 2. 超过dict_force_resize_ratio
         */
        if ((d->ht[0].used >= d->ht[0].size && dict_can_resize) ||
            (d->ht[0].used / d->ht[0].size > dict_force_resize_ratio)) {
                return dictExpand(d, 2 * d->ht[0].used);
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
        h = dictHashKey(d, key);
        for (table = 0; table <= 1; table++) {
                idx = h & d->ht[table].sizemask;
                he = d->ht[table].table[idx];
                while (he) {
                        /*key已经存在了*/
                        if (dictCompareKeys(d, key, he->key)) {
                                return -1;
                        }
                        he = he->next;
                }
                /*
                 * 第一次到此处说明0表中无key
                 * 看是否在rehash,如果在rehash就在1号表中插入
                 * 否则就跳出
                 */
                if (!dictIsRehashing(d)) {
                        break;
                }
        }
        return idx;
}

/*清空所有的哈希表节点*/
void dictEmpty(dict *d, void (callback)(void *))
{
        _dictClear(d, &d->ht[0], callback);
        _dictClear(d, &d->ht[1], callback);
        d->rehashidx = -1;
        d->iterators = 0;
}

/*开启自动rehash*/
void dictEnableResize(void)
{
        dict_can_resize = 1;
}

void dictDisableResize(void)
{
        dict_can_resize = 0;
}



