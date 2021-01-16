/* memory allocating related
 * @author Elio Yang
 * @email  jluelioyang2001@gamil.com
 * @date 2021/1/15
 */

#include <stdio.h>
#include <stdlib.h>

/* This function provide us access to the original libc free(). This is useful
 * for instance to free results obtained by backtrace_symbols(). We need
 * to define this function before including zmalloc.h that may shadow the
 * free implementation if we use jemalloc or another non standard allocator. */
void zlibc_free(void *ptr)
{
        free(ptr);
}

#include <string.h>
#include <pthread.h>
#include "config.h"
#include "zmalloc.h"

#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#else
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(long long))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
#endif


#ifdef HAVE_ATOMIC
#define update_zmalloc_stat_add(__n) __sync_add_and_fetch(&used_memory, (__n))
#define update_zmalloc_stat_sub(__n) __sync_sub_and_fetch(&used_memory, (__n))
#else
/*
 * 线程安全方法更新，使用互斥锁(mutex)保证线程安全
 * 由update_zmalloc_stat_alloc调用
 */
#define update_zmalloc_stat_add(__n) do { \
    pthread_mutex_lock(&used_memory_mutex); \
used_memory += (__n); \
    pthread_mutex_unlock(&used_memory_mutex); \
} while(0)
#define update_zmalloc_stat_sub(__n) do { \
    pthread_mutex_lock(&used_memory_mutex); \
    used_memory -= (__n); \
    pthread_mutex_unlock(&used_memory_mutex); \
} while(0)

#endif

/*
 * 使得变量used_memory精确的维护实际分配的内存
 * 有线程安全保证
 */
#define update_zmalloc_stat_alloc(__n) do { \
    size_t _n = (__n); \
    if (_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)); \
    if (zmalloc_thread_safe) { \
        update_zmalloc_stat_add(_n); \
    } else { \
        used_memory += _n; \
    } \
} while(0)

#define update_zmalloc_stat_free(__n) do { \
    size_t _n = (__n); \
    if (_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)); \
    if (zmalloc_thread_safe) { \
        update_zmalloc_stat_sub(_n); \
    } else { \
        used_memory -= _n; \
    } \
} while(0)

/*已经使用的内存*/
static size_t used_memory = 0;
/*线程安全标志 全局静态变量*/
static int zmalloc_thread_safe = 0;
/*互斥锁*/
pthread_mutex_t used_memory_mutex = PTHREAD_MUTEX_INITIALIZER;

/*错误处理,并退出*/
static void zmalloc_default_oom(size_t size)
{
        fprintf(stderr, "zmalloc: Out of memory trying to allocate %zu bytes\n",
                size);
        fflush(stderr);
        abort();
}

static void (*zmalloc_oom_handler)(size_t) = zmalloc_default_oom;


/*
 * zmalloc ， zcalloc ，zrealloc
 * 都申请了多一个PREFIX_SZIE 的内存大小，并与字长对齐
 */
void *zmalloc(size_t size)
{
        /*size 为实际需要的大小
         * PREFIX_SIZE 为预编译宏：根据机器而定,用于存储size的值*/
        void *ptr = malloc(size + PREFIX_SIZE);
        /*错误处理:调用函数default_oom*/
        if (!ptr) zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
        update_zmalloc_stat_alloc(zmalloc_size(ptr));
        return ptr;
#else
        /*分配内存的第一个字长放上 size*/
        *((size_t *) ptr) = size;
        /*更新已经使用的内存大小 全局量*/
        update_zmalloc_stat_alloc(size + PREFIX_SIZE);
        /*向右偏移PREFIX_SIZE 此时指针指向的空间大小就是size*/
        //     +--------------+-----------------+
        //     | PREFIX_SIZE  | size            |
        //     +--------------+-----------------+
        //     ^              ^
        //     |              |
        //    ptr            (char*)ptr+PREFIX_SIZE
        return (char *) ptr + PREFIX_SIZE;
#endif
}


void *zcalloc(size_t size)
{
        /*
         * calloc是线程安全函数
         * 分配的内存大小为 num*size
         * 并初始化为0
         */
        void *ptr = calloc(1, size + PREFIX_SIZE);

        if (!ptr) zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
        update_zmalloc_stat_alloc(zmalloc_size(ptr));
        return ptr;
#else
        *((size_t *) ptr) = size;
        update_zmalloc_stat_alloc(size + PREFIX_SIZE);
                return (char *) ptr + PREFIX_SIZE;
#endif
        }

/*重新分配内存*/
void *zrealloc(void *ptr, size_t size)
{
#ifndef HAVE_MALLOC_SIZE
        void *realptr;
#endif
        size_t oldsize;
        void *newptr;

        /*重新申请一块内存并返回*/
        if (ptr == NULL) return zmalloc(size);
#ifdef HAVE_MALLOC_SIZE
        oldsize = zmalloc_size(ptr);
            /*calloc重新申请内存*/
        newptr = realloc(ptr,size);
        if (!newptr) zmalloc_oom_handler(size);
            /*free原来的内存*/
        update_zmalloc_stat_free(oldsize);
            /*更新全局量 used_memory*/
        update_zmalloc_stat_alloc(zmalloc_size(newptr));
        return newptr;
#else
        /*向前PREFIX_SIZE*/
        realptr = (char *) ptr - PREFIX_SIZE;
        /*原来内存的大小*/
        oldsize = *((size_t *) realptr);
        /*重新申请内存*/
        newptr = realloc(realptr, size + PREFIX_SIZE);
        if (!newptr) zmalloc_oom_handler(size);
        /*存储size*/
        *((size_t *) newptr) = size;
        /*free原来的空间*/
        update_zmalloc_stat_free(oldsize);
        /*更新全局量 used_memory*/
        update_zmalloc_stat_alloc(size);
                return (char *) newptr + PREFIX_SIZE;
#endif
}

/* Provide zmalloc_size() for systems where this function is not provided by
 * malloc itself, given that in that case we store a header with this
 * information as the first bytes of every allocation.
 *
 */
#ifndef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr)
{
        /* malloc的内存的大小*/
        /*向前偏移一个字长*/
        void *realptr = (char *) ptr - PREFIX_SIZE;
        /*获得大小*/
        size_t size = *((size_t *) realptr);

        /* Assume at least that all the allocations are padded at sizeof(long) by
         * the underlying allocator. */
        /*内存对齐*/
        if (size & (sizeof(long) - 1)) size += sizeof(long) - (size & (sizeof(long) - 1));
        return size + PREFIX_SIZE;
}
#endif


void zfree(void *ptr)
{
#ifndef HAVE_MALLOC_SIZE
        void *realptr;
        size_t oldsize;
#endif

        if (ptr == NULL) return;
#ifdef HAVE_MALLOC_SIZE
        update_zmalloc_stat_free(zmalloc_size(ptr));
        free(ptr);
#else
        /*向前偏移字长，回到最初malloc返回的地址*/
        realptr = (char *) ptr - PREFIX_SIZE;
        /*也就是zmalloc中的内存大小size*/
        oldsize = *((size_t *) realptr);
        /*更新内存大小*/
        update_zmalloc_stat_free(oldsize + PREFIX_SIZE);
        /*free掉malloc的空间*/
        free(realptr);
#endif
}

/*复制字符串*/
char *zstrdup(const char *s)
{
        size_t l = strlen(s) + 1;
        char *p = zmalloc(l);
        memcpy(p, s, l);
        return p;
}

/*返回used_memory 线程安全*/
size_t zmalloc_used_memory(void)
{
        size_t um;

        if (zmalloc_thread_safe) {
#ifdef HAVE_ATOMIC
                um = __sync_add_and_fetch(&used_memory, 0);
#else
                pthread_mutex_lock(&used_memory_mutex);
                um = used_memory;
                pthread_mutex_unlock(&used_memory_mutex);
#endif
        }/*保证线程安全*/
        else {
                um = used_memory;
        }/*不用保证*/

        return um;
}

/*
 * 是否需要线程安全
 * 0代表不需要
 */
void zmalloc_enable_thread_safeness(void)
{
        zmalloc_thread_safe = 1;
}

/* oom状态下采取的操作: out of memory
 * 默认为 zmalloc_default_oom()
 */
void zmalloc_set_oom_handler(void (*oom_handler)(size_t))
{
        zmalloc_oom_handler = oom_handler;
}

/* Get the RSS information in an OS-specific way.
 *
 * WARNING: the function zmalloc_get_rss() is not designed to be fast
 * and may not be called in the busy loops where Redis tries to release
 * memory expiring or swapping out objects.
 *
 * For this kind of "fast RSS reporting" usages use instead the
 * function RedisEstimateRSS() that is a much faster (and less precise)
 * version of the function.
 */

/*
 * RSS: resident set size
 * 当前进程实际所驻留在内存中的空间大小
 * 具体实现与OS相关
 */


#if defined(HAVE_PROC_STAT)
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
size_t zmalloc_get_rss(void) {
    /*sysconf为系统函数，获取页大小*/
    int page = sysconf(_SC_PAGESIZE);
    size_t rss;
    char buf[4096];
    char filename[256];
    int fd, count;
    char *p, *x;

        /*将当前进程所对应的stat文件的绝对路径保存到filename*/
    snprintf(filename,256,"/proc/%d/stat",getpid());
        /*只读打开stat文件*/
    if ((fd = open(filename,O_RDONLY)) == -1) return 0;
    if (read(fd,buf,4096) <= 0) {
        close(fd);
        return 0;
    }
    close(fd);
        /*读完信息，存到buf*/
    p = buf;
    count = 23; /* RSS is the 24th field in /proc/<pid>/stat */
    while(p && count--) {
                /*查找空格，由空格分隔字段*/
        p = strchr(p,' ');
                /*指向下一个字段首地址*/
        if (p) p++;
    }
    if (!p) return 0;
    x = strchr(p,' ');
    if (!x) return 0;
    *x = '\0';
        /*string to long long*/
    rss = strtoll(p,NULL,10);
        /*rss获取的是内存页的页数，乘以页大小即可知*/
    rss *= page;
    return rss;
}
#else
size_t zmalloc_get_rss(void)
{
        /* If we can't get the RSS in an OS-specific way for this system just
         * return the memory usage we estimated in zmalloc()..
         *
         * Fragmentation will appear to be always 1 (no fragmentation)
         * of course... */
        return zmalloc_used_memory();
}
#endif

/*
 * Fragmentation = RSS / allocated-bytes
 * 内存碎片率
 */
float zmalloc_get_fragmentation_ratio(size_t rss)
{
        return (float) rss / zmalloc_used_memory();
}
