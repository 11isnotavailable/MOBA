#include <iostream>
#include <map>
#include <vector>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include "protocol.h"
#include "map.h" 

#define PORT 8888
#define MAX_EVENTS 1000

// === 全局数据 ===
int game_map[MAP_SIZE][MAP_SIZE];

// 物理判定
bool is_valid_move(int x, int y) {
    if (x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE) return false;
    int t_head = game_map[y][x];
    // 简单的脚部判定（防止穿墙）
    int t_feet = (y+1 < MAP_SIZE) ? game_map[y+1][x] : TILE_WALL;
    
    // 允许穿过水晶(Base)和空地/河道，不允许穿墙和塔
    auto is_obs = [](int t) { 
        // 墙(1), 塔壁(10), 塔身(11-23)
        return t == TILE_WALL || t == TILE_TOWER_WALL || (t >= 11 && t <= 23); 
    };
    
    if (is_obs(t_head) || is_obs(t_feet)) return false;
    return true;
}

// === 玩家系统 ===
struct HeroTemplate {
    int id; int base_hp; int attack_range; int attack_dmg; int color; const char* name;
};

std::map<int, HeroTemplate> hero_db = {
    {HERO_WARRIOR, {HERO_WARRIOR, 1000, 2, 80, 1, "Warrior"}}, 
    {HERO_MAGE,    {HERO_MAGE,    500,  6, 50, 2, "Mage"}},    
    {HERO_TANK,    {HERO_TANK,    2000, 2, 30, 3, "Tank"}}     
};

struct Player {
    int fd, id, x, y, hp, max_hp, hero_id, color;
    bool is_playing;
    int current_effect;    
    int current_target_id; 
};

std::map<int, Player> players;
int global_id_counter = 1;

int setNonBlocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    return fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

int dist_sq(int x1, int y1, int x2, int y2) {
    return (x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2);
}

// === 战斗判定 ===
bool handle_attack(int attacker_fd) {
    if (players.count(attacker_fd) == 0) return false;
    Player& att = players[attacker_fd];
    if (hero_db.count(att.hero_id) == 0) return false;
    HeroTemplate& tmpl = hero_db[att.hero_id];

    bool hit_anyone = false;
    int range_sq = (tmpl.attack_range + 1) * (tmpl.attack_range + 1);

    for (auto& pair : players) {
        Player& target = pair.second;
        if (target.id == att.id) continue;
        if (!target.is_playing) continue;

        if (dist_sq(att.x, att.y, target.x, target.y) <= range_sq) {
            target.hp -= tmpl.attack_dmg;
            hit_anyone = true;
            target.current_effect = EFFECT_HIT; 
            att.current_target_id = target.id;

            std::cout << "[Combat] " << att.id << " -> " << target.id 
                      << " DMG:" << tmpl.attack_dmg << " HP:" << target.hp << std::endl;

            if (target.hp <= 0) {
                target.hp = target.max_hp;
                // 复活点：泉水中心 (12, 138)
                target.x = 12; 
                target.y = 138; 
                std::cout << "[Kill] " << target.id << " died and respawned." << std::endl;
            }
        }
    }
    return hit_anyone;
}

void broadcast_world() {
    // 1. 先构建好 update 包
    std::vector<GamePacket> updates;
    for (auto& sp : players) {
        if (!sp.second.is_playing) continue;
        Player& p = sp.second;
        int range = 0;
        if (hero_db.count(p.hero_id)) range = hero_db[p.hero_id].attack_range;

        GamePacket pkt = {
            TYPE_UPDATE, p.id, p.x, p.y, p.hero_id, 0, p.color, 
            p.hp, p.max_hp, range, 
            p.current_effect, p.current_target_id
        };
        updates.push_back(pkt);
    }

    // 2. 发送给所有人
    GamePacket end_pkt; memset(&end_pkt, 0, sizeof(end_pkt));
    end_pkt.type = TYPE_FRAME;

    for (auto& pair : players) {
        int target_fd = pair.first;
        if (!pair.second.is_playing) continue;

        for(const auto& pkt : updates) {
            write(target_fd, &pkt, sizeof(pkt));
        }
        write(target_fd, &end_pkt, sizeof(end_pkt));
    }

    // 3. 清除临时状态
    for (auto& pair : players) {
        pair.second.current_effect = EFFECT_NONE;
        pair.second.current_target_id = 0;
    }
}

int main() {
    MapGenerator::init(game_map);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in address = {AF_INET, htons(PORT), INADDR_ANY};
    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, SOMAXCONN);
    
    int epoll_fd = epoll_create1(0);
    epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN; event.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    std::cout << "MOBA Server (Vector Move Support) Started..." << std::endl;

    while (true) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        bool need_broadcast = false;

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                sockaddr_in c_addr; socklen_t len = sizeof(c_addr);
                int c_fd = accept(server_fd, (struct sockaddr*)&c_addr, &len);
                setNonBlocking(c_fd);
                event.events = EPOLLIN | EPOLLET; event.data.fd = c_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, c_fd, &event);
                
                players[c_fd] = {c_fd, global_id_counter++, 0, 0, 0, 0, 0, 0, false};
                GamePacket welcome = {0, players[c_fd].id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
                write(c_fd, &welcome, sizeof(welcome));
                
                std::cout << "Client Connected ID: " << players[c_fd].id << std::endl;
            } else {
                int fd = events[i].data.fd;
                GamePacket pkt;
                int count = read(fd, &pkt, sizeof(pkt));
                
                if (count <= 0) {
                    close(fd); epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                    players.erase(fd); need_broadcast = true;
                } else {
                    if (players.count(fd)) {
                        Player& p = players[fd];
                        
                        if (!p.is_playing && pkt.type == TYPE_SELECT) {
                            if (hero_db.count(pkt.input)) {
                                auto tmpl = hero_db[pkt.input];
                                // 出生点：泉水中心 (12, 138)
                                p = {fd, p.id, 12, 138, tmpl.base_hp, tmpl.base_hp, tmpl.id, tmpl.color, true};
                                need_broadcast = true;
                                std::cout << "Player " << p.id << " spawned." << std::endl;
                            }
                        } 
                        else if (p.is_playing) {
                            if (pkt.type == TYPE_MOVE) {
                                // [关键修改] 读取向量 dx, dy 而不是 input
                                int dx = pkt.x; 
                                int dy = pkt.y;

                                // 简单的输入过滤，防止一次走太远
                                if (dx < -1) dx = -1; if (dx > 1) dx = 1;
                                if (dy < -1) dy = -1; if (dy > 1) dy = 1;

                                int nx = p.x + dx;
                                int ny = p.y + dy;
                                
                                if (is_valid_move(nx, ny)) { 
                                    p.x = nx; 
                                    p.y = ny; 
                                    need_broadcast = true; 
                                }
                            }
                            else if (pkt.type == TYPE_ATTACK) {
                                if (handle_attack(fd)) need_broadcast = true;
                            }
                            else if (pkt.type == TYPE_SPELL) {
                                p.hp += 100; if(p.hp > p.max_hp) p.hp = p.max_hp;
                                need_broadcast = true;
                            }
                        }
                    }
                }
            }
        }
        if (need_broadcast) broadcast_world();
    }
    return 0;
}