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
int wave_count = 0; 

// 物理判定
bool is_valid_move(int x, int y) {
    if (x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE) return false;
    int t = game_map[y][x];
    return !(t == TILE_WALL); 
}

// === 数据结构 ===
struct HeroTemplate {
    int id; int base_hp; int attack_range; int attack_dmg; int color; const char* name;
};

std::map<int, HeroTemplate> hero_db = {
    {HERO_WARRIOR, {HERO_WARRIOR, HERO_HP_DEFAULT, 2, HERO_DMG_DEFAULT, 1, "Warrior"}}, 
    {HERO_MAGE,    {HERO_MAGE,    HERO_HP_DEFAULT, 6, HERO_DMG_DEFAULT, 2, "Mage"}},    
    {HERO_TANK,    {HERO_TANK,    HERO_HP_DEFAULT, 2, HERO_DMG_DEFAULT, 3, "Tank"}}     
};

struct Player {
    int fd, id, x, y, hp, max_hp, hero_id, color;
    bool is_playing;
    int current_effect;    
    int current_target_id; 
    long long last_aggressive_time; 
    long long visual_end_time;      
};

struct Tower {
    int id, x, y, team; 
    int hp, max_hp;
    int target_id;              
    int consecutive_hits;       
    long long last_attack_time; 
    long long visual_end_time;  
};

struct Point { int x, y; };
const std::vector<Point> PATH_TOP = { {22, 128}, {22, 22},  {128, 22} };  
const std::vector<Point> PATH_MID = { {22, 128}, {75, 75},  {128, 22} };  
const std::vector<Point> PATH_BOT = { {22, 128}, {128, 128},{128, 22} };  

struct Minion {
    int id, team, type; float x, y; int hp, max_hp, dmg, range;
    int lane, wp_idx; 
    enum State { MARCHING, CHASING, RETURNING }; State state;
    int target_id; float anchor_x, anchor_y; 
    long long last_attack_time;
    long long visual_end_time; 
};

// 容器
std::map<int, Player> players; // Key是FD，不是PlayerID！
std::map<int, Minion> minions;
std::map<int, Tower> towers; 

int global_id_counter = 1;
int tower_id_counter = TOWER_ID_START;
int minion_id_counter = MINION_ID_START;
int last_spawn_minute = -1; 

// [修复] 通过ID查找玩家的辅助函数
Player* get_player_by_id(int id) {
    for (auto& pair : players) {
        if (pair.second.id == id && pair.second.is_playing) {
            return &pair.second;
        }
    }
    return nullptr;
}

// 物理判定补充
bool is_blocked_by_tower(int x, int y) {
    if (!is_valid_move(x, y)) return true;
    int t = game_map[y][x];
    // 阻挡逻辑：如果该位置有塔或者有水晶，且血量大于0，则阻挡
    if ((t >= 11 && t <= 23) || t == TILE_BASE) {
        for (auto& pair : towers) {
            if (pair.second.x == x && pair.second.y == y) return (pair.second.hp > 0);
        }
    }
    return false;
}

int setNonBlocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    return fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

// [修改] 初始化塔（包含水晶）
void init_towers() {
    for(int y=0; y<MAP_SIZE; y++) {
        for(int x=0; x<MAP_SIZE; x++) {
            int t = game_map[y][x];
            
            // [新增] 判断条件加入 TILE_BASE (2)
            // 使得水晶被视为一个 Tower 实体，拥有血量和攻击AI
            if ((t >= 11 && t <= 23) || t == TILE_BASE) {
                Tower tower; 
                tower.id = tower_id_counter++; 
                tower.x = x; 
                tower.y = y;
                tower.target_id = 0; 
                tower.consecutive_hits = 0; 
                tower.last_attack_time = 0; 
                tower.visual_end_time = 0;

                if (t == TILE_BASE) {
                    // 水晶逻辑
                    // 假设左侧(x<75)是蓝方基地(Team 1)，右侧是红方(Team 2)
                    tower.team = (x < 75) ? 1 : 2;
                    tower.max_hp = 10000; // [设置] 水晶血量为 1w
                } 
                else {
                    // 原有防御塔逻辑
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
                }
                
                tower.hp = tower.max_hp; 
                towers[tower.id] = tower;
            }
        }
    }
}

// === 出兵逻辑 ===
void spawn_wave() {
    wave_count++; 
    std::cout << "[System] Wave " << wave_count << " Spawned." << std::endl;
    
    int melee_hp = MELEE_BASE_HP + 200 * wave_count;
    int melee_dmg = MELEE_BASE_DMG + 150 * wave_count; 

    int ranged_hp = RANGED_BASE_HP + 150 * wave_count;
    int ranged_dmg = RANGED_BASE_DMG + 200 * wave_count; 

    for (int lane = 0; lane < 3; lane++) {
        for(int i=0; i<3; i++) {
            Minion m; m.id = minion_id_counter++; m.team = 1; m.lane = lane;
            m.type = (i < 2) ? MINION_TYPE_MELEE : MINION_TYPE_RANGED;
            m.hp = (m.type == 1) ? melee_hp : ranged_hp; m.max_hp = m.hp;
            m.dmg = (m.type == 1) ? melee_dmg : ranged_dmg;
            m.range = (m.type == 1) ? MELEE_RANGE : RANGED_RANGE;
            m.x = 22 + rand()%2; m.y = 128 + rand()%2; 
            m.wp_idx = 0; m.state = Minion::MARCHING; m.last_attack_time = 0; m.visual_end_time = 0;
            minions[m.id] = m;
        }
        for(int i=0; i<3; i++) {
            Minion m; m.id = minion_id_counter++; m.team = 2; m.lane = lane;
            m.type = (i < 2) ? MINION_TYPE_MELEE : MINION_TYPE_RANGED;
            m.hp = (m.type == 1) ? melee_hp : ranged_hp; m.max_hp = m.hp;
            m.dmg = (m.type == 1) ? melee_dmg : ranged_dmg;
            m.range = (m.type == 1) ? MELEE_RANGE : RANGED_RANGE;
            m.x = 128 + rand()%2; m.y = 22 + rand()%2; 
            const std::vector<Point>* path = (lane == 0) ? &PATH_TOP : ((lane == 1) ? &PATH_MID : &PATH_BOT);
            m.wp_idx = path->size() - 1; m.state = Minion::MARCHING; m.last_attack_time = 0; m.visual_end_time = 0;
            minions[m.id] = m;
        }
    }
}

// === 防御塔逻辑 (修复版) ===
void update_towers() {
    long long now = get_current_ms();
    int tower_range_sq = TOWER_ATK_RANGE * TOWER_ATK_RANGE;

    for (auto& pair : towers) {
        Tower& t = pair.second;
        if (t.hp <= 0) continue;

        // 1. 寻找目标
        int best_target = 0;
        int min_dist = 99999;

        // A. 仇恨优先
        for (auto& p : players) {
            Player& enemy = p.second;
            if (!enemy.is_playing || enemy.color == t.team) continue;
            int d = dist_sq(t.x, t.y, enemy.x, enemy.y);
            if (d > tower_range_sq) continue;

            if (now - enemy.last_aggressive_time < 2000) {
                best_target = enemy.id;
                break; 
            }
        }

        // B. 维持当前目标
        if (best_target == 0 && t.target_id != 0) {
            bool valid = false;
            int tx=0, ty=0;
            Player* p = get_player_by_id(t.target_id);
            if (p) { tx = p->x; ty = p->y; valid = true; }
            else if (minions.count(t.target_id)) {
                Minion& m = minions[t.target_id];
                if (m.hp > 0) { tx = (int)m.x; ty = (int)m.y; valid = true; }
            }
            if (valid && dist_sq(t.x, t.y, tx, ty) <= tower_range_sq) {
                best_target = t.target_id;
            }
        }

        // C. 最近敌人
        if (best_target == 0) {
            for (auto& m : minions) {
                if (m.second.team == t.team) continue;
                int d = dist_sq(t.x, t.y, (int)m.second.x, (int)m.second.y);
                if (d <= tower_range_sq && d < min_dist) { min_dist = d; best_target = m.second.id; }
            }
            if (best_target == 0) {
                for (auto& p : players) {
                    if (!p.second.is_playing || p.second.color == t.team) continue;
                    int d = dist_sq(t.x, t.y, p.second.x, p.second.y);
                    if (d <= tower_range_sq && d < min_dist) { min_dist = d; best_target = p.second.id; }
                }
            }
        }

        if (best_target != t.target_id) {
            t.consecutive_hits = 0; 
            t.target_id = best_target;
        }

        if (t.target_id != 0) {
            if (now - t.last_attack_time >= TOWER_ATK_COOLDOWN) {
                t.last_attack_time = now;
                t.visual_end_time = now + 200; 

                int damage = 0;
                Player* p = get_player_by_id(t.target_id);
                if (p) { // 打人
                    damage = TOWER_BASE_DMG_HERO * (int)pow(2, t.consecutive_hits);
                    t.consecutive_hits++; 
                    
                    p->hp -= damage;
                    p->current_effect = EFFECT_HIT;
                    std::cout << "Tower Hit Player " << p->id << " HP:" << p->hp << std::endl;

                    if(p->hp <= 0) {
                        p->hp = p->max_hp;
                        p->x = (p->color == 1) ? 22 : 128; p->y = (p->color == 1) ? 128 : 22;
                        t.target_id = 0; t.consecutive_hits = 0;
                    }
                } 
                else if (minions.count(t.target_id)) { // 打兵
                    damage = TOWER_BASE_DMG_MINION + 100 * wave_count;
                    minions[t.target_id].hp -= damage;
                }
            }
        } else {
            t.consecutive_hits = 0;
        }
    }
}

// === 小兵 AI (修复版) ===
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
                    // 视野范围稍微大一点，方便锁定塔
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
                Player* p = get_player_by_id(m.target_id);
                if (p) { tx = p->x; ty = p->y; exists = true; }
                else if (minions.count(m.target_id) && minions[m.target_id].hp > 0) {
                    tx = (int)minions[m.target_id].x; ty = (int)minions[m.target_id].y; exists = true;
                } else if (towers.count(m.target_id) && towers[m.target_id].hp > 0) {
                    tx = towers[m.target_id].x; ty = towers[m.target_id].y; exists = true;
                }

                if (!exists) m.state = Minion::RETURNING;
                else {
                    int dist_target = dist_sq((int)m.x, (int)m.y, tx, ty);
                    int atk_range_bonus = (m.target_id >= 100 && m.target_id < 1000) ? 2 : 0;
                    if (dist_target <= (m.range + atk_range_bonus) * (m.range + atk_range_bonus)) {
                        if (now - m.last_attack_time >= MINION_ATK_COOLDOWN) {
                            m.last_attack_time = now;
                            m.visual_end_time = now + 200; 
                            // [修复] 扣血
                            if (p) p->hp -= m.dmg;
                            else if (minions.count(m.target_id)) minions[m.target_id].hp -= m.dmg;
                            else if (towers.count(m.target_id)) towers[m.target_id].hp -= m.dmg; 
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

// === 玩家攻击 (修复版) ===
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
        // 塔的碰撞体积稍微大一点
        if (d <= range_sq + 10 && d < min_dist) { min_dist = d; target_id = t.second.id; }
    }

    if (target_id != 0) {
        att.current_target_id = target_id; 
        att.visual_end_time = get_current_ms() + 200; 
        hit = true;

        Player* p = get_player_by_id(target_id);
        if (p) {
            p->hp -= tmpl.attack_dmg; 
            p->current_effect = EFFECT_HIT;
            att.last_aggressive_time = get_current_ms(); // 产生仇恨
            if (p->hp <= 0) {
                p->hp = p->max_hp;
                p->x = (p->color == 1) ? 22 : 128;
                p->y = (p->color == 1) ? 128 : 22;
                std::cout << "[Kill] Player " << p->id << " died." << std::endl;
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
    long long now = get_current_ms();
    std::vector<GamePacket> updates;
    
    // Player
    for (auto& sp : players) {
        if (!sp.second.is_playing) continue;
        Player& p = sp.second;
        int range = hero_db[p.hero_id].attack_range;
        int atk_target = (now < p.visual_end_time) ? p.current_target_id : 0;
        GamePacket pkt = { TYPE_UPDATE, p.id, p.x, p.y, p.hero_id, 0, p.color, p.hp, p.max_hp, range, p.current_effect, atk_target };
        updates.push_back(pkt);
    }
    // Tower
    for (auto& pair : towers) {
        Tower& t = pair.second;
        if (t.hp > 0) { 
            int atk_target = (now < t.visual_end_time) ? t.target_id : 0;
            GamePacket pkt = { TYPE_UPDATE, t.id, t.x, t.y, 0, 0, t.team, t.hp, t.max_hp, 0, 0, atk_target };
            updates.push_back(pkt);
        }
    }
    // Minion
    for (auto& pair : minions) {
        Minion& m = pair.second;
        int atk_target = (now < m.visual_end_time) ? m.target_id : 0;
        GamePacket pkt = { TYPE_UPDATE, m.id, (int)m.x, (int)m.y, m.type, 0, m.team, m.hp, m.max_hp, 0, 0, atk_target };
        updates.push_back(pkt);
    }

    GamePacket end_pkt; memset(&end_pkt, 0, sizeof(end_pkt)); 
    end_pkt.type = TYPE_FRAME;
    end_pkt.extra = (int)((now - game_start_time) / 1000);

    for (auto& pair : players) {
        int fd = pair.first;
        if (!pair.second.is_playing) continue;
        for(const auto& pkt : updates) write(fd, &pkt, sizeof(pkt));
        write(fd, &end_pkt, sizeof(end_pkt));
    }
    for (auto& pair : players) { pair.second.current_effect = EFFECT_NONE; }
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
                                if (!is_blocked_by_tower(nx, ny)) { p.x = nx; p.y = ny; need_broadcast = true; }
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

        update_towers(); 
        if (!minions.empty()) { update_minions(); need_broadcast = true; }
        if (need_broadcast) broadcast_world();
    }
    return 0;
}