#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
using namespace std;

#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server: jdbhttpd/0.1.0\r\n"

vector<thread> g_thread_pool; // 线程池

// 处理从套接字上监听到的一个 HTTP 请求，在这里可以很大一部分地体现服务器处理请求流程
void accept_request(int);
// 返回给客户端这是个错误请求，HTTP 状态吗 400 BAD REQUEST.
void bad_request(int);
// 读取服务器上某个文件写到 socket 套接字
void cat(int, FILE *);
// 主要处理发生在执行 cgi 程序时出现的错误
void cannot_execute(int);
// 把错误信息写到 perror 并退出
void error_die(const char *);
// 运行 cgi 程序的处理，也是个主要函数
void execute_cgi(int, const char *, const char *, const char *);
// 读取套接字的一行，把回车换行等情况都统一为换行符结束
int get_line(int, char *, int);
// 把 HTTP 响应的头部写到套接字
void headers(int, const char *);
// 主要处理找不到请求的文件时的情况
void not_found(int);
// 调用 cat 把服务器文件返回给浏览器
void serve_file(int, const char *);
// 初始化 httpd 服务，包括建立套接字，绑定端口，进行监听等
int startup(u_short &);
// 返回给浏览器表明收到的 HTTP 请求所用的 method 不被支持
void unimplemented(int);

int main(void)
{
    int server_sock = -1;
    u_short port = 8080;
    int client_sock = -1;
    struct sockaddr_in client_name;
    socklen_t client_name_len = sizeof(client_name);

    server_sock = startup(port);
    printf("httpd running on port %d\n", port);

    while (1)
    {
        // 阻塞等待客户端的连接
        client_sock = ::accept(server_sock, (sockaddr *)&client_name, &client_name_len);
        if (client_sock == -1)
            error_die("accept");
        g_thread_pool.emplace_back(accept_request, client_sock);
    }

    for (auto &t : g_thread_pool)
        t.join();

    close(server_sock);

    return (0);
}

/**********************************************************************/
/* 处理客户端连接
 * 参数: 连接到客户端的套接字 */
/**********************************************************************/
void accept_request(int client)
{
    char buf[1024];
    int numchars;
    char method[255];
    char url[255];
    char path[512];
    size_t i, j;
    struct stat st;
    int cgi = 0; // 如果服务器认为这是一个CGI， 变为true程序
    char *query_string = nullptr;

    // 读取http 请求的第一行数据（请求头），把请求方法存进 method 中
    numchars = get_line(client, buf, sizeof(buf));
    i = 0;
    j = 0;
    while (!ISspace(buf[j]) && (i < sizeof(method) - 1))
    {
        method[i] = buf[j];
        i++;
        j++;
    }
    method[i] = '\0';

    // 如果请求的方法不是 GET 或 POST 任意一个的话就直接发送 response 告诉客户端没实现该方法
    if (strcasecmp(method, "GET") && strcasecmp(method, "POST"))
    {
        unimplemented(client);
        return;
    }

    // 如果是 POST 方法就将 cgi 标志变量置1(true)
    if (strcasecmp(method, "POST") == 0)
        cgi = 1;

    i = 0;
    // 跳过所有的空白字符(空格)
    while (ISspace(buf[j]) && (j < sizeof(buf)))
        j++;

    // 把 URL 读出来放到 url 数组中
    while (!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < sizeof(buf)))
    {
        url[i] = buf[j];
        i++;
        j++;
    }
    url[i] = '\0';

    // 如果这个请求是一个 GET 方法的话
    if (strcasecmp(method, "GET") == 0)
    {
        // 用一个指针指向 url
        query_string = url;

        // 去遍历这个 url，跳过字符 ？前面的所有字符，如果遍历完毕也没找到字符 ？则退出循环
        while (*query_string != '?' && *query_string != '\0')
            query_string++;

        // 退出循环后检查当前的字符是 ？还是字符串(url)的结尾
        if (*query_string == '?')
        {
            // 如果是 ？ 的话，证明这个请求需要调用 cgi，将 cgi 标志变量置1(true)
            cgi = 1;
            // 从字符 ？ 处把字符串 url 给分隔会两份
            *query_string = '\0';
            // 使指针指向字符 ？后面的那个字符
            query_string++;
        }
    }

    // 将前面分隔两份的前面那份字符串，拼接在字符串resource的后面之后就
    sprintf(path, "resource%s", url);

    // 如果 path 数组中的这个字符串的最后一个字符是以字符 / 结尾的话，就返回固定的index.html
    if (path[strlen(path) - 1] == '/')
        strcat(path, "index.html");

    // 在系统上去查询该文件是否存在
    if (stat(path, &st) == -1)
    {
        // 如果不存在，那把这次 http 的请求后续的内容(head 和 body)全部读完并忽略
        while ((numchars > 0) && strcmp("\n", buf))
            numchars = get_line(client, buf, sizeof(buf));
        // 然后返回一个找不到文件的 response 给客户端
        not_found(client);
    }
    else
    {
        // 文件存在，那去跟常量S_IFMT相与，相与之后的值可以用来判断该文件是什么类型的
        if ((st.st_mode & S_IFMT) == S_IFDIR)
            strcat(path, "/index.html"); // 不支持目录，返回首页

        // 判断是不是可执行文件，如果是将 cgi 标志变量置1
        if ((st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP) || (st.st_mode & S_IXOTH))
            cgi = 1;

        if (!cgi)
            serve_file(client, path);
        else
            execute_cgi(client, path, method, query_string);
    }

    close(client);
}

/**********************************************************************/
/* 通知客户端它所发出的请求有问题。
 * 参数: 客户端套接字 */
/**********************************************************************/
void bad_request(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "<P>Your browser sent a bad request, ");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "such as a POST without a Content-Length.\r\n");
    send(client, buf, sizeof(buf), 0);
}

/**********************************************************************/
/* 将文件的全部内容放到套接字上。
 * 这个函数是以UNIX的“cat”命令命名的
 * 参数:
 *    1.client: 客户端套接字描述符
 *    2.resource: 需要读取文件的句柄 */
/**********************************************************************/
void cat(int client, FILE *resource)
{
    char buf[1024];

    // 从文件文件描述符中读取指定内容
    fgets(buf, sizeof(buf), resource); // 此行用以防止空文件，保证能发送一个文件结束符
    while (!feof(resource))
    {
        send(client, buf, strlen(buf), 0);
        fgets(buf, sizeof(buf), resource);
    }
}

/**********************************************************************/
/* 通知客户端无法执行CGI脚本
 * 参数: 客户端scoket */
/**********************************************************************/
void cannot_execute(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
    send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* 错误处理函数，用perror打印错误消息
 * 参数: 自定义消息*/
/**********************************************************************/
void error_die(const char *sc)
{
    perror(sc);
    exit(1);
}

/**********************************************************************/
/* 它使用管道在父进程（Web 服务器）和子进程（CGI 脚本）之间进行通信
 * 根据 HTTP 方法，它为 CGI 脚本设置环境变量并执行它
 * --------------------------------------------------------------------
 * 参数:
 *    1.client: 客户端套接字
 *    2.path: CGI 脚本路径
 *    3.method: HTTP 方法
 *    4.query_string: 查询字符串 */
/**********************************************************************/
void execute_cgi(int client, const char *path, const char *method, const char *query_string)
{
    char buf[1024];
    int cgi_output[2];
    int cgi_input[2];
    pid_t pid;
    int status;
    int i;
    char c;
    int numchars = 1;
    int content_length = -1;

    // 往 buf 中填东西以保证能进入下面的 while
    buf[0] = 'A';
    buf[1] = '\0';
    // 如果是 http 请求是 GET 方法的话读取并忽略请求剩下的内容
    if (strcasecmp(method, "GET") == 0)
        while ((numchars > 0) && strcmp("\n", buf))
            numchars = get_line(client, buf, sizeof(buf));
    else /* POST */
    {
        // 只有 POST 方法才需要后续内容
        numchars = get_line(client, buf, sizeof(buf));
        // 这个循环的目的是读出指示 body 长度大小的参数，并记录 body 的长度大小。其余的 header 里面的参数一律忽略
        // 注意这里只读完 header 的内容，body 的内容没有读
        while ((numchars > 0) && strcmp("\n", buf))
        {
            buf[15] = '\0';
            if (strcasecmp(buf, "Content-Length:") == 0)
                content_length = atoi(&(buf[16])); // 记录 body 的长度大小
            numchars = get_line(client, buf, sizeof(buf));
        }

        // 如果 http 请求的 header 没有指示 body 长度大小的参数，则报错返回
        if (content_length == -1)
        {
            bad_request(client);
            return;
        }
    }

    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);

    // 下面这里创建两个管道，用于两个进程间通信
    if (pipe(cgi_output) < 0)
    {
        cannot_execute(client);
        return;
    }
    if (pipe(cgi_input) < 0)
    {
        cannot_execute(client);
        return;
    }

    // 创建一个子进程
    if ((pid = fork()) < 0)
    {
        cannot_execute(client);
        return;
    }

    // 子进程用来执行 cgi 脚本
    if (pid == 0)
    {
        char meth_env[255];
        char query_env[255];
        char length_env[255];

        // 将子进程的输出由标准输出重定向到 cgi_ouput 的管道写端上
        dup2(cgi_output[1], 1);
        // 将子进程的输出由标准输入重定向到 cgi_ouput 的管道读端上
        dup2(cgi_input[0], 0);
        // 关闭 cgi_ouput 管道的读端与cgi_input 管道的写端
        close(cgi_output[0]);
        close(cgi_input[1]);

        // 构造一个环境变量
        sprintf(meth_env, "REQUEST_METHOD=%s", method);
        // 将这个环境变量加进子进程的运行环境中
        putenv(meth_env);

        // 根据http 请求的不同方法，构造并存储不同的环境变量
        if (strcasecmp(method, "GET") == 0)
        {
            sprintf(query_env, "QUERY_STRING=%s", query_string);
            putenv(query_env);
        }
        else
        { /* POST */
            sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
            putenv(length_env);
        }

        // 最后将子进程替换成另一个进程并执行 cgi 脚本
        execl(path, path, NULL);
        exit(0);
    }
    else // 父进程接收脚本输出，返回给客户端
    {
        // 父进程则关闭了 cgi_output管道的写端和 cgi_input 管道的读端
        close(cgi_output[1]);
        close(cgi_input[0]);

        // 如果是 POST 方法的话就继续读 body 的内容，并写到 cgi_input 管道里让子进程去读
        if (strcasecmp(method, "POST") == 0)
            for (i = 0; i < content_length; i++)
            {
                recv(client, &c, 1, 0);
                write(cgi_input[1], &c, 1);
            }

        //然后从 cgi_output 管道中读子进程的输出，并发送到客户端去
        while (read(cgi_output[0], &c, 1) > 0)
            send(client, &c, 1, 0);

        //关闭管道
        close(cgi_output[0]);
        close(cgi_input[1]);
        //等待子进程的退出
        waitpid(pid, &status, 0);
    }
}

/**********************************************************************/
/* 从指定的套接字中读取一行，无论该行是否以换行符结束，
 * \n，或CRLF组合。终止读取的字符串
 * --------------------------------------------------------------------
 * 参数:
 *    1.sock: 指定的套接字
 *    2.buf: 存储数据的缓冲区
 *    3.size: 缓冲区的大小
 * 返回:
 *    存储的字节数(不包括null) */
/**********************************************************************/
int get_line(int sock, char *buf, int size)
{
    int i = 0;
    char c = '\0';
    int n;

    while ((i < size - 1) && (c != '\n'))
    {
        // 读一个字节的数据存放在 c 中
        n = recv(sock, &c, 1, 0);
        if (n > 0)
        {
            if (c == '\r')
            {
                n = recv(sock, &c, 1, MSG_PEEK); // 向后窥探一位，是否是结束符
                if ((n > 0) && (c == '\n'))
                    recv(sock, &c, 1, 0);
                else // 此行为兼容性，\r结束也追加一个\n，结束本行读取
                    c = '\n';
            }
            buf[i] = c;
            i++;
        }
        else
        {
            c = '\n';
        }
    }
    buf[i] = '\0';

    return i;
}

/**********************************************************************/
/* 组装返回文件的HTTP信息头并发至客户端scoket
 * 参数:
 *    1.client: 要打印报头的socket
 *    2.filename: 文件的名称 */
/**********************************************************************/
void headers(int client, const char *filename)
{
    char buf[1024];
    // 获取文件后缀名
    const char *filetype = strchr(filename, '.') + 1;

    strcpy(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/%s\r\n", filetype);
    send(client, buf, strlen(buf), 0);
    strcpy(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* 给客户端一个404 未找到状态消息
 * 参数: 客户端socket */
/**********************************************************************/
void not_found(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "your request because the resource specified\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "is unavailable or nonexistent.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* 将指定的文件作为 HTTP 响应发送给客户端
 * --------------------------------------------------------------------
 * 参数:
 *    1. client: 要发送文件的客户端
 *    2. filename: 请求的文件名 */
/**********************************************************************/
void serve_file(int client, const char *filename)
{
    FILE *resource = NULL;
    int numchars = 1;
    char buf[1024];

    // 确保 buf 里面有东西，能进入下面的 while 循环
    buf[0] = 'A';
    buf[1] = '\0';
    // 读取并忽略掉这个 http 请求后面的所有内容
    while (numchars > 0 && strcmp("\n", buf))
        numchars = get_line(client, buf, sizeof(buf));

    // 打开这个传进来的这个路径所指的文件
    resource = fopen(filename, "r");
    if (resource == NULL)
    {
        not_found(client);
    }
    else
    {
        // 打开成功后，将这个文件的基本信息封装成 response 的头部(header)并返回
        headers(client, filename);
        // 接着把这个文件的内容读出来作为 response 的 body 发送到客户端
        cat(client, resource);
    }

    fclose(resource);
}

/**********************************************************************/
/* 此函数为web服务器启动程序，包括创建、绑定、监听scoket，
 * 如果port为0，可动态分配端口号，并由port参数带出
 * --------------------------------------------------------------------
 * 参数: 绑定端口的变量，设为引用，为了将其传出
 * 返回: 可以用于服务的scoket */
/**********************************************************************/
int startup(u_short &port)
{
    int httpd = 0;
    struct sockaddr_in name;

    // 这里的PF_INET其实是与 AF_INET同义，AF_INET只是PF_INET的别名
    httpd = socket(PF_INET, SOCK_STREAM, 0);
    if (httpd == -1)
        error_die("socket");
    int opt = 1;

    // 增加端口复用
    setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, (const void *)&opt, sizeof(opt));

    memset(&name, 0, sizeof(name));
    name.sin_family = AF_INET;
    // 将port 转换成以网络字节序表示的16位整数
    name.sin_port = htons(port);
    // INADDR_ANY是一个 IPV4通配地址的常量，实现为0.0.0.0
    name.sin_addr.s_addr = htonl(INADDR_ANY);

    // bind()用于绑定地址与 socket
    // 如果传进去的sockaddr结构中的 sin_port 指定为0，这时系统会选择一个临时的端口号
    if (bind(httpd, (struct sockaddr *)&name, sizeof(name)) < 0)
        error_die("bind");

    //如果调用 bind 后端口号仍然是0，则手动调用getsockname()获取端口号
    if (port == 0)
    {
        socklen_t namelen = sizeof(name);
        // getsockname()包含于<sys/socker.h>中
        // 调用getsockname()获取系统给 httpd 这个 socket 随机分配的端口号
        if (::getsockname(httpd, (struct sockaddr *)&name, &namelen) == -1)
            error_die("getsockname");
        port = ntohs(name.sin_port);
    }

    if (listen(httpd, 128) < 0)
        error_die("listen");
    return httpd;
}

/**********************************************************************/
/* 通知客户端请求的web方法尚未被调用执行。
 * 参数: 客户端scoket */
/**********************************************************************/
void unimplemented(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</TITLE></HEAD>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}
