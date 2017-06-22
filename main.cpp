#include <string.h>//memset()
#include <signal.h>//信号处理，sigaction,sigfillset()
#include <assert.h>//异常处理，断言
#include <stdlib.h>//字符串转换，atoi()
#include <errno.h>//使用errno变量
#include <stdio.h>//printf()
/* unix std的意思，是POSIX标准定义的unix类系统定义符号常量的头文件，包含类许多UNIX系统服务的函数原型
 * read,unlink,write,usleep,sleep,access,alarm,chdir,chown,close,confstr,_exit,fork
 * SEEK_CUR,SEEK_END,SEEK_SET
 * */
#include <unistd.h>//提供通用的文件、目录、程序及进程操作的函数
#include <arpa/inet.h>//主要定义类格式转换函数，比如IP地址转换函数，linger结构体
#include <netinet/in.h>//互联网地址族，定义数据结构sockaddr_in
#include <sys/epoll.h>//epoll所有函数的头文件
/* 网络编程常用类型，如fd_set,pid_t,clock_t,time_t,mode_t,ino_t,fpos_t,size_t,ssize_t,uid_t,gid_t,dev_t,caddr_t等 */
#include <sys/types.h>
#include <sys/socket.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000

using namespace std;

/* 声明使用http_conn.cpp中定义的函数 */
extern int addfd(int epollfd,int fd,bool one_shot);
extern int removefd(int epollfd,int fd);

void addsig(int sig,void (*handler)(int),bool restart=true){
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler=handler;
    if(restart) sa.sa_flags|=SA_RESTART;//sa_flags用于设置程序收到信号时的行为
    sigfillset(&sa.sa_mask);//初始化信号集，使其包括所有信号,屏蔽所有信号，只处理sig
    assert(sigaction(sig, &sa, NULL)!=-1);//异常事件注册不出现错误
}

void show_error(int connfd,const char *info){
    printf("%s",info);
    send(connfd,info,strlen(info),0);
    close(connfd);
}

int main(int argc,const char *argv[]){
    if(argc<=2){
        printf("usage: %s ip_address port_number\n",basename(argv[0]));
        return 1;
    }
    const char *ip=argv[1];
    int port=atoi(argv[2]);

    addsig(SIGPIPE,SIG_IGN);//忽略SIGPIPE信号

    /* 创建线程池  */
    threadpool<http_conn> *pool=NULL;
    try{
        pool=new threadpool<http_conn>(4);
    }
    catch(...){
        return 1;
    }

    /* 预先为每个可能的客户连接分配一个http_conn对象
     * user数组的下标为文件描述符的值
     * */
    http_conn *users=new http_conn[MAX_FD];
    assert(users);
    int user_count=0;

    int listenfd=socket(PF_INET,SOCK_STREAM,0);
    assert(listenfd>=0);

    /* 指定函数close对面向连接的协议如何操作 */
    struct linger tmp={1,0};
    setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    int ret;
    struct sockaddr_in address;
    memset(&address,0,sizeof(address));
    address.sin_family=AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port=htons(port);

    ret=bind(listenfd,(struct sockaddr *)&address,sizeof(address));
    assert(ret>=0);

    ret=listen(listenfd,5);
    assert(ret>=0);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd=epoll_create(5);
    assert(epollfd!=-1);
    addfd(epollfd,listenfd,false);
    http_conn::m_epollfd=epollfd;

    while(true){
        int number=epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
        if((number<0)&&(errno!=EINTR)){
            printf("epoll failure\n");
            break;
        }
        for(int i=0;i<number;++i){
            int sockfd=events[i].data.fd;
            if(sockfd==listenfd){
                struct sockaddr_in client_address;
                socklen_t client_addrlength=sizeof(client_address);
                int connfd=accept(listenfd,(struct sockaddr *)&client_address,&client_addrlength);
                if(connfd<0){
                    printf("errno is: %d\n",errno);
                    continue;
                }
                if(http_conn::m_user_count>=MAX_FD){
                    show_error(connfd,"Internal server busy");
                    continue;
                }
                users[connfd].init(connfd,client_address);
            }
             /* EPOLLIN 连接到达，有数据来临
              * EPOLLRDHUP TCP连接被对方关闭，或者对方关闭了写操作
              * EPOLLHUP 挂起，相关联的描述符，比如说管道的写端被关闭后，读端将收到EPOLLHUP
              * 对端正常关闭，触发EPOLLIN和EPOLLHUP，但是不触发EPOLLERR和EPOLLHUP，只有本端错误才触发
              * EPOLLERR 服务器出错后，本方采取行动，收到EPOLLERR，说明对方已经异常断开
              * */
            else if(events[i].events&(EPOLLRDHUP|EPOLLHUP|EPOLLERR)){
                users[sockfd].close_conn();
            }
            else if(events[i].events&EPOLLIN){
                if(users[sockfd].read()){
                    pool->append(users+sockfd);
                }
                else{
                    users[sockfd].close_conn();
                }
            }
            else if(events[i].events&EPOLLOUT){
                if(!users[sockfd].write()){
                    users[sockfd].close_conn();
                }
            }
        }
    }
    close(epollfd);
    close(listenfd);
    delete []users;
    delete pool;
    return 0;
}
