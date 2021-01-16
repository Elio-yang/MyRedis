/* memory allocating related
 * @author Elio Yang
 * @email  jluelioyang2001@gamil.com
 * @date 2021/1/15
 */

#ifndef __ZMALLOC_H
#define __ZMALLOC_H

/*stringfication*/
#define __xstr(s) __str(s)
#define __str(s) #s


void *zmalloc(size_t size);
void *zcalloc(size_t size);
void *zrealloc(void *ptr, size_t size);
void zfree(void *ptr);
char *zstrdup(const char *s);
size_t zmalloc_used_memory(void);
void zmalloc_enable_thread_safeness(void);
void zmalloc_set_oom_handler(void (*oom_handler)(size_t));
float zmalloc_get_fragmentation_ratio(size_t rss);
size_t zmalloc_get_rss(void);
size_t zmalloc_get_private_dirty(void);
void zlibc_free(void *ptr);

#ifndef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr);
#endif



#endif //MY_REDIS_ZMALLOC_H