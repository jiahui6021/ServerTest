#include "http_conn.h"
#include <map>
#include <fstream>
#include <iostream>

using namespace std;

//#define ET//边缘触发非阻塞
#define LT//水平触发阻塞

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;

//网站路径
const char *doc_root = "./home/wwwroot/server";

///////////////////////////////////////spoll//////////////////////////////////////////

//设置文件描述符非阻塞
int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//内核事件表注册新事件
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
#ifdef ET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef LT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif
    if (one_shot) event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核事件表删除描述符
void removefd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd,int fd,int ev){
    epoll_event event;
    event.data.fd=fd;
#ifdef ET
    event.events = ev|EPOLLET|EPOLLONESHOT|EPOLLREHUP;
#endif
#ifdef LT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

void http_conn::close_conn(bool real_close){
    if(real_close && (m_sockfd != -1)){
        removefd(m_epollfd,m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接，外部调用初始化套接字地址
void http_conn::init(int sockfd,const sockaddr_in &addr){
    m_sockfd=sockfd;
    m_address=addr;
    addfd(m_epollfd,sockfd,true);
    m_user_count++;
    init();
}



//初始化新接受的连接
void http_conn::init(){
    //mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method= GET;
    m_url = 0;
    m_version = 0;
    m_content_length =0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf,'\0',WRITE_BUFFER_SIZE);
    memset(m_real_file,'\0',FILENAME_LEN);
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once(){
    if(m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }
    int bytes_read = 0;
    while(true){
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if(bytes_read == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                break;
            }
            return false;
        }
        else if(bytes_read == 0){
            return false;
        }
        m_read_idx += bytes_read;
    }
    cout<<"m_read_buf:"<<endl<<m_read_buf<<endl;
    return true;
}

//线程通过process函数进行处理
void http_conn::process(){
    cout<<"process"<<endl;
    HTTP_CODE read_ret = process_read();
    //NO_REQUEST 请求不完整，需要继续接收请求数据
    if(read_ret == NO_REQUEST){
        //注册并继续监听读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        cout<<"no_request"<<endl;
        return;
    }
    cout<<read_ret<<" "<<"read end"<<endl;
    //完成报文相应
    bool write_ret = process_write(read_ret);
    if(!write_ret){
        close_conn();
    }
    cout<<m_write_idx<<" "<< m_write_buf<<endl;
    cout<<"write end"<<endl;
    //注册并监听写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
///////////////////////////////read/////////////////////////////
http_conn::HTTP_CODE http_conn::process_read(){
    //初始化从状态机状态，http解析请求结果
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text =0;

    //parse_line为从状态机具体实现
    while((m_check_state == CHECK_STATE_CONTENT && line_status ==LINE_OK)
    ||((line_status = parse_line()) == LINE_OK)){
        cout<<line_status<<" "<<m_check_state<<" "<<ret<<endl;
        text = get_line();
        //cout<<"text"<<endl;
        //m_start_line是每一个数据行在m_read_buffer中的开始位置
        //m_checked_inedx表示从状态机在m_read_buffer中读取的位置
        m_start_line = m_checked_idx;
        //need log
        //三种状态转移
        switch(m_check_state){
    
            case CHECK_STATE_REQUESTLINE:{
                //解析请求行
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER:{
                //解析请求头
                ret = parse_headers(text);
                if(ret == BAD_REQUEST)
                    return BAD_REQUEST;
                //完整解析get后转到报文相应函数
                else if(ret == GET_REQUEST){
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:{
                //解析消息体
                ret = parse_content(text);
                //完整解析后跳转到报文相应函数
                if(ret == GET_REQUEST)
                    return do_request();
                //完成报文解析，更新line_status状态
                line_status = LINE_OPEN;
                break;
            }
            default:
                return INTERNAL_ERROR;
        }
    }
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态（LINE_OK, LINE_BAD, LINE_OPEN)

http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    //m_read_idx指向缓冲区m_read_buf的数据末尾的下一个字节
    //m_checked_idx指向从状态机当前正在分析的字节
    for(; m_checked_idx < m_read_idx; ++m_checked_idx){
        //当前分析的字节
        temp = m_read_buf[m_checked_idx];
        //如果读取到了'\r'则可能会读取到完整行
        if(temp == '\r'){
            //下一个字符达到了buffer结尾，则接受不完整，需要继续接收
            if((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            //下一个字符是\n，将\r\n改为\0\0
            else if(m_read_buf[m_checked_idx + 1] == '\n'){
                m_read_buf[m_checked_idx++]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            //如果都不符合,返回语法错误
            return LINE_BAD;
        }

        //如果当前字符是\n，也有可能读取到完整行
        //可能是上一次读到\r就到buffer结尾了，没有接收完整
        else if(temp == '\n'){
            //前一个字符是\r，则接受完整
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r'){
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }    
    }
    //继续接收
    return LINE_OPEN;
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text){
    //在http报文中，请求行中请求类型，要访问的资源，http版本，其中各个部分之间通过\t或空格分隔
    //strpbrk 返回两个字符串中首个相同字符的位置
    m_url = strpbrk(text, " \t");

    //如果没有空格或\t，则报文格式有误
    if(!m_url){
        return BAD_REQUEST;
    }
    //将该位置改为\0，用于将前面数据输出
    *m_url++='\0';

    //取出数据，通过与GET或POST比较，以确定请求方式
    char *method = text;
    if(strcasecmp(method, "GET")==0)
        m_method = GET;
    else if(strcasecmp(method, "POST")==0){
        m_method = POST;
        cgi = 1;
    }
    else return BAD_REQUEST;

    //m_url向后偏移，继续跳过空格和\t，指向请求资源的第一个字符
    m_url+= strspn(m_url," \t");
    //判断HTTP版本号
    m_version = strpbrk(m_url," \t");
    if(!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    //对前7个字符进行判断（资源中带有http://）
    if(strncasecmp(m_url, "http://", 7)==0){
        m_url+=7;
        m_url=strchr(m_url,'/');
    }
    //https同理
    if (strncasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    if(!m_url||m_url[0]!='/')
        return BAD_REQUEST;
    if(strlen(m_url)==1){
        strcat(m_url,"judge.html");
    }
    //处理完毕，将著状态机转移处理请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求中的请求头
http_conn::HTTP_CODE http_conn::parse_headers(char *text){
    //判断是空行还是请求头
    if(text[0] == '\0'){
        //判断是 GET 还是 POST 请求
        if(m_content_length!=0){
            //POST,跳转消息体处理状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    //解析请求头部连接字段
    else if(strncasecmp(text,"Connection:",11) == 0){
        text += 11;
        //跳过空格和\t字符
        text+=strspn(text," \t");
        if(strcasecmp(text,"keep-alive")==0){
            //如果是长连接，则将linger标志设置为true
            m_linger = true;
        }
    }
    //解析请求头部内容长度字段
    else if(strncasecmp(text,"Content-length:",15) == 0){
        text+=15;
        text+=strspn(text," \t");
        m_content_length = atol(text);
    }
    //解析请求头部HOST字段
    else if(strncasecmp(text,"Host:",5) == 0){
        text+= 5;
        text+= strspn(text," \t");
        m_host = text;
    }
    else {
        //异常
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text){
    if(m_read_idx >= (m_content_length + m_checked_idx)){
        text[m_content_length] = '\0';
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}
///////////////////////////// write  //////////////////////////

http_conn::HTTP_CODE http_conn::do_request(){
    strcpy(m_real_file,doc_root);
    int len = strlen(doc_root);

    //找到m_url中/的位置
    const char *p =strrchr(m_url,'/');
    //cout<<"m_url: "<<m_url<<" "<<p<<endl;
    //登录和注册校验
    if(cgi==1 && (*(p+1) == '2' || *(p+1) == '3')){
        //
    }
    //如果请求资源为/0，表示跳转到注册页面
    if(*(p+1)=='0'){
        char *m_url_real = (char *)malloc(sizeof(char)*200);
        strcpy(m_url_real,"/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    //如果请求资源为/1，表示跳转到登录页面
    else if(*(p+1)=='1'){
        char *m_url_real = (char *)malloc(sizeof(char)*200);
        strcpy(m_url_real,"/login.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len -1);

    cout<<"http_conn real_file: "<<m_real_file<<endl;

    //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    if(stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    
    //判断文件权限是否可读
    if(!(m_file_stat.st_mode&S_IROTH))
        return FORBIDDEN_REQUEST;
    //判断文件类型
    if(S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;
    
    //以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file,O_RDONLY);
    m_file_address = (char*)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

bool http_conn::add_response(const char *format,...){
    //写入内容超出m_write_buf大小
    if(m_write_idx >= WRITE_BUFFER_SIZE){
        return false;
    }
    //定义可变参数列表
    va_list arg_list;
    //将变量arg_list初始化为传入参数
    va_start(arg_list, format);
    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx,
     WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    //写入数据长度超过缓冲区剩余空间
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)){
        va_end(arg_list);
        return false;
    }

    //更新m_write_idx位置
    m_write_idx+=len;
    va_end(arg_list);
    return true;
}

//添加状态行
bool http_conn::add_status_line(int status, const char *title){
    return add_response("%s %d %s\r\n","HTTP/1.1",status,title);
}
//添加消息报头（具体的文本长度，连接状态和空行）
bool http_conn::add_headers(int content_len){
    add_content_length(content_len);
    add_linger();
    add_blank_line();
    return true;
}
//添加content_length，表示响应报文的长度
bool http_conn::add_content_length(int content_len){
    return add_response("Content-Length:%d\r\n",content_len);
}
//添加文本类型
bool http_conn::add_content_type(){
    return add_response("Content-Type:%s\r\n","text/html");
}
//添加连接状态
bool http_conn::add_linger(){
    return add_response("Connection:%s\r\n",(m_linger == true)?"keep-alive":"close");
}
//添加空行
bool http_conn::add_blank_line(){
    return add_response("%s", "\r\n");
}
//添加文本content
bool http_conn::add_content(const char* content){
    return add_response("%s",content);
}

bool http_conn::process_write(HTTP_CODE ret){
    switch(ret){
        //500
        case INTERNAL_ERROR:{
            add_status_line(500,error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form))
                return false;
            break;
        }
        //404
        case BAD_REQUEST:{
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form))
                return false;
            break;
        }
        //403
        case FORBIDDEN_REQUEST:{
            add_status_line(403,error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form));
                return false;
            break;
        }
        //200
        case FILE_REQUEST:{
            add_status_line(200,ok_200_title);
            //如果请求的资源存在
            if(m_file_stat.st_size!=0){
                add_headers(m_file_stat.st_size);
                //第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                //第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                //发送的全部数据为响应报文头部信息和文件大小
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else{
                //如果请求的资源大小为0，则返回空白html文件
                const char* ok_string="<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string))
                    return false;
            }
        }
        default:
            return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

bool http_conn::write(){
    int temp = 0;
    int newadd = 0;
    while(1){
        //将状态行，消息头，空行和相应正文发送给浏览器
        temp = writev(m_sockfd, m_iv, m_iv_count);

        //正常发送，temp为发送的字节数
        if (temp > 0){
            //更新已发送字节
            bytes_have_send += temp;
            newadd = bytes_have_send - m_write_idx;
        }
        if(temp <= -1){
            //判断缓冲区是否满
            if(errno == EAGAIN){
                if(bytes_have_send >= m_iv[0].iov_len){
                    //不继续发送头部信息
                    m_iv[0].iov_len = 0;
                    m_iv[1].iov_base = m_file_address + newadd;
                    m_iv[1].iov_len = bytes_to_send;
                }
                else{
                    m_iv[0].iov_base = m_write_buf + bytes_to_send;
                    m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
                }
                //重新注册写事件
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        //更新已发送字节数
        bytes_to_send -= temp;

        //判断条件，数据已全部发送完
        if(bytes_to_send <= 0){
            unmap();

            //在epoll树上重置EPOLLONESHOT事件
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            if(m_linger){
                init();
                return true;
            }
            else return false;
        }
    }
}