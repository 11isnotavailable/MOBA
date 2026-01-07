#include <iostream>
#include <map>
#include <vector>
#include <cstring>
#include <cmath>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <chrono>
#include <algorithm>
#include "protocol.h"
#include "map.h" 

#define PORT 8888
#define MAX_EVENTS 1000

// === 辅助工具 ===
long long get_current_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int dist_sq(int x1, int y1, int x2, int y2) {
    return (x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2);
}

// === 全局数据 ===
int game_map[MAP_SIZE][MAP_SIZE];
long long game_start_time = 0;

// 物理判定
bool is_valid_move(int x, int y) {
    if (x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE) return false;
    int t = game_map[y][x];
    return !(t == TILE_WALL || t == TILE_TOWER_WALL || (t >= 11 && t <= 23));
}

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

struct Point { int x, y; };
const std::vector<Point> PATH_TOP = { {22, 128}, {22, 22},  {128, 22} };  
const std::vector<Point> PATH_MID = { {22, 128}, {75, 75},  {128, 22} };  
const std::vector<Point> PATH_BOT = { {22, 128}, {128, 128},{128, 22} };  

struct Minion {
    int id;
    int team; 
    int type; 
    float x, y; 
    int hp, max_hp;
    int dmg, range;
    
    int lane;   
    int wp_idx; 

    enum State { MARCHING, CHASING, RETURNING };
    State state;
    
    int target_id; 
    float anchor_x, anchor_y; 
    long long last_attack_time; 
};

std::map<int, Player> players;
std::map<int, Minion> minions;
int global_id_counter = 1;
int minion_id_counter = MINION_ID_START;
int last_spawn_minute = -1; 

int setNonBlocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    return fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

// === 出兵逻辑 ===
void spawn_wave() {
    std::cout << "[System] Minion Wave Spawned (Time: " << (get_current_ms() - game_start_time)/1000 << "s)" << std::endl;
    for (int lane = 0; lane < 3; lane++) {
        // Blue (2近1远)
        for(int i=0; i<3; i++) {
            Minion m;
            m.id = minion_id_counter++; m.team = 1; m.lane = lane;
            // i=0,1 为近战(TYPE=1), i=2 为远程(TYPE=2)
            m.type = (i < 2) ? MINION_TYPE_MELEE : MINION_TYPE_RANGED;
            
            m.hp = (m.type == MINION_TYPE_MELEE) ? MELEE_HP : RANGED_HP; m.max_hp = m.hp;
            m.dmg = (m.type == MINION_TYPE_MELEE) ? MELEE_DMG : RANGED_DMG;
            m.range = (m.type == MINION_TYPE_MELEE) ? MELEE_RANGE : RANGED_RANGE;
            
            // 为了防止重叠太厉害，给个微小的随机偏移
            m.x = 22 + rand()%2; m.y = 128 + rand()%2; 
            m.wp_idx = 0; m.state = Minion::MARCHING;
            m.last_attack_time = 0;
            minions[m.id] = m;
        }
        // Red (2近1远)
        for(int i=0; i<3; i++) {
            Minion m;
            m.id = minion_id_counter++; m.team = 2; m.lane = lane;
            m.type = (i < 2) ? MINION_TYPE_MELEE : MINION_TYPE_RANGED;
            
            m.hp = (m.type == MINION_TYPE_MELEE) ? MELEE_HP : RANGED_HP; m.max_hp = m.hp;
            m.dmg = (m.type == MINION_TYPE_MELEE) ? MELEE_DMG : RANGED_DMG;
            m.range = (m.type == MINION_TYPE_MELEE) ? MELEE_RANGE : RANGED_RANGE;
            
            m.x = 128 + rand()%2; m.y = 22 + rand()%2; 
            const std::vector<Point>* path = (lane == 0) ? &PATH_TOP : ((lane == 1) ? &PATH_MID : &PATH_BOT);
            m.wp_idx = path->size() - 1; m.state = Minion::MARCHING;
            m.last_attack_time = 0;
            minions[m.id] = m;
        }
    }
}

// === 小兵 AI ===
void update_minions() {
    std::vector<int> dead_ids;
    long long now = get_current_ms();

    for (auto& pair : minions) {
        Minion& m = pair.second;
        // 血量判定：可被击杀
        if (m.hp <= 0) { dead_ids.push_back(m.id); continue; }

        if (m.state == Minion::MARCHING) {
            // 1. 索敌
            int min_d = 9999, found = 0;
            int vision_sq = MINION_VISION_RANGE * MINION_VISION_RANGE;

            for (auto& p : players) {
                if (!p.second.is_playing || p.second.color == m.team) continue;
                int d = dist_sq((int)m.x, (int)m.y, p.second.x, p.second.y);
                if (d <= vision_sq && d < min_d) { min_d = d; found = p.second.id; }
            }
            if (found == 0) {
                for (auto& em : minions) {
                    if (em.second.team == m.team) continue;
                    int d = dist_sq((int)m.x, (int)m.y, (int)em.second.x, (int)em.second.y);
                    if (d <= vision_sq && d < min_d) { min_d = d; found = em.second.id; }
                }
            }

            if (found != 0) {
                m.state = Minion::CHASING; m.target_id = found;
                m.anchor_x = m.x; m.anchor_y = m.y;
            } else {
                // 移动
                const std::vector<Point>* path = (m.lane == 0) ? &PATH_TOP : ((m.lane == 1) ? &PATH_MID : &PATH_BOT);
                Point target = (*path)[m.wp_idx];
                float dx = target.x - m.x, dy = target.y - m.y;
                float dist = sqrt(dx*dx + dy*dy);

                if (dist < 2.0f) {
                    if (m.team == 1 && m.wp_idx < path->size() - 1) m.wp_idx++;
                    else if (m.team == 2 && m.wp_idx > 0) m.wp_idx--;
                } else {
                    m.x += (dx/dist) * MINION_MOVE_SPEED; m.y += (dy/dist) * MINION_MOVE_SPEED;
                }
            }
        }
        else if (m.state == Minion::CHASING) {
            // 2. 追击/攻击
            int dist_anchor = dist_sq((int)m.x, (int)m.y, (int)m.anchor_x, (int)m.anchor_y);
            if (dist_anchor > MINION_CHASE_LIMIT * MINION_CHASE_LIMIT) {
                m.state = Minion::RETURNING; m.target_id = 0;
            } else {
                int tx = 0, ty = 0; bool exists = false;
                if (m.target_id < MINION_ID_START) {
                    for(auto& p : players) if (p.second.id == m.target_id && p.second.hp > 0) {
                        tx = p.second.x; ty = p.second.y; exists = true; break;
                    }
                } else if (minions.count(m.target_id) && minions[m.target_id].hp > 0) {
                    tx = (int)minions[m.target_id].x; ty = (int)minions[m.target_id].y; exists = true;
                }

                if (!exists) m.state = Minion::RETURNING;
                else {
                    int dist_target = dist_sq((int)m.x, (int)m.y, tx, ty);
                    if (dist_target <= m.range * m.range) {
                        // 攻击冷却
                        if (now - m.last_attack_time >= MINION_ATK_COOLDOWN) {
                            m.last_attack_time = now;
                            if (m.target_id < MINION_ID_START) {
                                for(auto& p : players) if(p.second.id == m.target_id) p.second.hp -= m.dmg;
                            } else minions[m.target_id].hp -= m.dmg;
                        }
                    } else {
                        // 移动
                        float dx = tx - m.x, dy = ty - m.y;
                        float dist = sqrt(dx*dx + dy*dy);
                        m.x += (dx/dist) * MINION_MOVE_SPEED; m.y += (dy/dist) * MINION_MOVE_SPEED;
                    }
                }
            }
        }
        else if (m.state == Minion::RETURNING) {
            // 3. 归位
            float dx = m.anchor_x - m.x, dy = m.anchor_y - m.y;
            float dist = sqrt(dx*dx + dy*dy);
            if (dist < 1.0f) m.state = Minion::MARCHING;
            else {
                // 回归加速
                m.x += (dx/dist) * (MINION_MOVE_SPEED * 2.0f);
                m.y += (dy/dist) * (MINION_MOVE_SPEED * 2.0f);
            }
        }
    }
    for (int id : dead_ids) minions.erase(id);
}

// === 玩家攻击 ===
bool handle_attack(int attacker_fd) {
    if (players.count(attacker_fd) == 0) return false;
    Player& att = players[attacker_fd];
    if (hero_db.count(att.hero_id) == 0) return false;
    HeroTemplate& tmpl = hero_db[att.hero_id];

    bool hit = false;
    int range_sq = (tmpl.attack_range + 1) * (tmpl.attack_range + 1);
    int target_id = 0, min_dist = 9999;

    // 搜玩家
    for (auto& p : players) {
        if (p.second.id == att.id || p.second.color == att.color || !p.second.is_playing) continue;
        int d = dist_sq(att.x, att.y, p.second.x, p.second.y);
        if (d <= range_sq && d < min_dist) { min_dist = d; target_id = p.second.id; }
    }
    // 搜小兵
    for (auto& m : minions) {
        if (m.second.team == att.color) continue;
        int d = dist_sq(att.x, att.y, (int)m.second.x, (int)m.second.y);
        if (d <= range_sq && d < min_dist) { min_dist = d; target_id = m.second.id; }
    }

    if (target_id != 0) {
        att.current_target_id = target_id; hit = true;
        if (target_id < MINION_ID_START) {
            for(auto& p : players) if(p.second.id == target_id) {
                p.second.hp -= tmpl.attack_dmg;
                p.second.current_effect = EFFECT_HIT;
                if (p.second.hp <= 0) {
                    p.second.hp = p.second.max_hp;
                    p.second.x = (p.second.color == 1) ? 22 : 128;
                    p.second.y = (p.second.color == 1) ? 128 : 22;
                    std::cout << "[Kill] Player " << p.second.id << " died." << std::endl;
                }
                break;
            }
        } else {
            // [玩家击杀小兵]
            if (minions.count(target_id)) minions[target_id].hp -= tmpl.attack_dmg;
        }
    }
    return hit;
}

void broadcast_world() {
    std::vector<GamePacket> updates;
    for (auto& sp : players) {
        if (!sp.second.is_playing) continue;
        Player& p = sp.second;
        int range = hero_db[p.hero_id].attack_range;
        // 玩家包：input字段存英雄ID
        GamePacket pkt = { TYPE_UPDATE, p.id, p.x, p.y, p.hero_id, 0, p.color, p.hp, p.max_hp, range, p.current_effect, p.current_target_id };
        updates.push_back(pkt);
    }
    for (auto& pair : minions) {
        Minion& m = pair.second;
        // 小兵包：input字段存小兵类型 (1或2)，让客户端能区分o和i
        // 之前的错误是把type存到了extra，导致客户端读不到
        GamePacket pkt = { TYPE_UPDATE, m.id, (int)m.x, (int)m.y, m.type, 0, m.team, m.hp, m.max_hp, 0, 0, m.target_id };
        updates.push_back(pkt);
    }

    GamePacket end_pkt; memset(&end_pkt, 0, sizeof(end_pkt)); 
    end_pkt.type = TYPE_FRAME;
    end_pkt.extra = (int)((get_current_ms() - game_start_time) / 1000);

    for (auto& pair : players) {
        int fd = pair.first;
        if (!pair.second.is_playing) continue;
        for(const auto& pkt : updates) write(fd, &pkt, sizeof(pkt));
        write(fd, &end_pkt, sizeof(end_pkt));
    }
    for (auto& pair : players) { pair.second.current_effect = EFFECT_NONE; pair.second.current_target_id = 0; }
}

int main() {
    MapGenerator::init(game_map);
    game_start_time = get_current_ms(); // 初始化时间

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in address = {AF_INET, htons(PORT), INADDR_ANY};
    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, SOMAXCONN);
    
    int epoll_fd = epoll_create1(0);
    epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN; event.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    std::cout << "MOBA Server Started." << std::endl;

    while (true) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 50);
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
                                int team = (p.id % 2 != 0) ? 1 : 2;
                                int sx = (team == 1) ? 22 : 128, sy = (team == 1) ? 128 : 22;
                                p = {fd, p.id, sx, sy, tmpl.base_hp, tmpl.base_hp, tmpl.id, team, true};
                                need_broadcast = true;
                            }
                        } else if (p.is_playing) {
                            if (pkt.type == TYPE_MOVE) {
                                int dx = pkt.x, dy = pkt.y;
                                if (dx < -1) dx = -1; if (dx > 1) dx = 1;
                                if (dy < -1) dy = -1; if (dy > 1) dy = 1;
                                int nx = p.x + dx, ny = p.y + dy;
                                if (is_valid_move(nx, ny)) { p.x = nx; p.y = ny; need_broadcast = true; }
                            }
                            else if (pkt.type == TYPE_ATTACK) { if (handle_attack(fd)) need_broadcast = true; }
                            else if (pkt.type == TYPE_SPELL) { p.hp += 100; if(p.hp > p.max_hp) p.hp = p.max_hp; need_broadcast = true; }
                        }
                    }
                }
            }
        }

        // 核心循环更新
        long long now = get_current_ms();
        int current_sec = (now - game_start_time) / 1000;
        
        // 出兵逻辑：检测 x:30
        if (current_sec >= 30 && (current_sec - 30) % 60 == 0) {
            if (current_sec != last_spawn_minute) { 
                spawn_wave();
                last_spawn_minute = current_sec;
                need_broadcast = true;
            }
        }

        if (!minions.empty()) {
            update_minions();
            need_broadcast = true; 
        }
        if (need_broadcast) broadcast_world();
    }
    return 0;
}