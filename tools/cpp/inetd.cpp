/*
 * 程序名：inetd.cpp，正向网络代理服务程序。
 * 作者：吴从周
*/
#include "_public.h"
using namespace idc;

// 代理路由参数的结构体。
struct st_route
{
    int    srcport;           // 源端口。
    char dstip[31];        // 目标主机的地址。
    int    dstport;          // 目标主机的端口。
    int    listensock;      // 源端口监听的socket。
}stroute;
vector<struct st_route> vroute;       // 代理路由的容器。
bool loadroute(const char *inifile);  // 把代理路由参数加载到vroute容器。

// 初始化服务端的监听端口。
int initserver(const int port);

int epollfd=0;      // epoll的句柄。
int tfd=0;             // 定时器的句柄。

#define MAXSOCK  1024          // 最大连接数。
int clientsocks[MAXSOCK];       // 存放每个socket连接对端的socket的值。
int clientatime[MAXSOCK];       // 存放每个socket连接最后一次收发报文的时间。

// 向目标地址和端口发起socket连接。
int conntodst(const char *ip,const int port);

void EXIT(int sig);     // 进程退出函数。

clogfile logfile;

cpactive pactive;       // 进程心跳。

int main(int argc,char *argv[])
{
    if (argc != 3)
    {
        printf("\n");
        printf("Using :./inetd logfile inifile\n\n");
        printf("Sample:./inetd /tmp/inetd.log /etc/inetd.conf\n\n");
        printf("        /project/tools/bin/procctl 5 /project/tools/bin/inetd /tmp/inetd.log /etc/inetd.conf\n\n");
        printf("本程序的功能是正向代理，如果用到了1024以下的端口，则必须由root用户启动。\n");
        printf("logfile 本程序运行的日是志文件。\n");
        printf("inifile 路由参数配置文件。\n");

        return -1;
    }

    // 关闭全部的信号和输入输出。
    // 设置信号,在shell状态下可用 "kill + 进程号" 正常终止些进程。
    // 但请不要用 "kill -9 +进程号" 强行终止。
    closeioandsignal(true);  signal(SIGINT,EXIT); signal(SIGTERM,EXIT);

    // 打开日志文件。
    if (logfile.open(argv[1])==false)
    {
        printf("打开日志文件失败（%s）。\n",argv[1]); return -1;
    }

    pactive.addpinfo(30,"inetd");       // 设置进程的心跳超时间为30秒。

    // 把代理路由参数配置文件加载到vroute容器。
    if (loadroute(argv[2])==false) return -1;

    logfile.write("加载代理路由参数成功(%d)。\n",vroute.size());

    // 初始化服务端用于监听的socket。
    for (auto &aa:vroute)
    {
        if ( (aa.listensock=initserver(aa.srcport)) < 0 )
        {
            // 如果某一个socket初始化失败，忽略它。
            logfile.write("initserver(%d) failed.\n",aa.srcport);   continue;
        }

        // 把监听socket设置成非阻塞。
        fcntl(aa.listensock,F_SETFL,fcntl(aa.listensock,F_GETFD,0)|O_NONBLOCK);
    }

    // 创建epoll句柄。
    epollfd=epoll_create1(0);

    struct epoll_event ev;  // 声明事件的数据结构。

    // 为监听的socket准备读事件。
    for (auto aa:vroute)
    {
        if (aa.listensock<0) continue;

        ev.events=EPOLLIN;                     // 读事件。
        ev.data.fd=aa.listensock;              // 指定事件的自定义数据，会随着epoll_wait()返回的事件一并返回。
        epoll_ctl(epollfd,EPOLL_CTL_ADD,aa.listensock,&ev);  // 把监听的socket的事件加入epollfd中。
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // 把定时器加入epoll。
    tfd=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC);   // 创建timerfd。
    struct itimerspec timeout;                                // 定时时间的数据结构。
    memset(&timeout,0,sizeof(struct itimerspec));
    timeout.it_value.tv_sec = 10;                            // 定时时间为10秒。
    timeout.it_value.tv_nsec = 0;
    timerfd_settime(tfd,0,&timeout,0);                  // 开始计时。
    ev.data.fd=tfd;                                                  // 为定时器准备事件。
    ev.events=EPOLLIN;
    epoll_ctl(epollfd,EPOLL_CTL_ADD,tfd,&ev);     // 把定时器fd加入epoll。
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // 把信号加入epoll。
    sigset_t sigset;                                                     // 创建信号集。
    sigemptyset(&sigset);                                         // 初始化（清空）信号集。
    sigaddset(&sigset, SIGINT);                                // 把SIGINT信号加入信号集。
    sigaddset(&sigset, SIGTERM);                             // 把SIGTERM信号加入信号集。
    sigprocmask(SIG_BLOCK, &sigset, 0);                 // 对当前进程屏蔽信号集（当前程将收不到信号集中的信号）。
    int sigfd=signalfd(-1, &sigset, 0);                       // 创建信号的fd。
    ev.data.fd = sigfd;                                               // 为信号fd准备事件。
    ev.events = EPOLLIN;
    epoll_ctl(epollfd,EPOLL_CTL_ADD,sigfd,&ev);     // 把定时器fd加入epoll。
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////

    struct epoll_event evs[10];      // 存放epoll返回的事件。

    while (true)     // 进入事件循环。
    {
        // 等待监视的socket有事件发生。
        int infds=epoll_wait(epollfd,evs,10,-1);

        // 返回失败。
        if (infds < 0) { logfile.write("epoll() failed。\n"); EXIT(-1); }

        // 遍历epoll返回的已发生事件的数组evs。
        for (int ii=0;ii<infds;ii++)
        {
            logfile.write("已发生事件的socket=%d\n",evs[ii].data.fd);

            ////////////////////////////////////////////////////////
            // 如果定时器的时间已到，有两件事要做：1）更新进程的心跳；2）清理空闲的客户端socket。
            if (evs[ii].data.fd==tfd)
            {
                logfile.write("定时器时间已到。\n");

                timerfd_settime(tfd,0,&timeout,0);       // 重新开始计时。

                pactive.uptatime();        // 1）更新进程心跳。

                // 2）清理空闲的客户端socket。
                for (int jj=0;jj<MAXSOCK;jj++)         // 可以把最大的socket记下来，这个循环不必遍历整个数组。
                {
                    // 如果客户端socket空闲的时间超过80秒就关掉它。
                    if ( (clientsocks[jj]>0) && ((time(0)-clientatime[jj])>80) )
                    {
                        logfile.write("client(%d,%d) timeout。\n",clientsocks[jj],clientsocks[clientsocks[jj]]);
                        close(clientsocks[clientsocks[jj]]);
                        close(clientsocks[jj]);  
                        // 把数组中对端的socket置空，这一行代码和下一行代码的顺序不能乱。
                        clientsocks[clientsocks[jj]]=0;
                        // 把数组中本端的socket置空，这一行代码和上一行代码的顺序不能乱。
                        clientsocks[jj]=0;
                    }
                }

                continue;
            }
            ////////////////////////////////////////////////////////
            
            ////////////////////////////////////////////////////////
            // 如果收到了信号。
            if (evs[ii].data.fd==sigfd)
            {
                struct signalfd_siginfo siginfo;                                                  // 信号的数据结构。
                int s = read(sigfd, &siginfo, sizeof(struct signalfd_siginfo));    // 读取信号。
                logfile.write("收到了信号=%d。\n",siginfo.ssi_signo); 

                // 在这里编写处理信号的代码。

                continue;
            }
            ////////////////////////////////////////////////////////
      
            ////////////////////////////////////////////////////////
            // 如果发生事件的是listensock，表示有新的客户端连上来。
            int jj=0;
            for (jj=0;jj<vroute.size();jj++)
            {
                if (evs[ii].data.fd==vroute[jj].listensock)     // 判断是哪个源端口有客户端连上来了。5058
                {
                    // 从已连接队列中获取客户端连上来的socket。 // socket是7
                    struct sockaddr_in client;
                    socklen_t len = sizeof(client);
                    int srcsock = accept(vroute[jj].listensock,(struct sockaddr*)&client,&len);
                    if (srcsock<0) break;
                    if (srcsock>=MAXSOCK) 
                    {
                        logfile.write("连接数已超过最大值%d。\n",MAXSOCK); close(srcsock); break;
                    }

                    // 向目标地址和端口发起连接，如果连接失败，会被epoll发现，将关闭通道。
                    int dstsock=conntodst(vroute[jj].dstip,vroute[jj].dstport);        // socket是8
                    if (dstsock<0) { close(srcsock); break; }
                    if (dstsock>=MAXSOCK)
                    {
                        logfile.write("连接数已超过最大值%d。\n",MAXSOCK); close(srcsock); close(dstsock); break;
                    }

                    logfile.write("accept on port %d,client(%d,%d) ok。\n",vroute[jj].srcport,srcsock,dstsock);

                    // 为新连接的两个socket准备读事件，并添加到epoll中。
                    ev.data.fd=srcsock; ev.events=EPOLLIN;
                    epoll_ctl(epollfd,EPOLL_CTL_ADD,srcsock,&ev);
                    ev.data.fd=dstsock; ev.events=EPOLLIN;
                    epoll_ctl(epollfd,EPOLL_CTL_ADD,dstsock,&ev);

                    // 更新clientsocks数组中两端soccket的值和活动时间。
                    clientsocks[srcsock]=dstsock;  clientatime[srcsock]=time(0); 
                    clientsocks[dstsock]=srcsock;  clientatime[dstsock]=time(0);

                    break;
                }
            }

            // 如果jj<vroute.size()，表示事件在上面的for循环中已被处理，流程不必往下。
            if (jj<vroute.size()) continue;
            ////////////////////////////////////////////////////////

            ////////////////////////////////////////////////////////
            // 如果是客户端连接的socke有事件，表示有报文发过来或者连接已断开。
      
            char buffer[5000];     // 存放从接收缓冲区中读取的数据。
            int    buflen=0;          // 从接收缓冲区中读取的数据的大小。

            // 从通道的一端读取数据。
            if ( (buflen=recv(evs[ii].data.fd,buffer,sizeof(buffer),0)) <= 0 )
            {
                // 如果连接已断开，需要关闭通道两端的socket。
                logfile.write("client(%d,%d) disconnected。\n",evs[ii].data.fd,clientsocks[evs[ii].data.fd]);
                close(evs[ii].data.fd);                                         // 关闭客户端的连接。
                close(clientsocks[evs[ii].data.fd]);                     // 关闭客户端对端的连接。
                clientsocks[clientsocks[evs[ii].data.fd]]=0;       // 把数组中对端的socket置空，这一行代码和下一行代码的顺序不能乱。
                clientsocks[evs[ii].data.fd]=0;                           // 把数组中本端的socket置空，这一行代码和上一行代码的顺序不能乱。

                continue;
            }
      
            // 成功的读取到了数据，把接收到的报文内容原封不动的发给通道的对端。
            logfile.write("from %d to %d,%d bytes。\n",evs[ii].data.fd,clientsocks[evs[ii].data.fd],buflen);
            send(clientsocks[evs[ii].data.fd],buffer,buflen,0);

            // 更新通道两端socket的活动时间。
            clientatime[evs[ii].data.fd]=time(0); 
            clientatime[clientsocks[evs[ii].data.fd]]=time(0);  
        }
    }

    return 0;
}

// 初始化服务端的监听端口。
int initserver(const int port)
{
    int sock = socket(AF_INET,SOCK_STREAM,0);
    if (sock < 0)
    {
        logfile.write("socket(%d) failed.\n",port); return -1;
    }

    int opt = 1; unsigned int len = sizeof(opt);
    setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&opt,len);

    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if (bind(sock,(struct sockaddr *)&servaddr,sizeof(servaddr)) < 0 )
    {
        logfile.write("bind(%d) failed.\n",port); close(sock); return -1;
    }

    if (listen(sock,5) != 0 )
    {
        logfile.write("listen(%d) failed.\n",port); close(sock); return -1;
    }

    return sock;
}

// 把代理路由参数加载到vroute容器。
bool loadroute(const char *inifile)
{
    cifile ifile;

    if (ifile.open(inifile)==false)
    {
        logfile.write("打开代理路由参数文件(%s)失败。\n",inifile); return false;
    }

    string strbuffer;
    ccmdstr cmdstr;

    while (true)
    {
        if (ifile.readline(strbuffer)==false) break;

        // 删除说明文字，#后面的部分。
        auto pos=strbuffer.find("#");
        if (pos==string::npos) break;
        strbuffer.resize(pos);

        replacestr(strbuffer,"  "," ",true);    // 把两个空格替换成一个空格，注意第四个参数。
        deletelrchr(strbuffer,' ');                 // 删除两边的空格。

        // 拆分参数。
        cmdstr.splittocmd(strbuffer," ");
        if (cmdstr.size()!=3) continue;

        memset(&stroute,0,sizeof(struct st_route));
        cmdstr.getvalue(0,stroute.srcport);          // 源端口。
        cmdstr.getvalue(1,stroute.dstip);             // 目标地址。
        cmdstr.getvalue(2,stroute.dstport);         // 目标端口。

        vroute.push_back(stroute);
    }

    return true;
}

// 向目标地址和端口发起socket连接。
int conntodst(const char *ip,const int port)
{
    // 第1步：创建客户端的socket。
    int sockfd;
    if ( (sockfd = socket(AF_INET,SOCK_STREAM,0))==-1) return -1; 

    // 第2步：向服务器发起连接请求。
    struct hostent* h;
    if ( (h = gethostbyname(ip)) == 0 ) { close(sockfd); return -1; }
  
    struct sockaddr_in servaddr;
    memset(&servaddr,0,sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port); // 指定服务端的通讯端口。
    memcpy(&servaddr.sin_addr,h->h_addr,h->h_length);

    // 把socket设置为非阻塞。
    fcntl(sockfd,F_SETFL,fcntl(sockfd,F_GETFD,0)|O_NONBLOCK);

    if (connect(sockfd, (struct sockaddr *)&servaddr,sizeof(servaddr))<0)
    {
        if (errno!=EINPROGRESS)
        {
            logfile.write("connect(%s,%d) %s failed.\n",ip,port); return -1;
        }
    }

    return sockfd;
}

void EXIT(int sig)
{
    logfile.write("程序退出，sig=%d。\n\n",sig);

    // 关闭全部监听的socket。
    for (auto &aa:vroute)
        if (aa.listensock>0) close(aa.listensock);

    // 关闭全部客户端的socket。
    for (auto aa:clientsocks)
        if (aa>0) close(aa);

    close(epollfd);   // 关闭epoll。

    close(tfd);       // 关闭定时器。

    exit(0);
}