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

#include "protocol.h"
#include "user_manager.h"
#include "room_manager.h"

#define PORT 8888
#define MAX_EVENTS 1000

// 每个客户端独立的接收缓冲区
struct ClientBuffer {
    char data[10240];
    int len;
};
std::map<int, ClientBuffer> client_buffers;

int setNonBlocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    return fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // [重要] 开启 TCP_NODELAY，这对实时移动至关重要
    int flag = 1;
    setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
    
    sockaddr_in address = {AF_INET, htons(PORT), INADDR_ANY};
    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, SOMAXCONN);
    
    int epoll_fd = epoll_create1(0);
    epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN; event.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    UserManager user_mgr("users.txt");
    RoomManager room_mgr(&user_mgr);

    std::cout << "MOBA Server Started on 8888." << std::endl;

    while (true) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 5);
        
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                sockaddr_in c_addr; socklen_t len = sizeof(c_addr);
                int c_fd = accept(server_fd, (struct sockaddr*)&c_addr, &len);
                setNonBlocking(c_fd);
                int f=1; setsockopt(c_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&f, sizeof(int));
                
                event.events = EPOLLIN | EPOLLET; event.data.fd = c_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, c_fd, &event);
                
                client_buffers[c_fd].len = 0;
                std::cout << "Client connected: " << c_fd << std::endl;
            } else {
                int fd = events[i].data.fd;
                ClientBuffer& buf_obj = client_buffers[fd];
                
                // 1. 读取数据追加到缓冲区
                int count = read(fd, buf_obj.data + buf_obj.len, sizeof(buf_obj.data) - buf_obj.len);
                
                if (count <= 0) {
                    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { /* 稍后重试 */ }
                    else {
                        close(fd); epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                        user_mgr.logout_user(fd); room_mgr.on_player_disconnect(fd);
                        client_buffers.erase(fd); // 清理缓冲
                        std::cout << "Client disconnected: " << fd << std::endl;
                    }
                } else {
                    buf_obj.len += count;
                    int ptr = 0;
                    
                    // 2. 循环解析包
                    while (ptr < buf_obj.len) {
                        if (buf_obj.len - ptr < 4) break; // 连包头都不够
                        
                        int type = *(int*)(buf_obj.data + ptr);
                        int pkt_len = 0;

                        // 确定包长
                        if (type == TYPE_LOGIN_REQ || type == TYPE_REG_REQ) pkt_len = sizeof(LoginPacket);
                        else if (type == TYPE_JOIN_ROOM || type == TYPE_ROOM_UPDATE) pkt_len = sizeof(RoomControlPacket);
                        else if (type >= 10 && type <= 21) pkt_len = sizeof(int); // 只有头的包
                        else pkt_len = sizeof(GamePacket); // 游戏包

                        if (buf_obj.len - ptr < pkt_len) break; // 数据不够完整包

                        void* pdata = buf_obj.data + ptr;

                        // [调试日志] 可以在这里监控所有包
                        // if (type == TYPE_MOVE) std::cout << "[NET] Recv MOVE from " << fd << std::endl;

                        // 3. 处理包
                        if (type == TYPE_LOGIN_REQ || type == TYPE_REG_REQ) {
                            LoginPacket* pkt = (LoginPacket*)pdata;
                            LoginResponsePacket resp; memset(&resp, 0, sizeof(resp));
                            resp.type = (type == TYPE_LOGIN_REQ) ? TYPE_LOGIN_RESP : TYPE_REG_RESP;
                            int ret = (type == TYPE_REG_REQ) ? user_mgr.register_user(pkt->username, pkt->password) : user_mgr.login_user(fd, pkt->username, pkt->password);
                            resp.result = ret; resp.user_id = fd; 
                            write(fd, &resp, sizeof(resp));
                        }
                        else if (type >= 14 && type <= 21) {
                            if (type == TYPE_ROOM_UPDATE) room_mgr.handle_room_control(fd, *(RoomControlPacket*)pdata);
                            else room_mgr.handle_lobby_packet(fd, type, pdata);
                        }
                        else {
                            // [DEBUG] 如果是移动包，在这里打印
                            if (type == TYPE_MOVE) {
                                std::cout << "[NET] Recv MOVE from FD: " << fd << ". Forwarding to Room..." << std::endl;
                            }
                            room_mgr.handle_game_packet(fd, *(GamePacket*)pdata);
                        }

                        ptr += pkt_len;
                    }

                    // 4. 移动残余数据
                    if (ptr > 0) {
                        if (ptr < buf_obj.len) memmove(buf_obj.data, buf_obj.data + ptr, buf_obj.len - ptr);
                        buf_obj.len -= ptr;
                    }
                }
            }
        }

        room_mgr.update_all();
        usleep(5000); 
    }
    return 0;
}