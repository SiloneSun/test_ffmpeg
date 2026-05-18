# 指定 CMake 最低版本要求
cmake_minimum_required(VERSION 3.10)

# 项目名称和使用的语言（CXX 表示 C++）
project(my_test CXX)

# 设置 C++ 语言标准为 C++11
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================
# 源文件目录（所有 .c/.cpp 文件自动参与编译）
# ============================================
# 递归收集 src/ 目录下所有 .c 和 .cpp 文件
file(GLOB_RECURSE SOURCES
    main.cpp
    json11.cpp
    src/*.c
    src/*.cpp
)

# ============================================
# 头文件目录（直接 include 即可找到）
# ============================================
# 指定头文件搜索路径
include_directories(
    include
    /home/sunxilong/work/mycode/ffmpeg-snapshot-git/my_build/include
    /home/sunxilong/work/mycode/ffmpeg-snapshot-git/my_build/include/libavformat
)

# ============================================
# 链接库设置
# ============================================

# ---------- 方式一：指定链接库目录（等价于 -L） ----------
# link_directories 相当于 gcc/clang 的 -L 选项
# 告诉链接器去哪些目录查找库文件（.a / .so / .lib）
link_directories(
    lib
    /home/sunxilong/work/mycode/ffmpeg-snapshot-git/my_build/lib
    /usr/lib
    /usr/lib/x86_64-linux-gnu
    # /usr/local/lib                          # ffmpeg 默认安装路径
    # /path/to/other/libs                   # 其他库路径
)

# ---------- 方式二：链接具体的库（等价于 -l） ----------
# 使用 target_link_libraries 指定需要链接的库
# CMake 会自动处理 -l 前缀，你只需要写库名（不带 lib 前缀和扩展名）

# 先定义可执行文件
add_executable(my_test ${SOURCES})

# 然后链接库到目标
# 语法：target_link_libraries(目标名 [PUBLIC|PRIVATE|INTERFACE] 库1 库2 ...)
# PUBLIC/PRIVATE/INTERFACE 表示链接的可见性：
#   - PRIVATE:  仅当前目标链接该库（最常用）
#   - PUBLIC:   当前目标及依赖它的目标都链接
#   - INTERFACE: 仅依赖当前目标的目标链接，当前目标不链接
#  我的备注：  当链接器处理 avformat（它依赖 avcodec 和 avutil）时，它发现缺符号，它会往后找。如果你的 avcodec 和 avutil 放在了 avformat 前面（或者顺序混乱），链接器在处理 avformat 时可能已经“路过”了 avutil，导致它认为那些符号不需要，或者在处理 avcodec 时还没看到 avutil。 所以顺序上必须 按照下面顺序：
target_link_libraries(my_test PRIVATE
    # --- 链接 ffmpeg 的核心库 ---
    avdevice            # -lavdevice  （设备库）
    avfilter            # -lavfilter  （滤镜库）
    avformat            # -lavformat  （封装格式库）
    avcodec             # -lavcodec   （编解码库）
    swscale             # -lswscale   （图像缩放/色彩空间转换）
    swresample          # -lswresample（音频重采样）
    avutil              # -lavutil    （工具库）
    

    # --- 其他常见第三方库示例（根据实际需要启用） ---
    # x264               # -lx264     （H.264 编码器）
    # x265               # -lx265     （H.265 编码器）
    # mp3lame            # -lmp3lame  （MP3 编码器）
    # fdk-aac            # -lfdk-aac  （AAC 编码器）
    lzma                # -llzma
    z                  # -lz        （zlib 压缩库）
    # pthread            # -lpthread  （POSIX 线程）
    m                  # -lm        （数学库）
    pthread            # -lpthread
)

# ============================================
# 补充：更推荐的做法（使用 find_package）
# ============================================
# 上面的 link_directories 是手动指定路径，缺点是路径写死了，移植性差
# 更推荐的做法是用 find_package 自动查找库及其路径，例如：
#
#   find_package(PkgConfig REQUIRED)
#   pkg_check_modules(LIBAVCODEC REQUIRED libavcodec)
#   if(LIBAVCODEC_FOUND)
#       include_directories(${LIBAVCODEC_INCLUDE_DIRS})
#       target_link_libraries(my_test PRIVATE ${LIBAVCODEC_LIBRARIES})
#   endif()
#
# 但对于 ffmpeg，直接用 target_link_libraries 指定库名通常就够了，
# 前提是库文件在系统默认路径（如 /usr/lib, /usr/local/lib）中。
# 如果不在默认路径，才需要用 link_directories 或 target_link_directories 指定路径。

# ============================================
# 更精细的库路径控制（CMake 3.13+）
# ============================================
# 如果不想全局设置 link_directories（影响所有目标），
# 可以使用 target_link_directories 为特定目标指定路径：
#
#   target_link_directories(my_test PRIVATE
#       /usr/local/lib
#       /custom/lib/path
#   )