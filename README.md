把skynet C 的部分，用c++ 方式写一下，了解其内容，兼容win32 和 linux。
额外增加了一个 ylt 的库，主要用于一些模板和序列化的功能。
在skynet 原有的基础上扩展了一个 cpp 的 rpc 调用。

使用以下编译编译通过:
  gcc13.2.0
  clang18.1
  msvc143

main.cpp :
1. 一个pingpong 的tcp示例
2. 附带一个 rpc的调用。

TODO:
1. C++ 协程
2. lua 绑定
3. 仿照skynet 的cluser 或者 自己做一个简单的协议支持 remote
4. module base 改成纯模板，不再使用虚基类
