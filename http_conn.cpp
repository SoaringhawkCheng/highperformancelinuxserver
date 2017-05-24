
#include <fcntl.h>//定义了很多宏和open,fcntl函数原型
#include <string.h>//memset()
#include <errno.h>//errno全局变量
#include <stdlib.h>//字符串转换，atoi()
#include <stdio.h>//printf()
#include <sys/epoll.h>//epoll_creat(),epoll_wait(),epoll_ctl(
#include <sys/socket.h>//提供socket函数及数据结构
#include <sys/stat.h>//stat结构体
#include "http_conn.h"

/* HTTP响应的状态信息 */
const char *ok_200_title="OK";
const char *error_400_title="Bad Request";
const char *error_400_form="Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title="Forbidden";
const char *error_403_form="You do not have permission to get file form this server.\n";
const char *error_404_title="Not Found";
const char *error_404_form="The requested file was not found on this server.\n";
const char *error_500_title="Internet Error";
const char *error_500_form="There was an unusual problem serving the requested file.\n";

/* 网站的根目录 */
const char *doc_root="var/www/html";

/* 函数系列1
 * 设置描述符属性的一系列函数
 * */

int setnonblocking(int fd){
    int old_option=fcntl(fd,F_GETFL);
    int new_option=old_option|O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

/* 往事件表里注册fd上的事件  */
void addfd(int epollfd,int fd,bool one_shot){
    epoll_event event;
    event.data.fd=fd;
    event.events=EPOLLIN|EPOLLET|EPOLLRDHUP;
    if(one_shot){
        event.events|=EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);
}

/* 删除fd上的注册事件  */
void removefd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
}

/* 修改fd上的注册事件 */
void modfd(int epollfd,int fd,int ev){
    epoll_event event;
    event.data.fd=fd;
    event.events=ev|EPOLLET|EPOLLONESHOT|EPOLLRDHUP;
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

int http_conn::m_usr_count=0;
int http_conn::m_epollfd=-1;

/* 函数系列2
 * 初始化和关闭客户连接
 * */

void http_conn::init(int sockfd,const sockaddr_in &addr){
    m_sockfd=sockfd;
    m_address=addr;
    /* 如下两行是为了避免TIME_WAIT状态，仅用于调试，实际使用时应去掉 */
    int reuse=1;
    setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd,sockfd,true);
    m_usr_count++;

    init();
}

void http_conn::init(){
    m_check_state=CHECK_STATE_REQUESTLINE;//初始状态，解析请求栏
    m_linger=false;

    m_method=GET;
    m_url=0;
    m_version=0;
    m_content_length=0;
    m_host=0;
    m_start_line=0;
    m_check_idx=0;
    m_read_idx=0;
    m_write_idx=0;

    memset(m_read_buf,0,READ_BUFFER_SIZE);
    memset(m_write_buf,0,WRITE_BUFFER_SIZE);
    memset(m_real_file,0,FILENAME_LEN);
}

void http_conn::close_conn(bool real_close){
    if(real_close&&(m_sockfd!=-1)){
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;
        m_user_count--;
    }
}

bool http_conn::read(){
    if(m_read_idx>=READ_BUFFER_SIZE){
        return false;
    }
    int bytes_read=0;
    while(true){
        bytes_read=recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        if(bytes_read==-1){
            /* 由非阻塞套接字上不能立即完成的操作返回
             * 如套接字上没有排队数据可读时调用了recv() */
            if(errno==EAGAIN||errno==EWOULDBLOCK){
                break;
            }
            return false;//其他错误直接退出
        }
        else if(bytes_read==0){
            return false;
        }
        m_read_idx+=bytes_read;
    }
    return true;
}

/* 由线程池中的工作线程调用，处理HTTP请求的入口函数 */
void http_conn::process(){
    HTTP_CODE read_ret=process_read();
    if(read_ret==NO_REQUEST){
        /* 重置EPOLLONESHOT事件，确保socket下次可读 */
        modfd(m_epollfd,sockfd,EPOLLIN);
        return;
    }
    bool write_ret=process_write(read_ret);
    if(!write_ret){
        close_conn();
    }
    modfd(m_epollfd,m_sockfd,EPOLLOUT);
    /* modfd传入EPOLLIN,EPOLLOUT，是为了同时处理输入输出套接字 */
}

/* 分析HTTP请求的入口函数  */
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;
    char *text=0;

    while(((m_check_state==CHECK_STATE_CONTENT)&&(line_status==LINE_OK))||((line_status=parse_line())=LINE_OK)){
        text=get_line();
        m_start_line=m_checked_idx;
        printf("got 1 http line: %s\n",text);

        switch(m_check_state){
            case CHECK_STATE_REQUESTLINE:
                {
                    ret=parse_request_line(text);
                    if(ret==BAD_REQUEST){
                        return BAD_REQUEST;
                    }
                    break;
                }
            case CHECK_STATE_HEADER:
                {
                    ret=parse_headers(text);
                    if(ret==BAD_REQUEST){
                        return BAD_REQUEST;
                    }
                    else if(ret==GET_REQUEST){
                        return do_request();
                    }
                    break;
                }
            case CHECK_STATE_CONTENT:
                {
                    ret=parse_content(text);
                    if(ret==GET_REQUEST){
                        return do_request();
                    }
                    line_status=LINE_OPEN;
                    break;
                }
            default:return INTERNAL_ERROR;
    }
    return NO_REQUEST;
}

/* 从状态机，用于解析一行的内容 */
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    for(;m_checked_idx<m_read_idx;++m_checked_idx){
        temp=m_read_buf[m_checked_idx];
        /* 当前的字节是'\r'，可能读取到完整的行 */
        if(temp=='\r'){
            /* '\r'是缓冲区最后一个已经被读入的客户数据，这次解析未读到完整的行，需要继续读取客户数据 */
            if((m_checked_idx+1)==m_read_idx){
                return LINE_OPEN;
            }
            /* 当前的字节是'\n'，即换行符，则说明读取到一个完整的行，把末尾的'\r'和'\n'换成'\0'  */
            else if(m_read_buf[m_check_idx+1]=='\n')//
                m_read_buf[m_check_idx++]='\0';
                m_read_buf[m_check_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;//'\r'不是缓冲区最后一个已经被读入的客户数据，'\r'后面不是'\n'，错误的行
        }
        else if(temp=='\n'){//当前字符是'\n'，则也说明可能读取到一个完整的行
            /* 前一个字符是'\r'，则说明读取到一个完整的行，把'\r'和'\n'换成'\0' */
            if((m_checked_idx>1)&&m_read_buf[m_checked_idx]=='\r'){
                m_read_buf[m_check_idx-1]='\0';
                m_read_buf[m_check_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;//'\n'前没有'\r'，错误的行
        }
    }
    return LINE_OPEN;//没读到'\r'和'\n'，不完整的行，表示还需要读取客户端数据才能进一步分析
}


http_conn::HTTP_CODE http_conn::parse_request_line(char *text){
    /* 在源字符串中找出最先含有搜索字符串中任意一个字符的位置
     *若没有，返回空指针
     * */
    m_url=strpbrk(text," \t");
    if(!m_url){
        return BAD_REQUEST;
    }
    *m_url++='\0';

    char *method=text;
    if(strcasecmp(method,"GET")==0){//比较源字符串和目标字符串
        m_method=GET;
    }
    else{
        return BAD_REQUEST;
    }
    /* 返回源字符串中第一个不在搜索字符串中出现的元素的下标 */
    m_url+=strspn(m_url," \t");
    m_version=strpbrk(m_url," \t");
    if(!m_version){
        return BAD_REQUEST;
    }
    *m_version++='0';
    m_version+=strspn(m_version," \t");
    if(strcasecmp(m_version,"HTTP/1.1")!=0){
        return BAD_REQUEST;
    }
    if(strncasecmp(m_url,"http://",7)==0){//比较源字符串的前n个与目标字符串
        m_url+=7;
        m_url=strchr(m_url,'/')；//查找字符串首次出现字符的位置,不存在返回空指针
    }
    if(!m_url||m_url[0]!='/'){
        return BAD_REQUEST;
    }
    m_check_state=CHECK_STATE_HEADER;//解析完请求栏，状态变为解析头部
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char *text){
    /* 遇到空行，首部块解析完毕 */
    if(text[0]=='\0'){
        if(m_content_length!=0){//主体内容不为空
            m_check_state=CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        /* 如果为空，解析了完整的HTTP请求 */
        return GET_REQUEST;
    }
    else if(strncasecmp(text,"Connection:",11)==0){
        text+=11;
        text+=strspn(text," \t");
        if(strcasecmp(text,"keep-alive")==0){
            m_linger=true;
        }
    }
    else if(strncasecmp(text,"Content-length:",15)==0){
        text+=15;
        text+=strspn(text," \t");
        m_content_length=atoi(text);
    }
    else if(strncasecmp(text,"Host:",5)==0){
        text+=5;
        text+=strspn(text," \t");
        m_host=text;
    }
    else{
        printf("oop! unknow header %s\n",text);
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char *text){
    if(m_read_idx>=(m_content_length+m_checked_idx)){
        text[m_content_length]='\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request(){
    strcpy(m_real_file,doc_root);
    int len=strlen(doc_root);
    strncpy(m_real_file+len,m_url,FILENAME_LEN-len-1);
    if(stat(m_real_file,&m_file_stat)<0){
        return NO_RESOURCE;
    }
    if(!(m_file_stat.st_mode&S_IROTH)){//文件允许其他用户读
        return FORBIDDEN_REQUEST;
    }
    if(S_ISDIR(m_file_stat.st_mode)){//目标是目录不是文件
        return BAD_REQUEST;
    }

    int fd=open(m_real_file,O_RDONLY);
    m_file_address=(char *)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    close(fd);
    return FILE_REQUEST;
}

bool http_conn::write(){
    int temp=0
}

void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address=0;
    }
}

