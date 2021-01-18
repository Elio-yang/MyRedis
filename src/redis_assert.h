/*
 * @author Elio Yang
 * @email  jluelioyang2001@gamil.com
 * @date 2021/1/18
 */

#ifndef MY_REDIS_REDIS_ASSERT_H
#define MY_REDIS_REDIS_ASSERT_H
#include <unistd.h>
#define redis_assert(_e) ((_e)?(void)0:(_redisAssert(#_e,__FILE__,__LINE__),exit(1)))
void _redisAssert(char *estr,char *file,int line);
#endif //MY_REDIS_REDIS_ASSERT_H
