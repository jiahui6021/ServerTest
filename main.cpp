#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <cassert>
#include <error.h>

#include "./http/http_conn.h"
#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
using namespace std;

//触发方式
//#defin ET //边缘触发非阻塞
#define LT //水平触发阻塞

//defin
#define MAX_FD 65536//
#define MAX_EVENT_NUMBER 10000//最大事件数

//定时器相关
static int epollfd = 0;

//引用http_conn中定义的函数
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd,int fd);
extern int setnonblocking(int fd);

int main(int argc, char const *argv[]) {
    cout<<"start"<<endl;
    //创建线程池
    threadpool<http_conn> *pool = NULL;
    try{
        //!缺少数据路连接池
        pool = new threadpool<http_conn>();
    }
    catch(...){
        return 1;
    }
    cout<<"threadpool create"<<endl;
    int port = atoi(argv[1]);
    //创建套接字，返回listenfd
    //int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    int listenfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listenfd >= 0);

    //struct linger tmp={1,0};
    int ret=0;
    struct sockaddr_in address;
    //将内存（字符串）前n个字节清零
    bzero(&address,sizeof(address));
    address.sin_family = AF_INET;
    //0.0.0.0 任意地址
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    //设置端口复用，绑定端口
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    //listen函数的第一个参数即为要监听的socket描述字，第二个参数为相应socket可以排队的最大连接个数。
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    //创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    //将listenfd方在epoll树上
    addfd(epollfd, listenfd, false);

    //将上述epollfd赋值给http类对象的mepollfd属性
    http_conn::m_epollfd = epollfd;

    cout<<"epollend"<<endl;

    //创建MAXFD个http类对象
    http_conn *users = new http_conn[MAX_FD];
    bool stop_server = false;
    while(!stop_server){
        //等待所监控文件描述符上有事件的产生
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if(number <= 0 && errno != EINTR){
            break;
        }
        //对所有就绪事件进行处理
        for(int i = 0; i < number; i++){
            int sockfd = events[i].data.fd;
            //处理新客户连接
            if(sockfd == listenfd){
                cout<<"new"<<endl;
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                
                #ifdef LT
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if(connfd<0)
                    continue;
                if(http_conn::m_user_count >= MAX_FD){
                    cout<<"show error"<<endl;
                    //show_error(connfd,"Internal server busy");
                    continue;
                }
                users[connfd].init(connfd, client_address);
                #endif

                #ifdef ET
                while(1){
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if(connfd < 0){
                        break;
                    }
                    if(http_conn::m_user_count >= MAX_FD){
                        show_error(connfd,"Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd,client_address);
                }
                continue;
                #endif
            }

            //处理异常事件
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                cout<<"error a"<<endl;
                //服务器关闭连接
            }

            //处理信号

            //处理客户连接上收到的数据
            else if(events[i].events & EPOLLIN){
                cout<<"read a"<<endl;
                //读入对应缓冲区
                if(users[sockfd].read_once()){
                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);
                    cout<<"pool->append"<<endl;
                }
                else{
                    //服务器关闭连接
                }
            }
        }
    }
    return 0;
}
