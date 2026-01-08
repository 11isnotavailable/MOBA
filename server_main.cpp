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
#include <thread> 

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

// 后台数据持久化线程函数
void data_persistence_thread(UserManager* um) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        um->save_db();
    }
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        return -1;
    }

    int opt = 1; 
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    int flag = 1;
    setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
    
    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        return -1;
    }

    if (listen(server_fd, 1024) < 0) {
        perror("listen failed");
        return -1;
    }

    UserManager user_mgr;
    RoomManager room_mgr(&user_mgr);

    // 启动后台持久化线程
    std::thread bg_saver(data_persistence_thread, &user_mgr);
    bg_saver.detach(); 
    std::cout << "[Server] Background persistence thread started." << std::endl;

    int epoll_fd = epoll_create1(0);
    epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    std::cout << "[Server] Listening on port " << PORT << "..." << std::endl;

    const int TICK_MS = 33; // 约30FPS
    long long last_tick_time = get_current_ms();

    while (true) {
        long long now = get_current_ms();
        long long elapsed = now - last_tick_time;
        int wait_ms = TICK_MS - (int)elapsed;
        if (wait_ms < 0) wait_ms = 0; 
        
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, wait_ms);

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                sockaddr_in client_addr; 
                socklen_t len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &len);
                if (client_fd >= 0) {
                    setNonBlocking(client_fd);
                    ev.events = EPOLLIN | EPOLLET; 
                    ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
                    client_buffers[client_fd] = {{0}, 0};
                    std::cout << "[Server] New connection: " << client_fd << std::endl;
                }
            } else {
                int fd = events[i].data.fd;
                char recv_buf[2048];
                int n_read = read(fd, recv_buf, sizeof(recv_buf));
                
                if (n_read <= 0) {
                    if (n_read < 0 && errno == EAGAIN) continue;
                    std::cout << "[Server] Client disconnected: " << fd << std::endl;
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    client_buffers.erase(fd);
                    room_mgr.on_player_disconnect(fd);
                    user_mgr.logout_user(fd);
                } else {
                    ClientBuffer& buf_obj = client_buffers[fd];
                    if (buf_obj.len + n_read > 10240) { 
                        // 简单处理缓冲区溢出
                        buf_obj.len = 0; 
                    } 
                    memcpy(buf_obj.data + buf_obj.len, recv_buf, n_read);
                    buf_obj.len += n_read;

                    int ptr = 0;
                    while (buf_obj.len - ptr >= 4) {
                        int type = *(int*)(buf_obj.data + ptr);
                        size_t pkt_len = 0;

                        // 根据类型判定包长度
                        if (type == TYPE_LOGIN_REQ || type == TYPE_REG_REQ) pkt_len = sizeof(LoginPacket);
                        else if (type == TYPE_ROOM_LIST_REQ || type == TYPE_MATCH_REQ || 
                                 type == TYPE_CREATE_ROOM || type == TYPE_LEAVE_ROOM || type == TYPE_GAME_START) pkt_len = sizeof(int);
                        else if (type == TYPE_JOIN_ROOM || type == TYPE_ROOM_UPDATE) pkt_len = sizeof(RoomControlPacket);
                        else if (type == TYPE_MOVE || type == TYPE_ATTACK || type == TYPE_SPELL || 
                                 type == TYPE_SELECT || type == TYPE_BUY_ITEM || 
                                 type == TYPE_SKILL_U || type == TYPE_SKILL_I) pkt_len = sizeof(GamePacket);

                        // 长度非法或数据不足，跳出循环等待后续数据
                        if (pkt_len == 0) { 
                            buf_obj.len = 0; // 发生未知错误，重置缓冲区
                            break; 
                        } 
                        if (buf_obj.len - ptr < (int)pkt_len) break; 

                        void* pdata = buf_obj.data + ptr;

                        // 分发处理
                        if (type == TYPE_LOGIN_REQ || type == TYPE_REG_REQ) {
                            LoginPacket* pkt = (LoginPacket*)pdata;
                            LoginResponsePacket resp; 
                            memset(&resp, 0, sizeof(resp));
                            resp.type = (type == TYPE_LOGIN_REQ ? TYPE_LOGIN_RESP : TYPE_REG_RESP);
                            int ret = (type == TYPE_REG_REQ) ? user_mgr.register_user(pkt->username, pkt->password) : user_mgr.login_user(fd, pkt->username, pkt->password);
                            resp.result = ret; 
                            resp.user_id = fd; 
                            write(fd, &resp, sizeof(resp));
                        }
                        else if (type >= 20 && type <= 29) {
                            // 大厅与房间控制 (TYPE_JOIN_ROOM 会在这里被处理)
                            if (type == TYPE_ROOM_UPDATE) {
                                room_mgr.handle_room_control(fd, *(RoomControlPacket*)pdata); 
                            }
                            else {
                                // 修正点：直接传递 pdata，RoomManager 内部会按照 RoomControlPacket 解析
                                room_mgr.handle_lobby_packet(fd, type, pdata);
                            }
                        }
                        else {
                            // 游戏内逻辑
                            room_mgr.handle_game_packet(fd, *(GamePacket*)pdata);
                        }

                        ptr += pkt_len;
                    }

                    // 移动剩余数据
                    if (ptr > 0) {
                        if (ptr < buf_obj.len) memmove(buf_obj.data, buf_obj.data + ptr, buf_obj.len - ptr);
                        buf_obj.len -= ptr;
                    }
                }
            }
        }

        // 逻辑帧更新
        now = get_current_ms();
        if (now - last_tick_time >= TICK_MS) {
            room_mgr.update_all();
            last_tick_time = now;
        }
    }
    
    close(server_fd);
    return 0;
}