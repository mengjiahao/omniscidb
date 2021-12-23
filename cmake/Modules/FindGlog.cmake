#.rst:
# FindGlog.cmake
# -------------
#
# Find a Google glog installation.
#
# This module finds if Google glog is installed and selects a default
# configuration to use.
#
# find_package(Glog ...)
#
#
# The following variables control which libraries are found::
#
#   Glog_USE_STATIC_LIBS  - Set to ON to force use of static libraries.
#
# The following are set after the configuration is done:
#
# ::
#
#   Glog_FOUND            - Set to TRUE if Glog was found.
#   Glog_INCLUDE_DIRS     - Include directories
#   Glog_LIBRARIES        - Path to the Glog libraries.
#   Glog_LIBRARY_DIRS     - compile time link directories
#
#
# Sample usage:
#
# ::
#
#    find_package(Glog)
#    if(Glog_FOUND)
#      target_link_libraries(<YourTarget> ${Glog_LIBRARIES})
#    endif()
#
# 如果找到这个链接库，则可以通过在工程的顶层目录中的CMakeLists.txt 文件添加include_directories(<LIBRARY_NAME>_INCLUDE_DIRS)来包含库的头文件，并添加target_link_libraries(Source_files <LIBRARY_NAME>_LIBRARIES)命令将源文件与库文件链接起来。
# 变量的列表可以使用命令 cmake –help-module FindBZip2 

if(Glog_USE_STATIC_LIBS)
  set(_CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
  # 设置查找静态库优先(跨平台).
  set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
endif()

# 在指定目录下查找 libglog.so/libglog.a的库(默认优先查找动态库)，并命名为 Glog_LIBRARY.
find_library(Glog_LIBRARY
  NAMES glog
  HINTS ENV LD_LIBRARY_PATH
  HINTS ENV DYLD_LIBRARY_PATH
  PATHS
  /usr/lib
  /usr/local/lib
  /usr/local/homebrew/lib
  /opt/local/lib)

# 在指定目录下查找头文件.
find_path(Glog_INCLUDE_DIR
  NAMES glog/logging.h
  HINTS ${Glog_LIBRARY}/../../include
  PATHS
  /usr/include
  /usr/local/include
  /usr/local/homebrew/include
  /opt/local/include)

# 获取文件的目录名.
get_filename_component(Glog_LIBRARY_DIR ${Glog_LIBRARY} DIRECTORY)

# Set standard CMake FindPackage variables if found.
# 导出cmake系统变量.
set(Glog_LIBRARIES ${Glog_LIBRARY})
set(Glog_INCLUDE_DIRS ${Glog_INCLUDE_DIR})
set(Glog_LIBRARY_DIRS ${Glog_LIBRARY_DIR})

find_package(Gflags)
if(GFLAGS_FOUND)
  # 添加库依赖.
  list(APPEND Glog_LIBRARIES ${Gflags_LIBRARIES})
endif()

find_library(Unwind_LIBRARY NAMES unwind)
if(Unwind_LIBRARY)
  list(INSERT Glog_LIBRARIES 0 ${Unwind_LIBRARY})
endif()

if(Glog_USE_STATIC_LIBS)
  # 还原库查找顺序 CMAKE_FIND_LIBRARY_SUFFIXES.
  set(CMAKE_FIND_LIBRARY_SUFFIXES ${_CMAKE_FIND_LIBRARY_SUFFIXES})
endif()

# 导入 FindPackageHandleStandardArgs 模块.
include(FindPackageHandleStandardArgs)
# 设置 <name>_FOUND 并打印或失败信息.
find_package_handle_standard_args(Glog REQUIRED_VARS Glog_LIBRARY Glog_INCLUDE_DIR)
