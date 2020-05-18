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
#include "./timer/timer.h"
using namespace std;

//触发方式
//#defin ET //边缘触发非阻塞
#define LT //水平触发阻塞

//defin
#define MAX_FD 65536//
#define MAX_EVENT_NUMBER 10000//最大事件数
#define TIMESLOT 5//最小超时单位
//定时器相关
static int epollfd = 0;
static int pipefd[2];
static sort_timer_lst timer_lst;

//引用http_conn中定义的函数
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd,int fd);
extern int setnonblocking(int fd);

//信号处理函数
void sig_handler(int sig){
    //为保持函数的可重入性，保留原有的errno??
    //可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1],(char *)&msg,1,0);
    //将原来的errno赋值为新的
    errno = save_errno;
}
//设置信号函数
void addsig(int sig, void(handler)(int), bool restart = true){
    //创建sigaction结构体变量
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    //信号处理函数中仅发送信号，不做逻辑处理
    sa.sa_handler = handler;
    if(restart){
        sa.sa_flags |= SA_RESTART;
    }
    //将所有信号添加到信号集中
    sigfillset(&sa.sa_mask);
    //执行sigaction函数
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data){
    //删除非活动连接在socket上的注册事件
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    //关闭文件描述符
    close(user_data->sockfd);
    http_conn::m_user_count--;
}

//定时处理任务，重新定时以不断触发sigalrm信号
void timer_handler(){
    timer_lst.tick();
    alarm(TIMESLOT);
}

//创建连接资源数组
client_data *users_timer = new client_data[MAX_FD];

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

    //创建管道套接字
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    //设置管道写端为非阻塞
    setnonblocking(pipefd[1]);
    //设置管道读端为ET非阻塞
    addfd(epollfd, pipefd[0], false);
    //传递给主循环的信号值
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);

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
    //循环条件
    bool stop_server = false;
    //超时标志
    bool timeout = false;
    //每隔TIMESLOT时间触发SIGALRM信号
    alarm(TIMESLOT);

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
                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                //初始化该连接对应的连接资源
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                //创建定时器临时变量
                util_timer *timer = new util_timer;
                //设置定时器对应的连接资源
                timer->user_data = &users_timer[connfd];
                //设置回调函数
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                //绝对超时时间
                timer->expire = cur+3*TIMESLOT;
                //创建该连接对应的定时器
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
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
                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur+3*TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
                continue;
                #endif

            }

            //处理异常事件
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                //服务器端关闭连接，移除对应的定时器
                cb_func(&users_timer[sockfd]);
                util_timer *timer = users_timer[sockfd].timer;
                if(timer){
                    timer_lst.del_tilmer(timer);
                }
                //服务器关闭连接
            }

            //处理信号
            else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)){
                int sig;
                char signals[1024];
                //从管道读端读出信号值，成功返回字节数，失败返回-1
                //从正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1){
                    //handle the error
                    continue;
                }
                else if(ret == 0){
                    continue;
                }
                else{
                    //处理信号值对应的逻辑
                    for(int i = 0; i < ret; i++){
                        //这里面是字符
                        switch(signals[i]){
                            case SIGALRM:{
                                timeout = true;
                                break;
                            }
                            case SIGTERM:{
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            //处理客户连接上收到的数据
            else if(events[i].events & EPOLLIN){
                cout<<"read a"<<endl;
                util_timer *timer = users_timer[sockfd].timer;
                //读入对应缓冲区
                if(users[sockfd].read_once()){
                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);
                    cout<<"pool->append"<<endl;
                    //若有数据传输，则将定时器后延3个单位
                    if(timer){
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                }
                else{
                    cout<<"server close"<<endl;
                    //服务器关闭连接,移除对应定时器
                    cb_func(&users_timer[sockfd]);
                    if(timer){
                        timer_lst.del_tilmer(timer);
                    }
                }
            }
            else if(events[i].events & EPOLLOUT){
                util_timer *timer = users_timer[sockfd].timer;
                cout<<"main_write"<<endl;
                if(users[sockfd].write()){
                    //若有数据传输，则将定时器延迟3个单位
                    if(timer){
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                }
                else{
                    //服务器关闭连接，移除定时器
                    cb_func(&users_timer[sockfd]);
                    if(timer){
                        timer_lst.del_tilmer(timer);
                    }
                }
            }
        }
        if(timeout){
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    delete[] users;
    return 0;
}
