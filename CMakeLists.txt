cmake_minimum_required(VERSION 3.15)
project(MY_REDIS)

set(CMAKE_C_STANDARD 99)

# 遍历项目(三级目录)所有的 .cpp 文件
#file (GLOB files *.cpp */*.cpp */*/*.cpp )
file (GLOB_RECURSE files *.c)
foreach (file ${files})
    string(REGEX REPLACE ".+/(.+)\\..*" "\\1" exe ${file})
    add_executable (${exe} ${file})
    message (\ \ \ \ --\ src/${exe}.c\ will\ be\ compiled\ to\ bin/${exe})
endforeach ()
#file (GLOB_RECURSE files *.cpp)