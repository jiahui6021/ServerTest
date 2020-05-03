#include "http_conn.h"
#include <map>
#include <fstream>

//#define ET//边缘触发非阻塞
#define LT//水平触发阻塞

//初始化连接，外部调用初始化套接字地址
void http_conn::init(int sockfd,const sockaddr_in &addr){
    m_sockfd=sockfd;
    m_address=addr;
    addfd(m_epollfd,sockfd,true);
    m_user_count++;
    init();
}

//内核事件表注册新事件
void addfd(int epollfd,int fd,bool one_shot){
    epoll_event evevt;
    event.data.fd = fd;
#ifdef ET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef LT
    evevt.events = EPOLLIN | EPOLLRDHUP;
#endif
    if(one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//设置文件描述符非阻塞
int setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}
