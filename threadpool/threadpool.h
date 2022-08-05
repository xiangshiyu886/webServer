// 防止该头文件被重复引用
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>    // 异常处理
#include <pthread.h>    //多线程相关操作头文件，可移植众多平台
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool {
public:
    // thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量
    threadpool(int actor_model, connection_pool* connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T* request, int state);
    bool append_p(T* request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    connection_pool *m_connPool;  //数据库
    int m_actor_model;          //模型切换(切换reactor/protactor)
};
template <typename T>
//线程池创建
threadpool<T>::threadpool(int actor_model, connection_pool* connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool) {
    if (thread_number <= 0 || max_requests <=0) {
        throw std::expection();
    }
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) {
        throw std::exception();
    }
    for (int i=0; i<thread_number; i++) {
        //创建成功则返回0，创建线程失败，关闭线程池
        if (pthread_create(m_threads+i, NULL, worker, this) != 0) {
            delete[] m_threads;
            throw std::exception()
        }
        /* 将线程属性更改为unjoinable，便于资源释放
        linux线程有两种状态，joinable和unjoinable状态
        joinable：调用pthread_join主线程阻塞等待子线程结束后，回收子线程资源
        unjoinable：调用pthread_detach，即主线程与子线程分离，子线程结束后，资源自动回收*/
        if (pthread_detach(m_threads[i])) {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
}
template <typename T>
bool threadpool<T>::append(T* request, int state) {
    m_queuelocker.unlock(); //互斥锁
    f (m_workqueue.size() >= m_max_requests) //请求队列中的请求数大于最大值
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}   
template <typename T>
//proactor模式下任务请求入队
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    /*调用时 *arg是this，所以下面的语句就是获取threadpool对象的地址
    线程吃中每一个线程创建都会调用run()，睡眠在队列中*/
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}
template <typename T>
void threadpool<T>::run()
{
    /*run()可以看作是一个回环事件，一直等待m_request()信号变量post，即新任务进入请求队列
    然后从和请求队列中取出一个任务进行处理*/
    while (true)
    {
        //线程池中所有的线程都睡眠，等待请求队列中新增任务
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;
        //线程开始进行任务处理
        if (1 == m_actor_model)
        {
            if (0 == request->m_state)
            {
                if (request->read_once())
                {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            else
            {
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}
#endif