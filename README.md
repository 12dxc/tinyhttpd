### clone自tinyhttpd，有丰富注释
#### 加入了C++标准线程，增强了可移植性
### 执行流程:
1. startup -> accept_request -> serve_file | execute_cgi
2. startup() 完成标准服务的socket创建流程
3. accept_request() 处理客户端的连接，解析请求方法和请求资源，如果需要cgi则调用execute_cgi，否则调用serve_file
    1. serve_file() 组装回送报文，读取文件并发送到客户端
    2. execute_cgi() 执行cgi脚本文件，并把输出发送给客户端，这里采用管道和cgi进程进行通信
4. 其他函数为解析http报文或组装报文
