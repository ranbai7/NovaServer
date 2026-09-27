#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

//与套接字相关的无状态操作。项目里每种只有一处使用场景，因此按自由函数提供，
//不包装成类——这也是 ROADMAP 明确不引入「地址与套接字的 RAII 包装类」的落点
namespace net
{
//设为非阻塞。失败即终止：本服务端的所有 IO 都建立在非阻塞之上，
//一处设置失败意味着后续所有读写的语义都不成立，继续运行没有意义
void set_nonblocking(int fd);
} // namespace net

#endif
