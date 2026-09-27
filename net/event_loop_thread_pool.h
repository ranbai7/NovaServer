#ifndef EVENT_LOOP_THREAD_POOL_H
#define EVENT_LOOP_THREAD_POOL_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

class EventLoop;

//子 Reactor 线程池：每个线程持有自己的事件循环（各自的 epoll、eventfd 与
//定时器队列），新连接按轮转分配，使连接在子线程间均匀分布。
//
//「主线程只 accept、连接归属某个子循环」是这套模型的全部要点：连接一旦分配，
//它的读写、协议解析与超时定时器就都在那个线程内完成，跨线程只传递连接对象本身
class EventLoopThreadPool
{
public:
    //thread_num 为子 Reactor 线程数。为 0 时不建线程，全部归主循环处理
    EventLoopThreadPool(EventLoop *base_loop, int thread_num);
    ~EventLoopThreadPool();

    EventLoopThreadPool(const EventLoopThreadPool &) = delete;
    EventLoopThreadPool &operator=(const EventLoopThreadPool &) = delete;

    //启动全部子线程，并阻塞到各自的事件循环就绪：此后 next_loop() 返回的
    //指针保持稳定
    void start();
    //轮转取下一个子循环。thread_num 为 0 时返回主循环
    EventLoop *next_loop();
    //请求所有子循环退出并等待线程结束。可重复调用
    void stop();

private:
    void thread_func(int index);

    EventLoop *m_base_loop;
    const int m_thread_num;
    std::atomic<int> m_next;

    std::vector<std::thread> m_threads;
    //线程自己填进来，退出前清空。仅在 start() 的等待期间与 stop() 之后需要
    //同步，运行期间只读
    std::vector<EventLoop *> m_loops;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    int m_started;
    bool m_stopped;
};

#endif
