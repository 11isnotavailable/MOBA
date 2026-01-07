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

struct SkillEffect {
    int x, y;
    int type; 
    long long start_time;
    long long end_time;
    int radius;
    int owner_id; 
};

struct JungleMonster {
    int id, type; 
    int x, y; 
    int hp, max_hp, dmg, range;
    int target_id; 
    long long last_hit_by_time; 
    long long last_attack_time; 
    long long last_regen_time;  
    long long visual_end_time;  

    int attack_counter;         
    enum BossState { IDLE, CASTING_PREPARE, CASTING_ACTIVE }; 
    BossState boss_state;
    long long skill_start_time; 
    long long next_tick_time;   
    std::vector<Point> skill_targets; 
};

std::map<int, Player> players; 
std::map<int, Minion> minions;
std::map<int, Tower> towers; 
std::map<int, JungleMonster> jungle_mobs; 
std::vector<SkillEffect> active_effects; 

int global_id_counter = 1;
int tower_id_counter = TOWER_ID_START;
int minion_id_counter = MINION_ID_START;
int jungle_id_counter = JUNGLE_ID_START; 
int boss_id_counter = BOSS_ID_START;
int last_spawn_minute = -1; 

Player* get_player_by_id(int id) {
    for (auto& pair : players) {
        if (pair.second.id == id && pair.second.is_playing) return &pair.second;
    }
    return nullptr;
}

bool is_blocked_by_tower(int x, int y) {
    if (!is_valid_move(x, y)) return true;
    int t = game_map[y][x];
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

void init_towers() {
    for(int y=0; y<MAP_SIZE; y++) {
        for(int x=0; x<MAP_SIZE; x++) {
            int t = game_map[y][x];
            if ((t >= 11 && t <= 23) || t == TILE_BASE) {
                Tower tower; 
                tower.id = tower_id_counter++; tower.x = x; tower.y = y;
                tower.target_id = 0; tower.consecutive_hits = 0; 
                tower.last_attack_time = 0; tower.visual_end_time = 0;
                if (t == TILE_BASE) {
                    tower.team = (x < 75) ? 1 : 2; tower.max_hp = 10000; 
                } else {
                    if (t >= 11 && t <= 13) {
                        tower.team = 1;
                        if (t==11) tower.max_hp = TOWER_HP_TIER_1; else if (t==12) tower.max_hp = TOWER_HP_TIER_2; else tower.max_hp = TOWER_HP_TIER_3;
                    } else {
                        tower.team = 2;
                        if (t==21) tower.max_hp = TOWER_HP_TIER_1; else if (t==22) tower.max_hp = TOWER_HP_TIER_2; else tower.max_hp = TOWER_HP_TIER_3;
                    }
                }
                tower.hp = tower.max_hp; towers[tower.id] = tower;
            }
        }
    }
}

void init_jungle() {
    // [修改] Boss位置向中心迁移
    // 主宰 (Overlord): 原(35,35) -> 向中心移动20格 -> (55, 55)
    JungleMonster overlord;
    overlord.id = boss_id_counter++;
    overlord.type = BOSS_TYPE_OVERLORD;
    overlord.x = 55; overlord.y = 55; // [Updated]
    overlord.hp = OVERLORD_HP; overlord.max_hp = OVERLORD_HP;
    overlord.dmg = OVERLORD_DMG; overlord.range = OVERLORD_RANGE;
    overlord.target_id = 0; overlord.last_hit_by_time = 0;
    overlord.last_attack_time = 0; overlord.last_regen_time = 0;
    overlord.attack_counter = 0; overlord.boss_state = JungleMonster::IDLE;
    jungle_mobs[overlord.id] = overlord;

    // 暴君 (Tyrant): 原(115,115) -> 向中心移动20格 -> (95, 95)
    JungleMonster tyrant;
    tyrant.id = boss_id_counter++;
    tyrant.type = BOSS_TYPE_TYRANT;
    tyrant.x = 95; tyrant.y = 95; // [Updated]
    tyrant.hp = TYRANT_HP; tyrant.max_hp = TYRANT_HP;
    tyrant.dmg = TYRANT_DMG; tyrant.range = TYRANT_RANGE;
    tyrant.target_id = 0; tyrant.last_hit_by_time = 0;
    tyrant.last_attack_time = 0; tyrant.last_regen_time = 0;
    tyrant.attack_counter = 0; tyrant.boss_state = JungleMonster::IDLE;
    jungle_mobs[tyrant.id] = tyrant;

    // 普通野区
    struct Zone { int x, y, size; int buff_type; };
    std::vector<Zone> zones = {
        {56, 96, 26, MONSTER_TYPE_BLUE}, 
        {68, 28, 26, MONSTER_TYPE_RED}, 
        {28, 62, 26, MONSTER_TYPE_RED}, 
        {96, 62, 26, MONSTER_TYPE_BLUE} 
    };

    for (const auto& z : zones) {
        int cx = z.x + z.size / 2;
        int cy = z.y + z.size / 2;
        
        JungleMonster buff;
        buff.id = jungle_id_counter++; buff.type = z.buff_type; 
        buff.x = cx; buff.y = cy;
        buff.hp = MONSTER_BUFF_HP; buff.max_hp = MONSTER_BUFF_HP;
        buff.dmg = MONSTER_BUFF_DMG; buff.range = MONSTER_BUFF_RANGE;
        buff.target_id = 0; buff.last_hit_by_time = 0;
        buff.last_attack_time = 0; buff.last_regen_time = 0;
        buff.attack_counter = 0; buff.boss_state = JungleMonster::IDLE;
        jungle_mobs[buff.id] = buff;

        int std_count = 0, attempts = 0;
        while(std_count < 3 && attempts < 50) { 
            attempts++;
            int rx = z.x + 2 + rand() % (z.size - 4);
            int ry = z.y + 2 + rand() % (z.size - 4);
            if (game_map[ry][rx] != TILE_EMPTY) continue;
            if (dist_sq(rx, ry, cx, cy) < 25) continue;
            bool overlap = false;
            for(const auto& m : jungle_mobs) { if (dist_sq(rx, ry, m.second.x, m.second.y) < 9) { overlap = true; break; } }
            if(overlap) continue;

            JungleMonster mob;
            mob.id = jungle_id_counter++; mob.type = MONSTER_TYPE_STD;
            mob.x = rx; mob.y = ry;
            mob.hp = MONSTER_STD_HP; mob.max_hp = MONSTER_STD_HP;
            mob.dmg = MONSTER_STD_DMG; mob.range = MONSTER_STD_RANGE;
            mob.target_id = 0; mob.last_hit_by_time = 0;
            mob.last_attack_time = 0; mob.last_regen_time = 0;
            mob.attack_counter = 0; mob.boss_state = JungleMonster::IDLE;
            jungle_mobs[mob.id] = mob;
            std_count++;
        }
    }
}

void update_jungle() {
    long long now = get_current_ms();
    std::vector<int> dead_ids;

    // 清理过期特效
    for (auto it = active_effects.begin(); it != active_effects.end(); ) {
        if (now >= it->end_time) it = active_effects.erase(it);
        else ++it;
    }

    for (auto& pair : jungle_mobs) {
        JungleMonster& m = pair.second;
        if (m.hp <= 0) { dead_ids.push_back(m.id); continue; }

        // 脱战回血 (使用宏: 5000 HP / sec)
        if (m.target_id == 0 && m.hp < m.max_hp) {
            if (now - m.last_regen_time >= 1000) {
                m.hp += MONSTER_REGEN_TICK;
                if (m.hp > m.max_hp) m.hp = m.max_hp;
                m.last_regen_time = now;
                m.attack_counter = 0;
                m.boss_state = JungleMonster::IDLE;
            }
        }
        // 脱战判定 (使用宏: 5000 ms)
        if (m.target_id != 0 && now - m.last_hit_by_time > MONSTER_AGGRO_TIMEOUT) {
            m.target_id = 0; m.attack_counter = 0; m.boss_state = JungleMonster::IDLE;
        }

        if (m.target_id != 0) {
            // === 状态机逻辑 ===
            if (m.boss_state == JungleMonster::CASTING_PREPARE) {
                // 主宰蓄力 (1.5s)
                if (m.type == BOSS_TYPE_OVERLORD) {
                    if (now - m.skill_start_time >= OVERLORD_SKILL_DELAY) {
                        for (auto& target_pos : m.skill_targets) {
                            // 爆发喷泉特效 (深紫)
                            SkillEffect eff = {target_pos.x, target_pos.y, VFX_OVERLORD_DMG, now, now + 500, OVERLORD_SKILL_RADIUS, m.id};
                            active_effects.push_back(eff);
                            
                            // 伤害结算
                            for (auto& p : players) {
                                if (!p.second.is_playing) continue;
                                if (dist_sq(p.second.x, p.second.y, target_pos.x, target_pos.y) <= OVERLORD_SKILL_RADIUS * OVERLORD_SKILL_RADIUS) {
                                    p.second.hp -= (m.dmg * 3);
                                    p.second.current_effect = EFFECT_HIT;
                                    std::cout << "Overlord Skill Hit Player " << p.second.id << " Dmg: " << (m.dmg * 3) << std::endl;
                                    if(p.second.hp <= 0) { 
                                        p.second.hp = p.second.max_hp; p.second.x = (p.second.color == 1)?22:128; p.second.y = (p.second.color == 1)?128:22;
                                        m.target_id = 0; m.attack_counter = 0; m.boss_state = JungleMonster::IDLE;
                                    }
                                }
                            }
                        }
                        m.boss_state = JungleMonster::IDLE;
                        m.last_attack_time = now; 
                    }
                }
                continue; 
            }
            else if (m.boss_state == JungleMonster::CASTING_ACTIVE) {
                // 暴君持续施法
                if (m.type == BOSS_TYPE_TYRANT) {
                    if (now - m.skill_start_time >= TYRANT_SKILL_DUR) {
                        m.boss_state = JungleMonster::IDLE;
                        m.last_attack_time = now;
                    } else {
                        if (now >= m.next_tick_time) {
                            m.next_tick_time += 500;
                            for (auto& p : players) {
                                if (!p.second.is_playing) continue;
                                int d = dist_sq(m.x, m.y, p.second.x, p.second.y);
                                if (d <= TYRANT_RANGE * TYRANT_RANGE) {
                                    p.second.hp -= (m.dmg * 2);
                                    p.second.current_effect = EFFECT_HIT;
                                    int push_dx = 0, push_dy = 0;
                                    if (p.second.x > m.x) push_dx = 1; else if (p.second.x < m.x) push_dx = -1;
                                    if (p.second.y > m.y) push_dy = 1; else if (p.second.y < m.y) push_dy = -1;
                                    int nx = p.second.x + push_dx;
                                    int ny = p.second.y + push_dy;
                                    if (!is_blocked_by_tower(nx, ny)) { p.second.x = nx; p.second.y = ny; }
                                    if(p.second.hp <= 0) {
                                        p.second.hp = p.second.max_hp; p.second.x = (p.second.color == 1)?22:128; p.second.y = (p.second.color == 1)?128:22;
                                        m.target_id = 0; m.attack_counter = 0; m.boss_state = JungleMonster::IDLE;
                                    }
                                }
                            }
                        }
                    }
                }
                continue; 
            }

            int tx=0, ty=0; bool valid=false;
            Player* p = get_player_by_id(m.target_id);
            if (p) { tx = p->x; ty = p->y; valid = true; }
            if (!valid) { m.target_id = 0; continue; }

            int d = dist_sq(m.x, m.y, tx, ty);
            if (d <= m.range * m.range) {
                int cd = (m.type == BOSS_TYPE_OVERLORD) ? OVERLORD_ATK_CD : 
                         (m.type == BOSS_TYPE_TYRANT) ? TYRANT_ATK_CD : MONSTER_ATK_COOLDOWN;
                
                if (now - m.last_attack_time >= cd) {
                    if ((m.type == BOSS_TYPE_OVERLORD || m.type == BOSS_TYPE_TYRANT) && m.attack_counter >= 3) {
                        m.attack_counter = 0;
                        
                        if (m.type == BOSS_TYPE_OVERLORD) {
                            m.boss_state = JungleMonster::CASTING_PREPARE;
                            m.skill_start_time = now;
                            m.skill_targets.clear();
                            for(auto& pl : players) {
                                if(pl.second.is_playing && dist_sq(m.x, m.y, pl.second.x, pl.second.y) <= OVERLORD_RANGE * OVERLORD_RANGE) {
                                    m.skill_targets.push_back({pl.second.x, pl.second.y});
                                    SkillEffect eff = {pl.second.x, pl.second.y, VFX_OVERLORD_WARN, now, now + OVERLORD_SKILL_DELAY, OVERLORD_SKILL_RADIUS, m.id};
                                    active_effects.push_back(eff);
                                }
                            }
                        } 
                        else if (m.type == BOSS_TYPE_TYRANT) {
                            m.boss_state = JungleMonster::CASTING_ACTIVE;
                            m.skill_start_time = now;
                            m.next_tick_time = now + 500;
                        }
                    } 
                    else {
                        m.last_attack_time = now;
                        m.visual_end_time = now + 200;
                        m.attack_counter++;
                        if (p) { p->hp -= m.dmg; p->current_effect = EFFECT_HIT; }
                    }
                }
            } else {
                if (d > (m.range + 5) * (m.range + 5)) { m.target_id = 0; m.attack_counter = 0; }
            }
        }
    }
    
    for (int id : dead_ids) jungle_mobs.erase(id);
}

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
            m.wp_idx = 0; m.state = Minion::MARCHING; 
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
            m.wp_idx = path->size() - 1; m.state = Minion::MARCHING; 
            minions[m.id] = m;
        }
    }
}

void update_towers() {
    long long now = get_current_ms();
    int tower_range_sq = TOWER_ATK_RANGE * TOWER_ATK_RANGE;

    for (auto& pair : towers) {
        Tower& t = pair.second;
        if (t.hp <= 0) continue;

        int best_target = 0;
        int min_dist = 99999;

        for (auto& p : players) {
            Player& enemy = p.second;
            if (!enemy.is_playing || enemy.color == t.team) continue;
            int d = dist_sq(t.x, t.y, enemy.x, enemy.y);
            if (d > tower_range_sq) continue;
            if (now - enemy.last_aggressive_time < 2000) { best_target = enemy.id; break; }
        }

        if (best_target == 0 && t.target_id != 0) {
            bool valid = false; int tx=0, ty=0;
            Player* p = get_player_by_id(t.target_id);
            if (p) { tx = p->x; ty = p->y; valid = true; }
            else if (minions.count(t.target_id)) {
                Minion& m = minions[t.target_id];
                if (m.hp > 0) { tx = (int)m.x; ty = (int)m.y; valid = true; }
            }
            if (valid && dist_sq(t.x, t.y, tx, ty) <= tower_range_sq) best_target = t.target_id;
        }

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

        if (best_target != t.target_id) { t.consecutive_hits = 0; t.target_id = best_target; }

        if (t.target_id != 0) {
            if (now - t.last_attack_time >= TOWER_ATK_COOLDOWN) {
                t.last_attack_time = now;
                t.visual_end_time = now + 200; 
                int damage = 0;
                Player* p = get_player_by_id(t.target_id);
                if (p) { 
                    damage = TOWER_BASE_DMG_HERO * (int)pow(2, t.consecutive_hits);
                    t.consecutive_hits++; 
                    p->hp -= damage;
                    p->current_effect = EFFECT_HIT;
                    if(p->hp <= 0) {
                        p->hp = p->max_hp;
                        p->x = (p->color == 1) ? 22 : 128; p->y = (p->color == 1) ? 128 : 22;
                        t.target_id = 0; t.consecutive_hits = 0;
                    }
                } 
                else if (minions.count(t.target_id)) { 
                    damage = TOWER_BASE_DMG_MINION + 100 * wave_count;
                    minions[t.target_id].hp -= damage;
                }
            }
        } else { t.consecutive_hits = 0; }
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
                    if (d <= (MINION_VISION_RANGE+2)*(MINION_VISION_RANGE+2) && d < min_d) { min_d = d; found = t.second.id; }
                }
            }
            if (found != 0) { m.state = Minion::CHASING; m.target_id = found; m.anchor_x = m.x; m.anchor_y = m.y; }
            else {
                const std::vector<Point>* path = (m.lane == 0) ? &PATH_TOP : ((m.lane == 1) ? &PATH_MID : &PATH_BOT);
                Point target = (*path)[m.wp_idx];
                float dx = target.x - m.x, dy = target.y - m.y; float dist = sqrt(dx*dx + dy*dy);
                if (dist < 2.0f) {
                    if (m.team == 1 && m.wp_idx < path->size() - 1) m.wp_idx++;
                    else if (m.team == 2 && m.wp_idx > 0) m.wp_idx--;
                } else { m.x += (dx/dist) * MINION_MOVE_SPEED; m.y += (dy/dist) * MINION_MOVE_SPEED; }
            }
        }
        else if (m.state == Minion::CHASING) {
            int dist_anchor = dist_sq((int)m.x, (int)m.y, (int)m.anchor_x, (int)m.anchor_y);
            if (dist_anchor > MINION_CHASE_LIMIT * MINION_CHASE_LIMIT) { m.state = Minion::RETURNING; m.target_id = 0; }
            else {
                int tx = 0, ty = 0; bool exists = false;
                Player* p = get_player_by_id(m.target_id);
                if (p) { tx = p->x; ty = p->y; exists = true; }
                else if (minions.count(m.target_id) && minions[m.target_id].hp > 0) { tx = (int)minions[m.target_id].x; ty = (int)minions[m.target_id].y; exists = true; }
                else if (towers.count(m.target_id) && towers[m.target_id].hp > 0) { tx = towers[m.target_id].x; ty = towers[m.target_id].y; exists = true; }
                if (!exists) m.state = Minion::RETURNING;
                else {
                    int dist_target = dist_sq((int)m.x, (int)m.y, tx, ty);
                    int atk_range_bonus = (m.target_id >= 100 && m.target_id < 1000) ? 2 : 0;
                    if (dist_target <= (m.range + atk_range_bonus) * (m.range + atk_range_bonus)) {
                        if (now - m.last_attack_time >= MINION_ATK_COOLDOWN) {
                            m.last_attack_time = now; m.visual_end_time = now + 200; 
                            if (p) p->hp -= m.dmg;
                            else if (minions.count(m.target_id)) minions[m.target_id].hp -= m.dmg;
                            else if (towers.count(m.target_id)) towers[m.target_id].hp -= m.dmg; 
                        }
                    } else {
                        float dx = tx - m.x, dy = ty - m.y; float dist = sqrt(dx*dx + dy*dy);
                        m.x += (dx/dist) * MINION_MOVE_SPEED; m.y += (dy/dist) * MINION_MOVE_SPEED;
                    }
                }
            }
        }
        else if (m.state == Minion::RETURNING) {
            float dx = m.anchor_x - m.x, dy = m.anchor_y - m.y; float dist = sqrt(dx*dx + dy*dy);
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
    for (auto& m : jungle_mobs) {
        int d = dist_sq(att.x, att.y, m.second.x, m.second.y);
        if (d <= range_sq + 5 && d < min_dist) { min_dist = d; target_id = m.second.id; }
    }

    if (target_id != 0) {
        att.current_target_id = target_id; 
        att.visual_end_time = get_current_ms() + 200; 
        hit = true;
        long long now = get_current_ms();
        Player* p = get_player_by_id(target_id);
        if (p) {
            p->hp -= tmpl.attack_dmg; 
            p->current_effect = EFFECT_HIT;
            att.last_aggressive_time = now; 
            if (p->hp <= 0) {
                p->hp = p->max_hp; p->x = (p->color == 1) ? 22 : 128; p->y = (p->color == 1) ? 128 : 22;
                std::cout << "[Kill] Player " << p->id << " died." << std::endl;
            }
        } else if (target_id >= JUNGLE_ID_START) {
            if (jungle_mobs.count(target_id)) {
                JungleMonster& mob = jungle_mobs[target_id];
                mob.hp -= tmpl.attack_dmg;
                mob.target_id = att.id;
                mob.last_hit_by_time = now; 
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
    
    for (auto& sp : players) {
        if (!sp.second.is_playing) continue;
        Player& p = sp.second;
        int range = hero_db[p.hero_id].attack_range;
        int atk_target = (now < p.visual_end_time) ? p.current_target_id : 0;
        GamePacket pkt = { TYPE_UPDATE, p.id, p.x, p.y, p.hero_id, 0, p.color, p.hp, p.max_hp, range, p.current_effect, atk_target };
        updates.push_back(pkt);
    }
    for (auto& pair : towers) {
        Tower& t = pair.second;
        if (t.hp > 0) { 
            int atk_target = (now < t.visual_end_time) ? t.target_id : 0;
            GamePacket pkt = { TYPE_UPDATE, t.id, t.x, t.y, 0, 0, t.team, t.hp, t.max_hp, 0, 0, atk_target };
            updates.push_back(pkt);
        }
    }
    for (auto& pair : minions) {
        Minion& m = pair.second;
        int atk_target = (now < m.visual_end_time) ? m.target_id : 0;
        GamePacket pkt = { TYPE_UPDATE, m.id, (int)m.x, (int)m.y, m.type, 0, m.team, m.hp, m.max_hp, 0, 0, atk_target };
        updates.push_back(pkt);
    }
    for (auto& pair : jungle_mobs) {
        JungleMonster& m = pair.second;
        if (m.hp > 0) {
            int atk_target = (now < m.visual_end_time) ? m.target_id : 0;
            GamePacket pkt = { TYPE_UPDATE, m.id, m.x, m.y, m.type, 0, 0, m.hp, m.max_hp, 0, 0, atk_target };
            if (m.type == BOSS_TYPE_TYRANT && m.boss_state == JungleMonster::CASTING_ACTIVE) {
                GamePacket eff = { TYPE_EFFECT, 0, m.x, m.y, VFX_TYRANT_WAVE, 0, 0, 0, 0, TYRANT_RANGE, 0, 0 };
                updates.push_back(eff);
            }
            updates.push_back(pkt);
        }
    }
    for (const auto& ef : active_effects) {
        GamePacket pkt = { TYPE_EFFECT, 0, ef.x, ef.y, ef.type, 0, 0, 0, 0, ef.radius, 0, 0 };
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
    init_jungle(); 
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
        update_jungle(); 
        if (!minions.empty()) { update_minions(); need_broadcast = true; }
        if (!jungle_mobs.empty()) need_broadcast = true; 

        if (need_broadcast) broadcast_world();
    }
    return 0;
}