#ifndef locker_h
#define locker_h

#include <exception>
#include <pthread.h>
#include <semaphore.h>//信号量头文件

/* 封装信号量 */
class sem{
public:
    sem(){
        /* 第二个参数pshare=0表示进程的局部信号量，第三个为初始值 */
        if(sem_init(&m_sem,0,0)!=0){
            throw std::exception();
        }
    }
    ~sem(){
        sem_destroy(&m_sem);
    }
    bool wait(){
        return sem_wait(&m_sem)==0;//将信号量的值减1
    }
    bool post(){
        return sem_post(&m_sem)==0;//将信号量的值加1
    }
private:
    sem_t m_sem;
};

/*封装互斥锁*/
class locker{
public:
    locker(){
        if(pthread_mutex_init(&m_mutex,NULL)!=0){
            throw std::exception();
        }
    }
    ~locker(){
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock(){
        return pthread_mutex_lock(&m_mutex)==0;
    }
    bool unlock(){
        return pthread_mutex_unlock(&m_mutex)==0;
    }
private:
    pthread_mutex_t m_mutex;
};

/* 封装条件变量 */
class cond{
    /* pthread_cond_wait()函数执行流程
     * 互斥量mutex先加锁--------------------|
     * 调用者把加锁的互斥量传给函数            |
     * 把调用线程放入条件变量的等待队列中       |这段时间pthread_cond_broadcast，pthread_cond_signal不会修改条件变量
     * 然后将互斥量mutex解锁-----------------|
     * 函数返回，互斥量再次被锁住
     * 对互斥量mutex解锁
     */
public:
    cond(){
        if(pthread_mutex_init(&m_mutex,NULL)!=0){
            throw std::exception();
        }
        if(pthread_cond_init(&m_cond,NULL)!=0){
            pthread_mutex_destroy(&m_mutex);//构造函数一旦出现问题，立即释放分配的资源
            throw std::exception();
        }
    }
    ~cond(){
        pthread_mutex_destroy(&m_mutex);
        pthread_cond_destroy(&m_cond);
    }
    bool wait(){
        int ret=0;
        pthread_mutex_lock(&m_mutex);
        ret=pthread_cond_wait(&m_cond,&m_mutex);
        pthread_mutex_unlock(&m_mutex);
        return ret==0;
    }
    bool signal(){
        return pthread_cond_signal(&m_cond)==0;
    }
private:
    pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
};

#endif /* locker_h */
