#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

//#include "../CGImysql/sql_connection_pool.h"
#include "../lock/locker.h"
class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    enum CHECK_STATE {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum METHOD {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
private:
    int m_sockfd;
    sockaddr_in m_address;
    //存储读取的请求报文数据
    char m_read_buf[READ_BUFFER_SIZE];
    //缓冲区中m_read_buf中数据的最后一个字节的下一个位置
    int m_read_idx;
    ////m_read_buf读取的位置m_checked_idx
    int m_checked_idx;
    ////m_read_buf中已经解析的字符个数
    int m_start_line;

    ////存储发出的响应报文数据
    char m_write_buf[WRITE_BUFFER_SIZE];
    //指示buffer中的长度
    int m_write_idx;

    //主状态机的状态
    CHECK_STATE m_check_state;
    //请求方法
    METHOD m_method;

    //以下为解析请求报文中对应的6个变量
    //存储读取文件的名称
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    int m_content_length;
    bool m_linger;

    //读取服务器上的文件地址
    char *m_file_address;
    struct stat m_file_stat;
    // io向量机制iovec
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send;  //剩余发送字节数
    int bytes_have_send;  //已发送字节数
public:
    static int m_epollfd;
    static int m_user_count;
    //MYSQL *mysql;
    
    enum HTTP_CODE {
        //请求不完整，需要继续请求
        NO_REQUEST,
        //获得了完整的请求数据
        GET_REQUEST,
        //请求报文语法有误
        BAD_REQUEST,
        //没有资源
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        //服务器内部错误
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    enum LINE_STATUS {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

   private: 
    void init();
    //从m_read_buf读取，并处理请求报文
    HTTP_CODE process_read();
    //向m_write_buf写入响应报文数据
    bool process_write(HTTP_CODE ret);
    //主状态机解析报文中的请求行数据
    HTTP_CODE parse_request_line(char *text);
    //主状态机解析报文中的请求头数据
    HTTP_CODE parse_headers(char *text);
    //主状态机解析报文中的请求内容
    HTTP_CODE parse_content(char *text);
    //生成响应报文
    HTTP_CODE do_request();

    // m_start_line是已经解析的字符
    // get_line用于将指针向后偏移，指向未处理的字符
    char *get_line() { return m_read_buf + m_start_line; };

    //从状态机读取一行，分析是请求报文的哪一部分
    LINE_STATUS parse_line();

    void unmap();

    //根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    bool add_response(const char *format, ...);
    bool add_content(const char* content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

   public:
    http_conn(){}
    ~http_conn(){}
    //初始化套接字地址
    void init(int sockfd, const sockaddr_in &addr);
    //读取浏览器内容
    bool read_once();
    void process();
    void close_conn(bool real_close = true);
    bool write();
};
#endif
