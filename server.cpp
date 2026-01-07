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

// === 数据结构定义 (前置以供 is_valid_move 使用) ===
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

struct Tower {
    int id;
    int x, y;
    int team; 
    int hp, max_hp;
    int tier;
};

struct Point { int x, y; };
const std::vector<Point> PATH_TOP = { {22, 128}, {22, 22},  {128, 22} };  
const std::vector<Point> PATH_MID = { {22, 128}, {75, 75},  {128, 22} };  
const std::vector<Point> PATH_BOT = { {22, 128}, {128, 128},{128, 22} };  

struct Minion {
    int id;
    int team; int type; float x, y; int hp, max_hp; int dmg, range;
    int lane; int wp_idx; 
    enum State { MARCHING, CHASING, RETURNING }; State state;
    int target_id; float anchor_x, anchor_y; long long last_attack_time; 
};

// 容器
std::map<int, Player> players;
std::map<int, Minion> minions;
std::map<int, Tower> towers; 

int global_id_counter = 1;
int tower_id_counter = TOWER_ID_START;
int minion_id_counter = MINION_ID_START;
int last_spawn_minute = -1; 

// === 物理判定 (核心修改) ===
bool is_valid_move(int x, int y) {
    if (x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE) return false;
    int t = game_map[y][x];
    
    // 1. 绝对阻挡：墙壁
    if (t == TILE_WALL) return false;

    // 2. 基座 (TILE_TOWER_WALL = 10)：现在允许通过！
    if (t == TILE_TOWER_WALL) return true;

    // 3. 塔核心 (11-23)：活的时候阻挡，死的时候通过
    if (t >= 11 && t <= 23) {
        // 在 towers 容器里找这个坐标的塔
        // 这种遍历效率略低，但考虑到地图只有150x150且塔少，可以接受
        // 更优解是建立坐标索引，但为了不改动太多数据结构，这里用遍历
        for (auto& pair : towers) {
            if (pair.second.x == x && pair.second.y == y) {
                if (pair.second.hp > 0) return false; // 活着，撞墙
                else return true; // 死了，通过
            }
        }
        // 如果没找到塔实例(理论上不可能)，默认阻挡保底
        return false;
    }

    return true; // 空地、河道、水晶等默认通过
}

int setNonBlocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    return fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

void init_towers() {
    for(int y=0; y<MAP_SIZE; y++) {
        for(int x=0; x<MAP_SIZE; x++) {
            int t = game_map[y][x];
            if (t >= 11 && t <= 23) {
                Tower tower;
                tower.id = tower_id_counter++;
                tower.x = x; tower.y = y;
                if (t >= 11 && t <= 13) {
                    tower.team = 1;
                    if (t==11) tower.max_hp = TOWER_HP_TIER_1;
                    else if (t==12) tower.max_hp = TOWER_HP_TIER_2;
                    else tower.max_hp = TOWER_HP_TIER_3;
                } else {
                    tower.team = 2;
                    if (t==21) tower.max_hp = TOWER_HP_TIER_1;
                    else if (t==22) tower.max_hp = TOWER_HP_TIER_2;
                    else tower.max_hp = TOWER_HP_TIER_3;
                }
                tower.hp = tower.max_hp;
                towers[tower.id] = tower;
            }
        }
    }
}

void spawn_wave() {
    std::cout << "[System] Wave Spawned." << std::endl;
    for (int lane = 0; lane < 3; lane++) {
        for(int i=0; i<3; i++) {
            Minion m;
            m.id = minion_id_counter++; m.team = 1; m.lane = lane;
            m.type = (i < 2) ? MINION_TYPE_MELEE : MINION_TYPE_RANGED;
            m.hp = (m.type == 1) ? MELEE_HP : RANGED_HP; m.max_hp = m.hp;
            m.dmg = (m.type == 1) ? MELEE_DMG : RANGED_DMG;
            m.range = (m.type == 1) ? MELEE_RANGE : RANGED_RANGE;
            m.x = 22 + rand()%2; m.y = 128 + rand()%2; 
            m.wp_idx = 0; m.state = Minion::MARCHING; m.last_attack_time = 0;
            minions[m.id] = m;
        }
        for(int i=0; i<3; i++) {
            Minion m;
            m.id = minion_id_counter++; m.team = 2; m.lane = lane;
            m.type = (i < 2) ? MINION_TYPE_MELEE : MINION_TYPE_RANGED;
            m.hp = (m.type == 1) ? MELEE_HP : RANGED_HP; m.max_hp = m.hp;
            m.dmg = (m.type == 1) ? MELEE_DMG : RANGED_DMG;
            m.range = (m.type == 1) ? MELEE_RANGE : RANGED_RANGE;
            m.x = 128 + rand()%2; m.y = 22 + rand()%2; 
            const std::vector<Point>* path = (lane == 0) ? &PATH_TOP : ((lane == 1) ? &PATH_MID : &PATH_BOT);
            m.wp_idx = path->size() - 1; m.state = Minion::MARCHING; m.last_attack_time = 0;
            minions[m.id] = m;
        }
    }
}

void update_minions() {
    std::vector<int> dead_ids;
    long long now = get_current_ms();

    for (auto& pair : minions) {
        Minion& m = pair.second;
        if (m.hp <= 0) { dead_ids.push_back(m.id); continue; }

        if (m.state == Minion::MARCHING) {
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
            if (found == 0) {
                for (auto& t : towers) {
                    if (t.second.team == m.team || t.second.hp <= 0) continue;
                    int d = dist_sq((int)m.x, (int)m.y, t.second.x, t.second.y);
                    if (d <= (MINION_VISION_RANGE+2)*(MINION_VISION_RANGE+2) && d < min_d) { 
                        min_d = d; found = t.second.id; 
                    }
                }
            }

            if (found != 0) {
                m.state = Minion::CHASING; m.target_id = found;
                m.anchor_x = m.x; m.anchor_y = m.y;
            } else {
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
            int dist_anchor = dist_sq((int)m.x, (int)m.y, (int)m.anchor_x, (int)m.anchor_y);
            if (dist_anchor > MINION_CHASE_LIMIT * MINION_CHASE_LIMIT) {
                m.state = Minion::RETURNING; m.target_id = 0;
            } else {
                int tx = 0, ty = 0; bool exists = false;
                if (m.target_id < 100) { 
                    for(auto& p : players) if (p.second.id == m.target_id && p.second.hp > 0) {
                        tx = p.second.x; ty = p.second.y; exists = true; break;
                    }
                } else if (m.target_id >= MINION_ID_START) {
                    if (minions.count(m.target_id) && minions[m.target_id].hp > 0) {
                        tx = (int)minions[m.target_id].x; ty = (int)minions[m.target_id].y; exists = true;
                    }
                } else { 
                    if (towers.count(m.target_id) && towers[m.target_id].hp > 0) {
                        tx = towers[m.target_id].x; ty = towers[m.target_id].y; exists = true;
                    }
                }

                if (!exists) m.state = Minion::RETURNING;
                else {
                    int dist_target = dist_sq((int)m.x, (int)m.y, tx, ty);
                    int atk_range_bonus = (m.target_id >= 100 && m.target_id < 1000) ? 2 : 0;
                    int atk_sq = (m.range + atk_range_bonus) * (m.range + atk_range_bonus);

                    if (dist_target <= atk_sq) {
                        if (now - m.last_attack_time >= MINION_ATK_COOLDOWN) {
                            m.last_attack_time = now;
                            if (m.target_id < 100) {
                                for(auto& p : players) if(p.second.id == m.target_id) p.second.hp -= m.dmg;
                            } else if (m.target_id >= MINION_ID_START) {
                                minions[m.target_id].hp -= m.dmg;
                            } else {
                                towers[m.target_id].hp -= m.dmg; 
                            }
                        }
                    } else {
                        float dx = tx - m.x, dy = ty - m.y;
                        float dist = sqrt(dx*dx + dy*dy);
                        m.x += (dx/dist) * MINION_MOVE_SPEED; m.y += (dy/dist) * MINION_MOVE_SPEED;
                    }
                }
            }
        }
        else if (m.state == Minion::RETURNING) {
            float dx = m.anchor_x - m.x, dy = m.anchor_y - m.y;
            float dist = sqrt(dx*dx + dy*dy);
            if (dist < 1.0f) m.state = Minion::MARCHING;
            else { m.x += (dx/dist) * (MINION_MOVE_SPEED * 2.0f); m.y += (dy/dist) * (MINION_MOVE_SPEED * 2.0f); }
        }
    }
    for (int id : dead_ids) minions.erase(id);
}

bool handle_attack(int attacker_fd) {
    if (players.count(attacker_fd) == 0) return false;
    Player& att = players[attacker_fd];
    if (hero_db.count(att.hero_id) == 0) return false;
    HeroTemplate& tmpl = hero_db[att.hero_id];

    bool hit = false;
    int range_sq = (tmpl.attack_range + 1) * (tmpl.attack_range + 1);
    int target_id = 0, min_dist = 9999;

    for (auto& p : players) {
        if (p.second.id == att.id || p.second.color == att.color || !p.second.is_playing) continue;
        int d = dist_sq(att.x, att.y, p.second.x, p.second.y);
        if (d <= range_sq && d < min_dist) { min_dist = d; target_id = p.second.id; }
    }
    for (auto& m : minions) {
        if (m.second.team == att.color) continue;
        int d = dist_sq(att.x, att.y, (int)m.second.x, (int)m.second.y);
        if (d <= range_sq && d < min_dist) { min_dist = d; target_id = m.second.id; }
    }
    for (auto& t : towers) {
        if (t.second.team == att.color || t.second.hp <= 0) continue;
        int d = dist_sq(att.x, att.y, t.second.x, t.second.y);
        if (d <= range_sq + 10 && d < min_dist) { min_dist = d; target_id = t.second.id; }
    }

    if (target_id != 0) {
        att.current_target_id = target_id; hit = true;
        if (target_id < 100) {
            for(auto& p : players) if(p.second.id == target_id) {
                p.second.hp -= tmpl.attack_dmg; p.second.current_effect = EFFECT_HIT;
                if (p.second.hp <= 0) {
                    p.second.hp = p.second.max_hp;
                    p.second.x = (p.second.color == 1) ? 22 : 128;
                    p.second.y = (p.second.color == 1) ? 128 : 22;
                    std::cout << "Player died." << std::endl;
                }
                break;
            }
        } else if (target_id >= MINION_ID_START) {
            if (minions.count(target_id)) minions[target_id].hp -= tmpl.attack_dmg;
        } else {
            if (towers.count(target_id)) towers[target_id].hp -= tmpl.attack_dmg;
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
        GamePacket pkt = { TYPE_UPDATE, p.id, p.x, p.y, p.hero_id, 0, p.color, p.hp, p.max_hp, range, p.current_effect, p.current_target_id };
        updates.push_back(pkt);
    }
    for (auto& pair : towers) {
        Tower& t = pair.second;
        // [关键] 只有活着的塔才发包，死了就不发，客户端就不画了
        if (t.hp > 0) { 
            GamePacket pkt = { TYPE_UPDATE, t.id, t.x, t.y, 0, 0, t.team, t.hp, t.max_hp, 0, 0, 0 };
            updates.push_back(pkt);
        }
    }
    for (auto& pair : minions) {
        Minion& m = pair.second;
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
    init_towers(); 
    game_start_time = get_current_ms();

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

        long long now = get_current_ms();
        int current_sec = (now - game_start_time) / 1000;
        
        if (current_sec >= 30 && (current_sec - 30) % 60 == 0) {
            if (current_sec != last_spawn_minute) { 
                spawn_wave();
                last_spawn_minute = current_sec;
                need_broadcast = true;
            }
        }

        if (!minions.empty()) { update_minions(); need_broadcast = true; }
        if (need_broadcast) broadcast_world();
    }
    return 0;
}