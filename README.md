学习使用ffmpeg

ffmpeg： ffmpeg源码。单独仓库。
my_build: ffmpeg 编译后install得到的lib、include、bin
my_test: 测试、学习使用ffmpeg的代码


FFmpeg 底层有很多使用 x86 SIMD 指令集（如 MMX、SSE、AVX、AVX2 等）编写的汇编优化代码，用于加速视频编解码、色彩空间转换、音频重采样等操作。
nasm 就是用来把这些 .asm 汇编文件"翻译"成机器码的工具。没有它，这些优化代码就无法编译。

sudo apt install nasm

然后编译ffmpeg，我的配置如下：

./configure --prefix=$HOME/work/mycode/ffmpeg-snapshot-git/my_build
make -j$(nproc)
make install



没有编译ffplay ： 因为ffplay依赖sdl2，需要先安装 sudo apt install libsdl2-dev ， 才能触发自动编译 ffplay
