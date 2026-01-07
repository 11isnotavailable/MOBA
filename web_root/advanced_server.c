#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/sendfile.h> // 零拷贝核心头文件
#include <pthread.h>      // 线程池核心头文件

#define PORT 8080
#define MAX_EVENTS 10000
#define THREAD_POOL_SIZE 8  // 线程池大小，通常设为 CPU 核心数
#define QUEUE_SIZE 1000     // 任务队列大小
#define WEB_ROOT "./web_root"

// --- 任务队列结构 ---
typedef struct {
    int client_fd;
} task_t;

typedef struct {
    task_t tasks[QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int shutdown;
} thread_pool_t;

thread_pool_t pool; // 全局线程池

// --- 辅助函数：设置非阻塞 ---
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// --- 业务逻辑：发送 HTTP 响应 ---
void send_response(int client_fd, int status, const char *desc, const char *type, long len) {
    char header[1024];
    sprintf(header, 
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Connection: close\r\n"
            "Server: Advanced-Epoll-Server/1.0\r\n\r\n", 
            status, desc, type, len);
    write(client_fd, header, strlen(header));
}

void send_error(int client_fd, int status, const char *desc) {
    char body[1024];
    sprintf(body, "<html><body><h1>%d %s</h1><p>Server Error</p></body></html>", status, desc);
    send_response(client_fd, status, desc, "text/html", strlen(body));
    write(client_fd, body, strlen(body));
}

// --- 核心业务：处理单个请求 ---
void handle_request(int client_fd) {
    char buffer[4096];
    // 简单的读取，实际场景可能需要循环读取直到读完 HTTP 头
    ssize_t len = read(client_fd, buffer, sizeof(buffer) - 1);
    
    if (len <= 0) {
        close(client_fd);
        return;
    }
    buffer[len] = '\0';

    char method[16], path[256];
    sscanf(buffer, "%s %s", method, path);

    // 简单路由：/ -> /index.html
    if (strcmp(path, "/") == 0) strcpy(path, "/index.html");

    // 安全检查
    if (strstr(path, "..")) {
        send_error(client_fd, 403, "Forbidden");
        close(client_fd);
        return;
    }

    char file_path[512];
    sprintf(file_path, "%s%s", WEB_ROOT, path);

    struct stat st;
    if (stat(file_path, &st) == -1) {
        send_error(client_fd, 404, "Not Found");
        close(client_fd);
        return;
    }

    int file_fd = open(file_path, O_RDONLY);
    if (file_fd == -1) {
        send_error(client_fd, 500, "Internal Server Error");
        close(client_fd);
        return;
    }

    // --- 高级功能：零拷贝发送 (Zero-Copy) ---
    send_response(client_fd, 200, "OK", "text/html", st.st_size);
    
    off_t offset = 0;
    // sendfile 直接在内核态搬运数据，不经过用户态 buffer
    ssize_t sent = sendfile(client_fd, file_fd, &offset, st.st_size);
    
    close(file_fd);
    close(client_fd); // 处理完毕，关闭连接
}

// --- 线程池工作函数 ---
void *worker_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&pool.mutex);
        
        // 如果没有任务，等待
        while (pool.count == 0 && !pool.shutdown) {
            pthread_cond_wait(&pool.cond, &pool.mutex);
        }

        if (pool.shutdown) {
            pthread_mutex_unlock(&pool.mutex);
            pthread_exit(NULL);
        }

        // 取出任务
        task_t task = pool.tasks[pool.head];
        pool.head = (pool.head + 1) % QUEUE_SIZE;
        pool.count--;

        pthread_mutex_unlock(&pool.mutex);

        // 执行任务 (处理请求)
        handle_request(task.client_fd);
    }
}

// --- 线程池初始化 ---
void thread_pool_init() {
    pool.head = 0;
    pool.tail = 0;
    pool.count = 0;
    pool.shutdown = 0;
    pthread_mutex_init(&pool.mutex, NULL);
    pthread_cond_init(&pool.cond, NULL);

    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, worker_thread, NULL);
        pthread_detach(tid); // 线程分离，自动回收资源
    }
}

// --- 添加任务到线程池 ---
void thread_pool_add(int client_fd) {
    pthread_mutex_lock(&pool.mutex);
    
    if (pool.count < QUEUE_SIZE) {
        pool.tasks[pool.tail].client_fd = client_fd;
        pool.tail = (pool.tail + 1) % QUEUE_SIZE;
        pool.count++;
        pthread_cond_signal(&pool.cond); // 唤醒一个工作线程
    } else {
        // 队列满了，直接拒绝或关闭
        printf("Queue full! Dropping connection.\n");
        close(client_fd);
    }
    
    pthread_mutex_unlock(&pool.mutex);
}

// --- 主程序 ---
int main() {
    int server_fd, epoll_fd;
    struct sockaddr_in address;
    struct epoll_event ev, events[MAX_EVENTS];

    // 1. 初始化线程池
    thread_pool_init();
    printf("Thread Pool Initialized with %d workers.\n", THREAD_POOL_SIZE);

    // 2. 创建监听 Socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed"); exit(1);
    }
    if (listen(server_fd, 1000) < 0) {
        perror("listen failed"); exit(1);
    }
    set_nonblocking(server_fd); // 监听套接字必须非阻塞

    // 3. Epoll 初始化
    epoll_fd = epoll_create1(0);
    ev.events = EPOLLIN | EPOLLET; // 边缘触发模式 (Edge Triggered)
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    printf("Advanced Epoll Server running on port %d...\n", PORT);

    // 4. 事件循环
    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        
        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == server_fd) {
                // A. 处理新连接 (ET模式下需要循环accept直到读空)
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t len = sizeof(client_addr);
                    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &len);
                    
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 读完了
                        else { perror("accept"); break; }
                    }
                    
                    set_nonblocking(client_fd);
                    
                    // 将新连接加入 epoll，关注读事件和ET模式，并开启一次性触发(EPOLLONESHOT)防止多线程竞争
                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                    client_ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);
                }
            } else {
                // B. 处理数据：如果有数据可读，扔给线程池
                // 注意：这里我们简单地将 socket fd 传给线程池
                // 线程处理完后会关闭连接。如果需要长连接，逻辑会更复杂
                if (events[i].events & EPOLLIN) {
                    thread_pool_add(events[i].data.fd);
                }
            }
        }
    }
    return 0;
}