#ifndef TIMER
#define TIMER

#include <netinet/in.h>
#include <time.h>
//连接资源结构体成员需要用到定时器类，故提前声明
class util_timer;
//连接资源
struct client_data{
    //客户端socket地址
    sockaddr_in address;
    //文件描述符
    int sockfd;
    //定时器
    util_timer *timer;
};
class util_timer{
public:
    util_timer():prev(NULL),next(NULL){}
public:
    //超时时间
    time_t expire;
    //回调函数
    void (*cb_func)(client_data*);
    //连接资源
    client_data* user_data;
    //前向定时器
    util_timer* prev;
    //后继定时器
    util_timer* next;
};

class sort_timer_lst{
public:
    sort_timer_lst():head(NULL),tail(NULL){}
    ~sort_timer_lst(){
        util_timer *tmp = head;
        while(tmp){
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }
    //添加定时器，内部调用私有成员add_timer
    void add_timer(util_timer* timer){
        if(!timer){
            return;
        }
        if( !head ){
            head = tail = timer;
            return;
        }
        if(timer->expire < head->expire){
            timer -> next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        add_timer(timer,head);
    }
    //当任务发生变化时，调整定时器
    void adjust_timer(util_timer* timer){
        if(!timer){
            return;
        }
        util_timer* tmp = timer->next;
        if(!tmp || (timer->expire <= tmp->expire))
            return;
        if(timer == head){
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer,head);
        }
        else{
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }
    //del
    void del_tilmer(util_timer* timer){
        if(!timer)
            return;
        //连表中只有一个定时器，需要删除该定时器
        if((timer == head) && (timer == tail)){
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        //被删除的是头节点
        if(timer == head){
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }
        //被删除的定时器为尾节点
        if(timer == tail){
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        //在内部
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }
    //定时器处理函数
    void tick(){
        if(!head){
            return;
        }
        //获取当前时间
        time_t cur = time(NULL);
        util_timer* tmp = head;

        //遍历
        while(tmp){
            //链表容器为升序排列
            //当前时间小于定时器的超时时间，后面的也没有到时
            if( cur < tmp->expire){
                break;
            }
            //执行定时事件
            tmp->cb_func(tmp->user_data);
            //将处理后的定时器从链表容器中删除，并重置头节点
            head = tmp->next;
            if(head){
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }
private:
    void add_timer(util_timer* timer,util_timer* lst_head){
        util_timer* prev = lst_head;
        util_timer* tmp = prev->next;
        while(tmp){
            if(timer->expire < tmp->expire){
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }
        if(!tmp){
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }
private:
    util_timer* head;
    util_timer* tail;
};
#endif