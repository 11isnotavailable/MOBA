#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h> 
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <cstring>
#include <chrono> 
#include <map>
#include <thread> // [新增] 多线程支持

#include "protocol.h"
#include "user_manager.h"
#include "room_manager.h"

#define PORT 8888
#define MAX_EVENTS 1000

// 获取当前毫秒时间戳
static long long get_current_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 每个客户端独立的接收缓冲区
struct ClientBuffer {
    char data[10240];
    int len;
};
std::map<int, ClientBuffer> client_buffers;

// 设置非阻塞
int setNonBlocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    return fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

// [新增] 后台数据持久化线程函数
// 每隔 30 秒将用户数据写入磁盘
void data_persistence_thread(UserManager* um) {
    while (true) {
        // 休眠 30 秒
        std::this_thread::sleep_for(std::chrono::seconds(30));
        
        // 执行保存 (UserManager 内部已加锁，线程安全)
        um->save_db();
        // std::cout << "[Background Thread] Auto-saved user data." << std::endl;
    }
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // [重要] 开启 TCP_NODELAY，这对实时移动至关重要
    int flag = 1;
    setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
    
    sockaddr_in address = {AF_INET, htons(PORT), INADDR_ANY};
    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 3);

    UserManager user_mgr;
    RoomManager room_mgr(&user_mgr);

    // [新增] 启动后台线程
    std::thread bg_saver(data_persistence_thread, &user_mgr);
    bg_saver.detach(); // 分离线程，让它独立运行
    std::cout << "[Server] Background persistence thread started." << std::endl;

    int epoll_fd = epoll_create1(0);
    epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    std::cout << "[Server] Listening on port " << PORT << "..." << std::endl;

    // [新增] 游戏循环时间控制
    const int TICK_MS = 33; // 目标帧率 30FPS (约33ms一帧)
    long long last_tick_time = get_current_ms();

    while (true) {
        // 1. 计算 epoll 应该等待多久
        long long now = get_current_ms();
        long long elapsed = now - last_tick_time;
        int wait_ms = TICK_MS - (int)elapsed;
        
        if (wait_ms < 0) wait_ms = 0; // 如果逻辑处理超时，立即进入下一帧
        
        // 2. 等待网络事件 (用剩余时间 wait，而不是死板的 usleep)
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, wait_ms);

        // 3. 处理网络 IO
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                sockaddr_in client_addr; socklen_t len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &len);
                setNonBlocking(client_fd);
                ev.events = EPOLLIN | EPOLLET; 
                ev.data.fd = client_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
                client_buffers[client_fd] = {0, 0};
                std::cout << "[Server] New connection: " << client_fd << std::endl;
            } else {
                int fd = events[i].data.fd;
                char buf[1024];
                int len = read(fd, buf, sizeof(buf));
                
                if (len <= 0) {
                    // 断开连接
                    if (len < 0 && errno == EAGAIN) continue;
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    client_buffers.erase(fd);
                    
                    room_mgr.on_player_disconnect(fd);
                    user_mgr.logout_user(fd);
                } else {
                    // 处理粘包
                    ClientBuffer& buf_obj = client_buffers[fd];
                    if (buf_obj.len + len > 10240) { len = 10240 - buf_obj.len; } // 防止溢出
                    memcpy(buf_obj.data + buf_obj.len, buf, len);
                    buf_obj.len += len;

                    int ptr = 0;
                    while (ptr < buf_obj.len) {
                        if (buf_obj.len - ptr < 4) break; 
                        int type = *(int*)(buf_obj.data + ptr);
                        
                        size_t pkt_len = 0;
                        if (type == TYPE_LOGIN_REQ || type == TYPE_REG_REQ) pkt_len = sizeof(LoginPacket);
                        else if (type == TYPE_ROOM_LIST_REQ || type == TYPE_MATCH_REQ || 
                                 type == TYPE_CREATE_ROOM || type == TYPE_LEAVE_ROOM || type == TYPE_GAME_START) pkt_len = sizeof(int);
                        else if (type == TYPE_JOIN_ROOM || type == TYPE_ROOM_UPDATE) pkt_len = sizeof(RoomControlPacket); // JOIN复用结构
                        else if (type == TYPE_MOVE || type == TYPE_ATTACK || type == TYPE_SPELL || 
                                 type == TYPE_SELECT || type == TYPE_BUY_ITEM) pkt_len = sizeof(GamePacket);

                        if (pkt_len == 0) { ptr = buf_obj.len; break; } // Error
                        if (buf_obj.len - ptr < pkt_len) break; // Incomplete

                        void* pdata = buf_obj.data + ptr;

                        // 分发逻辑
                        if (type == TYPE_LOGIN_REQ || type == TYPE_REG_REQ) {
                            LoginPacket* pkt = (LoginPacket*)pdata;
                            LoginResponsePacket resp; resp.type = (type==TYPE_LOGIN_REQ?TYPE_LOGIN_RESP:TYPE_REG_RESP);
                            int ret = (type == TYPE_REG_REQ) ? user_mgr.register_user(pkt->username, pkt->password) : user_mgr.login_user(fd, pkt->username, pkt->password);
                            resp.result = ret; resp.user_id = fd; 
                            write(fd, &resp, sizeof(resp));
                        }
                        else if (type >= 20 && type <= 29) {
                            if (type == TYPE_ROOM_UPDATE) {
                                room_mgr.handle_room_control(fd, *(RoomControlPacket*)pdata); // Ready/Switch
                            }
                            else {
                                room_mgr.handle_lobby_packet(fd, type, pdata);
                            }
                        }
                        else {
                            room_mgr.handle_game_packet(fd, *(GamePacket*)pdata);
                        }

                        ptr += pkt_len;
                    }

                    if (ptr > 0) {
                        if (ptr < buf_obj.len) memmove(buf_obj.data, buf_obj.data + ptr, buf_obj.len - ptr);
                        buf_obj.len -= ptr;
                    }
                }
            }
        }

        // 4. 检查是否需要执行游戏逻辑 Tick
        now = get_current_ms();
        if (now - last_tick_time >= TICK_MS) {
            // 执行所有房间的逻辑更新
            room_mgr.update_all();
            
            // 重置时间 (简单的追赶策略)
            last_tick_time = now;
        }
    }
    
    close(server_fd);
    return 0;
}