把skynet C 的部分，用c++ 方式写一下，了解其内容，兼容win32 和 linux。
额外增加了一个 ylt 的库，主要用于一些模板和序列化的功能。
在skynet 原有的基础上扩展了一个 cpp 的 rpc 调用。

main.cpp 中做了一个pingpong 示例，附带一个 rpc的调用。

TODO:
1. C++ 协程
2. lua 绑定
