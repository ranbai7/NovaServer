#include "socket_utils.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace net
{
void set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        std::perror("set_nonblocking");
        std::exit(EXIT_FAILURE);
    }
}
} // namespace net
