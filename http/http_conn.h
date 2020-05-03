#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <singnal.h>
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
private:
    int m_sockfd;
    sockaddr_in m_address;
public:
    static int m_epollfd;
public:
    http_conn(){}
    ~http_conn(){}
    //初始化套接字地址
    void init(int sockfd, const sockaddr_in &addr);
    //读取浏览器内容
    bool read_once();
};

http_conn::http_conn(/* args */)
{
}

http_conn::~http_conn()
{
}
