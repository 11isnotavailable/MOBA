#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#define PORT 8080           // 监听端口
#define MAX_EVENTS 1000     // epoll 最大事件数
#define BUFFER_SIZE 4096    // 缓冲区大小
#define WEB_ROOT "./web_root" // 网页根目录

// 设置文件描述符为非阻塞模式 (Epoll 的关键)
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 发送 HTTP 响应头
void send_response_header(int client_fd, int status, const char *desc, const char *type, long len) {
    char buf[1024];
    sprintf(buf, "HTTP/1.1 %d %s\r\n", status, desc);
    sprintf(buf + strlen(buf), "Content-Type: %s\r\n", type);
    sprintf(buf + strlen(buf), "Content-Length: %ld\r\n", len);
    sprintf(buf + strlen(buf), "Connection: close\r\n"); // 短连接
    sprintf(buf + strlen(buf), "\r\n"); // 空行，表示头结束
    write(client_fd, buf, strlen(buf));
}

// 发送错误页面 (404 或 400)
void send_error(int client_fd, int status, const char *desc) {
    char body[1024];
    sprintf(body, "<html><body><h1>%d %s</h1></body></html>", status, desc);
    send_response_header(client_fd, status, desc, "text/html", strlen(body));
    write(client_fd, body, strlen(body));
}

// 处理 HTTP 请求
void handle_client_request(int client_fd) {
    char buffer[BUFFER_SIZE];
    int len = read(client_fd, buffer, sizeof(buffer) - 1);
    
    if (len <= 0) {
        // 读取错误或连接关闭
        return;
    }
    buffer[len] = '\0';

    // 1. 解析请求行 (例如: "GET /index.html HTTP/1.1")
    char method[16], path[256], protocol[16];
    sscanf(buffer, "%s %s %s", method, path, protocol);

    printf("[Request] Method: %s, Path: %s\n", method, path);

    // 只支持 GET
    if (strcasecmp(method, "GET") != 0) {
        send_error(client_fd, 400, "Bad Request");
        return;
    }

    // 2. 简单的路由处理
    // 如果路径是 /，默认映射到 index.html
    if (strcmp(path, "/") == 0) {
        strcpy(path, "/index.html");
    }

    // 安全检查：防止目录遍历攻击 (包含 .. 就拒绝)
    if (strstr(path, "..")) {
        send_error(client_fd, 403, "Forbidden");
        return;
    }

    // 3. 拼接本地文件路径
    char file_path[512];
    sprintf(file_path, "%s%s", WEB_ROOT, path);

    // 4. 打开文件
    struct stat st;
    if (stat(file_path, &st) == -1) {
        // 文件不存在
        send_error(client_fd, 404, "Not Found");
        return;
    }

    int file_fd = open(file_path, O_RDONLY);
    if (file_fd == -1) {
        send_error(client_fd, 500, "Internal Server Error");
        return;
    }

    // 5. 发送正常响应 (200 OK)
    send_response_header(client_fd, 200, "OK", "text/html", st.st_size);

    // 发送文件内容 (使用 sendfile 可以零拷贝，这里用简单的 read/write 方便理解)
    int n;
    while ((n = read(file_fd, buffer, sizeof(buffer))) > 0) {
        write(client_fd, buffer, n);
    }
    
    close(file_fd);
}

int main() {
    int server_fd, epoll_fd;
    struct sockaddr_in address;
    struct epoll_event ev, events[MAX_EVENTS];

    // 1. 创建监听 Socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) { perror("socket"); exit(1); }

    // 端口复用 (避免重启时 bind error)
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // 2. 绑定与监听
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen"); exit(1);
    }

    printf("Epoll Web Server running on port %d...\n", PORT);

    // 3. 创建 epoll 实例
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) { perror("epoll_create1"); exit(1); }

    // 将 server_fd 添加到 epoll 监控列表，关注“读事件” (EPOLLIN)
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
        perror("epoll_ctl: server_fd"); exit(1);
    }

    // 4. 事件循环
    while (1) {
        // 等待事件发生 (阻塞)
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) { perror("epoll_wait"); break; }

        for (int n = 0; n < nfds; ++n) {
            if (events[n].data.fd == server_fd) {
                // A. 如果是 server_fd 有动静，说明有新连接进来 -> accept
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd == -1) continue;

                set_nonblocking(client_fd); // 设为非阻塞

                // 把新连接也加入 epoll 监控
                ev.events = EPOLLIN | EPOLLET; // 边缘触发模式 (Edge Triggered)
                ev.data.fd = client_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
                
                // printf("New connection: fd=%d\n", client_fd);
            } else {
                // B. 如果是其他 fd 有动静，说明有数据发过来 -> 处理请求
                int client_fd = events[n].data.fd;
                handle_client_request(client_fd);
                
                // 处理完后关闭连接 (HTTP短连接模型)
                // 在 epoll 中关闭 fd 会自动从监控列表中移除
                close(client_fd);
            }
        }
    }

    close(server_fd);
    close(epoll_fd);
    return 0;
}