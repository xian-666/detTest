#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
// Thread safe implementation of a Queue using a std::queue
template <typename T>
class YSSafeQueue
{
private:
    std::queue<T> m_queue; //利用模板函数构造队列
    std::mutex m_mutex;    // 访问互斥信号量
    std::condition_variable m_cond;//條件變量

public:
    YSSafeQueue() {}
    YSSafeQueue(YSSafeQueue &&other) {}
    ~YSSafeQueue() {}

    void clear()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        std::queue<T> empty;
        m_queue.swap(empty);
    }

    bool empty() //返回队列是否为空
    {
        std::unique_lock<std::mutex> lock(m_mutex); // 互斥信号变量加锁，防止m_queue被改变
        return m_queue.empty();
    }
    int size()
    {
        std::unique_lock<std::mutex> lock(m_mutex); // 互斥信号变量加锁，防止m_queue被改变
        return m_queue.size();
    }
    // 队列添加元素
    void enqueue(const T &t)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_queue.emplace(t);
    }

    void enqueue(T&& t)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_queue.emplace(std::move(t));
    }

    void enqueue_and_notify(const T& t)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_queue.emplace(t);
        lock.unlock();
        m_cond.notify_one();
    }

    void enqueue_and_notify(T&& t)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_queue.emplace(std::move(t));
        lock.unlock();
        m_cond.notify_one();
    }

    // 队列取出元素
    bool dequeue(T &t)
    {
        std::unique_lock<std::mutex> lock(m_mutex); // 队列加锁
        if (m_queue.empty())
            return false;
        t = std::move(m_queue.front()); // 取出队首元素，返回队首元素值，并进行右值引用
        m_queue.pop();                  // 弹出入队的第一个元素
        return true;
    }
    //等待條件變量并出隊
    void wait_and_dequeue(T& t)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_queue.empty())
            m_cond.wait(lock);
        t = std::move(m_queue.front()); // 取出队首元素，返回队首元素值，并进行右值引用
        m_queue.pop();                  // 弹出入队的第一个元素
    }

};