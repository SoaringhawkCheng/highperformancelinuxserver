#ifndef http_conn_h
#define http_conn_h

#include <sys/socket.h>//提供socket函数及数据结构
#include <sys/stat.h>//stat结构体
#include <sys/uio.h>//readv()，writev()，iovec结构体
#include <netinet/in.h>//互联网地址族，定义数据结构sockaddr_in
#include <arpa/inet.h>//主要定义类格式转换函数，比如IP地址转换函数，linger结构体

class http_conn{
public:
    static const int FILENAME_LEN=200;//文件名的最大长度
    static const int READ_BUFFER_SIZE=2048;//读缓冲区的大小
    static const int WRITE_BUFFER_SIZE=1024;//写缓冲区的大小
    enum METHOD{
        GET=0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATCH
    };
    /* 主状态机：三种可能状态 */
    enum CHECK_STATE{
        CHECK_STATE_REQUESTLINE=0,//解析请求行
        CHECK_STATE_HEADER,//解析首部字段
        CHECK_STATE_CONTENT//解析主题字段
    };
    /* 从状态机：三种可能状态  */
    enum LINE_STATUS{
        LINE_OK=0,//完整的行
        LINE_BAD,//行出错
        LINE_OPEN//行数据不完整
    };
    /* 处理HTTP请求的结果 */
    enum HTTP_CODE{
        NO_REQUEST,//请求不完整，需要继续读取数据
        GET_REQUEST,//获得完整的客户请求
        BAD_REQUEST,//客户请求有语法错误
        NO_RESOURCE,//客户请求的资源不存在
        FORBIDDEN_REQUEST,//客户对资源没有访问权限
        FILE_REQUEST,//
        INTERNAL_ERROR,//服务器内部错误
        CLOSED_CONNECTION//客户端已经关闭连接
    };

public:
    http_conn(){}
    ~http_conn(){}

public:
    void init(int sockfd,const sockaddr_in &addr);//初始化新接受的连接
    void close_conn(bool real_close=true);//关闭连接
    bool read();//非阻塞读操作
    void process();//处理客户请求
    bool write();//非阻塞写操作

private:
    void init();//初始化连接
    HTTP_CODE process_read();//解析HTTP请求
    bool process_write(HTTP_CODE ret);//填充HTTP应答

    /* process_read()分析HTTP请求所调用的函数 */
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    /* 获取parse_line解析的当前行的内容 */
    char *get_line(){return m_read_buf+m_start_line;}
    LINE_STATUS parse_line();


        /* process_write()填充HTTP应答所调用的函数 */
    void unmap();
    bool add_response(const char *format,...);
    bool add_content(const char *content);
    bool add_status_line(int status,const char *title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    /* 所有socket上的事件被注册到同一个epoll内核事件表中，将epoll文件描述符设置为静态 */
    static int m_epollfd;
    static int m_user_count;

private:
    int m_sockfd;//连接的套接字
    sockaddr_in m_address;//连接对方的套接字地址

    char m_read_buf[READ_BUFFER_SIZE];//读缓冲区
    int m_read_idx;//缓冲中已读入客户数据的最后一个字节的下一个位置
    int m_checked_idx;//当前正在分析的字符在读缓冲区中的位置
    int m_start_line;//正在解析的行的起始位置
    char m_write_buf[WRITE_BUFFER_SIZE];//写缓冲区
    int m_write_idx;//

    CHECK_STATE m_check_state;//解析HTTP报文的状态
    METHOD m_method;

    char m_real_file[FILENAME_LEN];
    char *m_url;//客户请求的目标文件的文件名
    char *m_version;//HTTP协议的版本号
    char *m_host;//主机名
    int m_content_length;//HTTP请求的主体部分长度
    bool m_linger;//HTTP请求是否要求保持连接

    /* 客户请求的目标文件被mmap到内存中的起始位置 */
    char *m_file_address;
    /* 存放目标文件的状态的结构体 */
    struct stat m_file_stat;

    /* 与readv和writev操作相关的结构体，readv和writev函数用于在一个原子操作中读、写多个非连续缓冲区 */
    struct iovec m_iv[2];
    int m_iv_count;
};


#endif /* http_conn_h */
