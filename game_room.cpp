#include "game_room.h"
#include "map.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono> // [修复] 必须包含此头文件

// =========================================
// 静态辅助函数与数据
// =========================================

static long long get_current_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int dist_sq(int x1, int y1, int x2, int y2) {
    return (x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2);
}

// 英雄属性表
struct HeroData { int base_hp, range, dmg; };
static std::map<int, HeroData> HERO_DB = {
    {HERO_WARRIOR, {HERO_HP_DEFAULT, 2, HERO_DMG_DEFAULT}},
    {HERO_MAGE,    {HERO_HP_DEFAULT, 6, HERO_DMG_DEFAULT}},
    {HERO_TANK,    {HERO_HP_DEFAULT, 2, HERO_DMG_DEFAULT}}
};

// 兵线路径
struct Pt { int x, y; };
static const std::vector<Pt> PATH_TOP = { {22, 128}, {22, 22},  {128, 22} };  
static const std::vector<Pt> PATH_MID = { {22, 128}, {75, 75},  {128, 22} };  
static const std::vector<Pt> PATH_BOT = { {22, 128}, {128, 128},{128, 22} };  

// =========================================
// GameRoom 生命周期管理
// =========================================

GameRoom::GameRoom(int id, const std::string& owner_name) {
    this->room_id = id;
    this->status = 0; // Waiting
    this->game_start_time = 0;
    this->wave_count = 0;
    this->last_spawn_minute = -1;
    
    // 初始化 ID 计数器
    this->global_id_counter = 1;
    this->tower_id_counter = TOWER_ID_START;
    this->minion_id_counter = MINION_ID_START;
    this->jungle_id_counter = JUNGLE_ID_START;
    this->boss_id_counter = BOSS_ID_START;
    
    // 初始化地图
    MapGenerator::init(this->game_map);
}

GameRoom::~GameRoom() {
    std::cout << "[Room " << room_id << "] Destroyed." << std::endl;
}

// =========================================
// 大厅/房间管理逻辑
// =========================================

bool GameRoom::add_player(int fd, const std::string& name) {
    if (players.size() >= 10) return false;
    if (status == 1) return false; // 游戏中禁止加入

    PlayerState p;
    p.fd = fd;
    p.name = name;
    p.id = global_id_counter++;
    p.is_ready = false;
    p.is_playing = false;
    
    // 自动分配空闲座位 (0-9)
    std::vector<int> taken_slots;
    for(auto& pair : players) taken_slots.push_back(pair.second.room_slot);
    
    for(int i=0; i<10; i++) {
        bool taken = false;
        for(int s : taken_slots) if(s == i) taken = true;
        if(!taken) { p.room_slot = i; break; }
    }
    
    // 决定阵营: 0-4 Team1(上), 5-9 Team2(下)
    p.color = (p.room_slot < 5) ? 1 : 2;
    
    players[fd] = p;
    return true;
}

void GameRoom::remove_player(int fd) {
    if (players.count(fd)) {
        players.erase(fd);
    }
}

void GameRoom::set_ready(int fd, bool ready) {
    if (players.count(fd)) players[fd].is_ready = ready;
}

void GameRoom::change_slot(int fd, int target_slot) {
    if (target_slot < 0 || target_slot > 9) return;
    if (players.count(fd) == 0) return;

    // 检查目标位置是否有人
    for(auto& pair : players) {
        if (pair.second.room_slot == target_slot) return; 
    }
    
    players[fd].room_slot = target_slot;
    players[fd].color = (target_slot < 5) ? 1 : 2;
}

bool GameRoom::start_game(int fd_requester) {
    if (status == 1) return false;
    
    status = 1; // Playing
    game_start_time = get_current_ms();
    
    // 初始化地图实体
    init_map_and_units();
    
    // 初始化玩家战斗属性
    for(auto& pair : players) {
        PlayerState& p = pair.second;
        p.is_playing = true;
        p.hero_id = HERO_WARRIOR; // 默认
        p.max_hp = HERO_HP_DEFAULT;
        p.hp = p.max_hp;
        // 设定出生点
        p.x = (p.color == 1) ? 22 : 128;
        p.y = (p.color == 1) ? 128 : 22;
        p.last_aggressive_time = 0;
        p.visual_end_time = 0;
    }
    
    return true;
}

RoomInfo GameRoom::get_room_info() {
    RoomInfo info;
    info.room_id = room_id;
    info.player_count = players.size();
    info.max_player = 10;
    info.status = status;
    
    // 寻找房主 (slot最小者)
    int min_slot = 99;
    std::string owner = "System";
    for(auto& pair : players) {
        if(pair.second.room_slot < min_slot) {
            min_slot = pair.second.room_slot;
            owner = pair.second.name;
        }
    }
    strncpy(info.owner_name, owner.c_str(), 31);
    return info;
}

RoomStatePacket GameRoom::get_room_state_packet() {
    RoomStatePacket pkt;
    pkt.type = TYPE_ROOM_UPDATE;
    pkt.room_id = room_id;
    memset(pkt.slots, 0, sizeof(pkt.slots));
    
    int min_slot = 99;
    for(auto& pair : players) if(pair.second.room_slot < min_slot) min_slot = pair.second.room_slot;

    for(auto& pair : players) {
        PlayerState& p = pair.second;
        int s = p.room_slot;
        if(s >= 0 && s < 10) {
            pkt.slots[s].is_taken = 1;
            strncpy(pkt.slots[s].name, p.name.c_str(), 31);
            pkt.slots[s].is_ready = p.is_ready;
            pkt.slots[s].team = p.color;
            if(s == min_slot) pkt.slots[s].is_owner = 1;
        }
    }
    return pkt;
}

std::vector<int> GameRoom::get_player_fds() {
    std::vector<int> fds;
    for(auto& pair : players) fds.push_back(pair.first);
    return fds;
}

// [新增] 获取玩家真实Entity ID
int GameRoom::get_player_id(int fd) {
    if (players.count(fd)) return players[fd].id;
    return -1;
}

bool GameRoom::is_empty() {
    return players.empty();
}

// =========================================
// 游戏核心逻辑 (物理/AI/战斗)
// =========================================

void GameRoom::handle_game_packet(int fd, const GamePacket& pkt) {
    // 1. 基础检查：房间状态必须是 playing (1)，玩家必须存在
    if (status != 1) {
        // std::cout << "[ROOM] Ignored packet: Room not playing." << std::endl;
        return;
    }
    if (players.count(fd) == 0) return;

    PlayerState& p = players[fd];
    
    if (pkt.type == TYPE_MOVE) {
        int dx = pkt.x;
        int dy = pkt.y;

        // 限制输入范围，防止瞬移
        if (dx < -1) dx = -1; if (dx > 1) dx = 1;
        if (dy < -1) dy = -1; if (dy > 1) dy = 1;
        
        int nx = p.x + dx;
        int ny = p.y + dy;

        // [DEBUG] 打印移动意图
        // std::cout << "[ROOM] Player " << p.id << " (" << p.name << ") wants move: " 
        //           << p.x << "," << p.y << " -> " << nx << "," << ny << std::endl;

        // 碰撞检测
        bool blocked = false;
        if (!is_valid_move(nx, ny)) {
            blocked = true; // 撞地图边界或墙
            std::cout << "[ROOM] Blocked by WALL/MAP at " << nx << "," << ny << std::endl;
        } 
        else if (is_blocked_by_tower(nx, ny)) {
            blocked = true; // 撞塔
            std::cout << "[ROOM] Blocked by TOWER at " << nx << "," << ny << std::endl;
        }

        if (!blocked) { 
            p.x = nx; 
            p.y = ny; 
            std::cout << "[ROOM] Move OK: " << p.id << " is now at " << p.x << "," << p.y << std::endl;
        }
    }
    else if (pkt.type == TYPE_ATTACK) {
        bool hit = handle_attack_logic(fd);
        if (hit) {
            std::cout << "[ROOM] Player " << p.id << " ATTACK HIT!" << std::endl;
        } else {
            std::cout << "[ROOM] Player " << p.id << " ATTACK MISS (No target in range)." << std::endl;
        }
    }
    else if (pkt.type == TYPE_SPELL) {
        p.hp += 100; 
        if(p.hp > p.max_hp) p.hp = p.max_hp;
    }
    else if (pkt.type == TYPE_SELECT) {
        if (HERO_DB.count(pkt.input)) {
            p.hero_id = pkt.input;
            p.max_hp = HERO_DB[pkt.input].base_hp;
            p.hp = p.max_hp;
            std::cout << "[ROOM] Player " << p.id << " selected hero " << p.hero_id << std::endl;
        }
    }
}

void GameRoom::update_logic() {
    if (status != 1) return;
    
    long long now = get_current_ms();
    
    // 1. 每分钟出兵
    int current_sec = (now - game_start_time) / 1000;
    if (current_sec >= 30 && (current_sec - 30) % 60 == 0) {
        if (current_sec != last_spawn_minute) { 
            spawn_wave();
            last_spawn_minute = current_sec;
        }
    }

    // 2. 实体更新
    update_towers(now);
    update_minions(now);
    update_jungle(now);
    
    // 3. 状态广播
    broadcast_world(now);
}

// =========================================
// 私有辅助逻辑 (初始化与AI)
// =========================================

void GameRoom::init_map_and_units() {
    // 1. 初始化防御塔
    for(int y=0; y<MAP_SIZE; y++) {
        for(int x=0; x<MAP_SIZE; x++) {
            int t = game_map[y][x];
            if ((t >= 11 && t <= 23) || t == TILE_BASE) {
                TowerObj tower; 
                tower.id = tower_id_counter++; tower.x = x; tower.y = y;
                tower.target_id = 0; tower.consecutive_hits = 0; 
                tower.last_attack_time = 0; tower.visual_end_time = 0;
                
                if (t == TILE_BASE) {
                    tower.team = (x < 75) ? 1 : 2; tower.max_hp = 10000;
                } else {
                    if (t >= 11 && t <= 13) {
                        tower.team = 1;
                        if(t==11) tower.max_hp=TOWER_HP_TIER_1; 
                        else if(t==12) tower.max_hp=TOWER_HP_TIER_2; 
                        else tower.max_hp=TOWER_HP_TIER_3;
                    } else {
                        tower.team = 2;
                        if(t==21) tower.max_hp=TOWER_HP_TIER_1; 
                        else if(t==22) tower.max_hp=TOWER_HP_TIER_2; 
                        else tower.max_hp=TOWER_HP_TIER_3;
                    }
                }
                tower.hp = tower.max_hp;
                towers[tower.id] = tower;
            }
        }
    }
    
    // 2. 初始化 Boss (位置更新为向中路迁移20格)
    // 主宰 (Overlord) @ (55, 55)
    JungleObj overlord;
    overlord.id = boss_id_counter++; overlord.type = BOSS_TYPE_OVERLORD;
    overlord.x = 55; overlord.y = 55; 
    overlord.hp = OVERLORD_HP; overlord.max_hp = OVERLORD_HP;
    overlord.dmg = OVERLORD_DMG; overlord.range = OVERLORD_RANGE;
    overlord.boss_state = 0; overlord.target_id = 0;
    overlord.last_hit_by_time = 0; overlord.last_attack_time = 0; overlord.last_regen_time = 0;
    jungle_mobs[overlord.id] = overlord;
    
    // 暴君 (Tyrant) @ (95, 95)
    JungleObj tyrant;
    tyrant.id = boss_id_counter++; tyrant.type = BOSS_TYPE_TYRANT;
    tyrant.x = 95; tyrant.y = 95;
    tyrant.hp = TYRANT_HP; tyrant.max_hp = TYRANT_HP;
    tyrant.dmg = TYRANT_DMG; tyrant.range = TYRANT_RANGE;
    tyrant.boss_state = 0; tyrant.target_id = 0;
    tyrant.last_hit_by_time = 0; tyrant.last_attack_time = 0; tyrant.last_regen_time = 0;
    jungle_mobs[tyrant.id] = tyrant;

    // 3. 初始化普通野怪
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
        
        // Buff怪
        JungleObj buff;
        buff.id = jungle_id_counter++; buff.type = z.buff_type; 
        buff.x = cx; buff.y = cy;
        buff.hp = MONSTER_BUFF_HP; buff.max_hp = MONSTER_BUFF_HP;
        buff.dmg = MONSTER_BUFF_DMG; buff.range = MONSTER_BUFF_RANGE;
        buff.target_id = 0; buff.last_hit_by_time = 0; buff.last_attack_time = 0; buff.last_regen_time = 0;
        buff.boss_state = 0; // IDLE
        jungle_mobs[buff.id] = buff;

        // 普通野怪
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

            JungleObj mob;
            mob.id = jungle_id_counter++; mob.type = MONSTER_TYPE_STD;
            mob.x = rx; mob.y = ry;
            mob.hp = MONSTER_STD_HP; mob.max_hp = MONSTER_STD_HP;
            mob.dmg = MONSTER_STD_DMG; mob.range = MONSTER_STD_RANGE;
            mob.target_id = 0; mob.last_hit_by_time = 0; mob.last_attack_time = 0; mob.last_regen_time = 0;
            mob.boss_state = 0;
            jungle_mobs[mob.id] = mob;
            std_count++;
        }
    }
}

void GameRoom::spawn_wave() {
    wave_count++;
    int melee_hp = MELEE_BASE_HP + 200 * wave_count;
    int melee_dmg = MELEE_BASE_DMG + 150 * wave_count; 
    int ranged_hp = RANGED_BASE_HP + 150 * wave_count;
    int ranged_dmg = RANGED_BASE_DMG + 200 * wave_count; 

    for (int lane = 0; lane < 3; lane++) {
        // Team 1
        for(int i=0; i<3; i++) {
            MinionObj m; m.id = minion_id_counter++; m.team = 1; m.lane = lane;
            m.type = (i < 2) ? MINION_TYPE_MELEE : MINION_TYPE_RANGED;
            m.hp = (m.type == 1) ? melee_hp : ranged_hp; m.max_hp = m.hp;
            m.dmg = (m.type == 1) ? melee_dmg : ranged_dmg;
            m.range = (m.type == 1) ? MELEE_RANGE : RANGED_RANGE;
            m.x = 22 + rand()%2; m.y = 128 + rand()%2; 
            m.wp_idx = 0; m.state = 0; m.last_attack_time = 0; m.visual_end_time = 0;
            minions[m.id] = m;
        }
        // Team 2
        for(int i=0; i<3; i++) {
            MinionObj m; m.id = minion_id_counter++; m.team = 2; m.lane = lane;
            m.type = (i < 2) ? MINION_TYPE_MELEE : MINION_TYPE_RANGED;
            m.hp = (m.type == 1) ? melee_hp : ranged_hp; m.max_hp = m.hp;
            m.dmg = (m.type == 1) ? melee_dmg : ranged_dmg;
            m.range = (m.type == 1) ? MELEE_RANGE : RANGED_RANGE;
            m.x = 128 + rand()%2; m.y = 22 + rand()%2; 
            const std::vector<Pt>* path = (lane == 0) ? &PATH_TOP : ((lane == 1) ? &PATH_MID : &PATH_BOT);
            m.wp_idx = path->size() - 1; m.state = 0; m.last_attack_time = 0; m.visual_end_time = 0;
            minions[m.id] = m;
        }
    }
}

void GameRoom::update_towers(long long now) {
    int tower_range_sq = TOWER_ATK_RANGE * TOWER_ATK_RANGE;
    
    for (auto& pair : towers) {
        TowerObj& t = pair.second;
        if (t.hp <= 0) continue;
        
        int best_target = 0;
        int min_dist = 99999;
        
        // 1. 仇恨机制：优先攻击 aggressive 的玩家
        for (auto& p : players) {
            if (!p.second.is_playing || p.second.color == t.team) continue;
            int d = dist_sq(t.x, t.y, p.second.x, p.second.y);
            if (d > tower_range_sq) continue;
            
            // 2秒内的仇恨判定
            if (now - p.second.last_aggressive_time < 2000) {
                best_target = p.second.id;
                break; // 找到仇恨目标，立即锁定
            }
        }

        // 2. 锁定机制：如果没有仇恨目标，且当前目标依然有效（活着且在范围内），则不切换
        if (best_target == 0 && t.target_id != 0) {
            bool valid = false;
            int tx = 0, ty = 0;
            
            // 检查玩家
            PlayerState* p = get_player_by_id(t.target_id);
            if (p) { tx = p->x; ty = p->y; valid = true; }
            else if (minions.count(t.target_id)) {
                // 检查小兵
                if (minions[t.target_id].hp > 0) {
                    tx = (int)minions[t.target_id].x;
                    ty = (int)minions[t.target_id].y;
                    valid = true;
                }
            }
            
            if (valid) {
                if (dist_sq(t.x, t.y, tx, ty) <= tower_range_sq) {
                    best_target = t.target_id; // 保持当前目标
                }
            }
        }

        // 3. 索敌机制：如果完全没有目标，寻找最近的敌人
        // [关键修复] 严格遵循 server.cpp 逻辑：先找小兵，只有完全没小兵时才找英雄
        if (best_target == 0) {
            // A. 先找小兵 (Minion Priority)
            for (auto& m : minions) {
                if (m.second.team == t.team) continue;
                int d = dist_sq(t.x, t.y, (int)m.second.x, (int)m.second.y);
                if (d <= tower_range_sq && d < min_dist) { 
                    min_dist = d; 
                    best_target = m.second.id; 
                }
            }
            
            // B. 只有当 best_target 依然为 0 (没找到任何小兵) 时，才找玩家
            if (best_target == 0) {
                for (auto& p : players) {
                    if (!p.second.is_playing || p.second.color == t.team) continue;
                    int d = dist_sq(t.x, t.y, p.second.x, p.second.y);
                    if (d <= tower_range_sq && d < min_dist) { 
                        min_dist = d; 
                        best_target = p.second.id; 
                    }
                }
            }
        }

        // 目标切换重置连击
        if (best_target != t.target_id) { 
            t.consecutive_hits = 0; 
            t.target_id = best_target; 
        }

        // 攻击逻辑
        if (t.target_id != 0 && now - t.last_attack_time >= TOWER_ATK_COOLDOWN) {
            t.last_attack_time = now;
            t.visual_end_time = now + 200;
            
            PlayerState* p = get_player_by_id(t.target_id);
            if (p) {
                int dmg = TOWER_BASE_DMG_HERO * (int)pow(2, t.consecutive_hits);
                t.consecutive_hits++;
                p->hp -= dmg;
                p->current_effect = EFFECT_HIT;
                if(p->hp <= 0) { 
                    p->hp = p->max_hp; 
                    p->x = (p->color == 1) ? 22 : 128; p->y = (p->color == 1) ? 128 : 22;
                    t.target_id = 0; t.consecutive_hits = 0;
                }
            } else if (minions.count(t.target_id)) {
                // 小兵伤害成长
                minions[t.target_id].hp -= (TOWER_BASE_DMG_MINION + 100 * wave_count);
            }
        }
    }
}

void GameRoom::update_minions(long long now) {
    std::vector<int> dead_ids;
    for (auto& pair : minions) {
        MinionObj& m = pair.second;
        if (m.hp <= 0) { dead_ids.push_back(m.id); continue; }
        
        // MARCHING
        if (m.state == 0) {
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
                m.state = 1; m.target_id = found; // CHASING
                m.anchor_x = m.x; m.anchor_y = m.y;
            } else {
                // Move along path
                const std::vector<Pt>* path = (m.lane == 0) ? &PATH_TOP : ((m.lane == 1) ? &PATH_MID : &PATH_BOT);
                if (m.wp_idx >= 0 && m.wp_idx < (int)path->size()) {
                    Pt target = (*path)[m.wp_idx];
                    float dx = target.x - m.x, dy = target.y - m.y;
                    float dist = sqrt(dx*dx + dy*dy);
                    if (dist < 2.0f) {
                        if (m.team == 1 && m.wp_idx < (int)path->size()-1) m.wp_idx++;
                        else if (m.team == 2 && m.wp_idx > 0) m.wp_idx--;
                    } else {
                        m.x += (dx/dist) * MINION_MOVE_SPEED;
                        m.y += (dy/dist) * MINION_MOVE_SPEED;
                    }
                }
            }
        }
        // CHASING
        else if (m.state == 1) {
            int dist_anchor = dist_sq((int)m.x, (int)m.y, (int)m.anchor_x, (int)m.anchor_y);
            if (dist_anchor > MINION_CHASE_LIMIT * MINION_CHASE_LIMIT) {
                m.state = 2; m.target_id = 0; // RETURNING
            } else {
                int tx = 0, ty = 0; bool exists = false;
                PlayerState* p = get_player_by_id(m.target_id);
                if (p) { tx = p->x; ty = p->y; exists = true; }
                else if (minions.count(m.target_id) && minions[m.target_id].hp > 0) {
                    tx = (int)minions[m.target_id].x; ty = (int)minions[m.target_id].y; exists = true;
                } else if (towers.count(m.target_id) && towers[m.target_id].hp > 0) {
                    tx = towers[m.target_id].x; ty = towers[m.target_id].y; exists = true;
                }

                if (!exists) m.state = 2; // RETURNING
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
        // RETURNING
        else if (m.state == 2) {
            float dx = m.anchor_x - m.x, dy = m.anchor_y - m.y;
            float dist = sqrt(dx*dx + dy*dy);
            if (dist < 1.0f) m.state = 0; // MARCHING
            else { 
                m.x += (dx/dist) * (MINION_MOVE_SPEED * 2.0f); 
                m.y += (dy/dist) * (MINION_MOVE_SPEED * 2.0f); 
            }
        }
    }
    for(int id : dead_ids) minions.erase(id);
}

void GameRoom::update_jungle(long long now) {
    std::vector<int> dead_ids;
    
    // 清理过期特效
    for (auto it = active_effects.begin(); it != active_effects.end(); ) {
        if (now >= it->end_time) it = active_effects.erase(it);
        else ++it;
    }

    for (auto& pair : jungle_mobs) {
        JungleObj& m = pair.second;
        if (m.hp <= 0) { dead_ids.push_back(m.id); continue; }

        // 脱战回血
        if (m.target_id == 0 && m.hp < m.max_hp) {
            if (now - m.last_regen_time >= 1000) {
                m.hp += MONSTER_REGEN_TICK;
                if (m.hp > m.max_hp) m.hp = m.max_hp;
                m.last_regen_time = now;
                m.attack_counter = 0;
                m.boss_state = 0; // IDLE
            }
        }
        // 脱战判定
        if (m.target_id != 0 && now - m.last_hit_by_time > MONSTER_AGGRO_TIMEOUT) {
            m.target_id = 0; m.attack_counter = 0; m.boss_state = 0;
        }

        if (m.target_id != 0) {
            // === Boss 技能状态机 (修复版) ===
            if (m.boss_state == 1) { // PREPARE (主宰蓄力)
                if (m.type == BOSS_TYPE_OVERLORD) {
                    if (now - m.skill_start_time >= OVERLORD_SKILL_DELAY) {
                        for (auto& target_pos : m.skill_targets) {
                            // 产生特效
                            SkillEffectObj eff = {target_pos.x, target_pos.y, VFX_OVERLORD_DMG, now, now + 500, OVERLORD_SKILL_RADIUS, m.id};
                            active_effects.push_back(eff);
                            
                            // 造成伤害
                            for (auto& p : players) {
                                if (!p.second.is_playing) continue;
                                if (dist_sq(p.second.x, p.second.y, target_pos.x, target_pos.y) <= OVERLORD_SKILL_RADIUS * OVERLORD_SKILL_RADIUS) {
                                    p.second.hp -= (m.dmg * 3);
                                    p.second.current_effect = EFFECT_HIT;
                                    if(p.second.hp <= 0) { 
                                        p.second.hp = p.second.max_hp; 
                                        p.second.x = (p.second.color == 1)?22:128; p.second.y = (p.second.color == 1)?128:22;
                                        m.target_id = 0; m.attack_counter = 0; m.boss_state = 0;
                                    }
                                }
                            }
                        }
                        m.boss_state = 0; // Back to IDLE
                        m.last_attack_time = now; 
                    }
                }
                continue; // 正在施法，跳过普攻
            }
            else if (m.boss_state == 2) { // ACTIVE (暴君持续施法)
                if (m.type == BOSS_TYPE_TYRANT) {
                    if (now - m.skill_start_time >= TYRANT_SKILL_DUR) {
                        m.boss_state = 0;
                        m.last_attack_time = now;
                    } else {
                        if (now >= m.next_tick_time) {
                            m.next_tick_time += 500;
                            // 全图/范围判定
                            for (auto& p : players) {
                                if (!p.second.is_playing) continue;
                                int d = dist_sq(m.x, m.y, p.second.x, p.second.y);
                                if (d <= TYRANT_RANGE * TYRANT_RANGE) {
                                    p.second.hp -= (m.dmg * 2);
                                    p.second.current_effect = EFFECT_HIT;
                                    // 击退逻辑
                                    int push_dx = 0, push_dy = 0;
                                    if (p.second.x > m.x) push_dx = 1; else if (p.second.x < m.x) push_dx = -1;
                                    if (p.second.y > m.y) push_dy = 1; else if (p.second.y < m.y) push_dy = -1;
                                    int nx = p.second.x + push_dx;
                                    int ny = p.second.y + push_dy;
                                    if (!is_blocked_by_tower(nx, ny)) { p.second.x = nx; p.second.y = ny; }
                                    
                                    if(p.second.hp <= 0) {
                                        p.second.hp = p.second.max_hp; 
                                        p.second.x = (p.second.color == 1)?22:128; p.second.y = (p.second.color == 1)?128:22;
                                        m.target_id = 0; m.attack_counter = 0; m.boss_state = 0;
                                    }
                                }
                            }
                        }
                    }
                }
                continue; // 正在施法，跳过普攻
            }

            // === 普通攻击逻辑 ===
            int tx=0, ty=0; bool valid=false;
            PlayerState* p = get_player_by_id(m.target_id);
            if (p) { tx = p->x; ty = p->y; valid = true; }
            if (!valid) { m.target_id = 0; continue; }

            int d = dist_sq(m.x, m.y, tx, ty);
            if (d <= m.range * m.range) {
                int cd = (m.type == BOSS_TYPE_OVERLORD) ? OVERLORD_ATK_CD : 
                         (m.type == BOSS_TYPE_TYRANT) ? TYRANT_ATK_CD : MONSTER_ATK_COOLDOWN;
                
                if (now - m.last_attack_time >= cd) {
                    // 触发 Boss 技能 (每3次普攻后)
                    if ((m.type == BOSS_TYPE_OVERLORD || m.type == BOSS_TYPE_TYRANT) && m.attack_counter >= 3) {
                        m.attack_counter = 0;
                        if (m.type == BOSS_TYPE_OVERLORD) {
                            m.boss_state = 1; // PREPARE
                            m.skill_start_time = now;
                            m.skill_targets.clear();
                            // 锁定范围内所有玩家
                            for(auto& pl : players) {
                                if(pl.second.is_playing && dist_sq(m.x, m.y, pl.second.x, pl.second.y) <= OVERLORD_RANGE * OVERLORD_RANGE) {
                                    m.skill_targets.push_back({pl.second.x, pl.second.y});
                                    // 添加预警圈
                                    SkillEffectObj eff = {pl.second.x, pl.second.y, VFX_OVERLORD_WARN, now, now + OVERLORD_SKILL_DELAY, OVERLORD_SKILL_RADIUS, m.id};
                                    active_effects.push_back(eff);
                                }
                            }
                        } 
                        else if (m.type == BOSS_TYPE_TYRANT) {
                            m.boss_state = 2; // ACTIVE
                            m.skill_start_time = now;
                            m.next_tick_time = now + 500;
                        }
                    } 
                    else {
                        // 普通攻击
                        m.last_attack_time = now;
                        m.visual_end_time = now + 200;
                        m.attack_counter++;
                        if (p) { p->hp -= m.dmg; p->current_effect = EFFECT_HIT; }
                    }
                }
            } else {
                // 超出追击范围
                if (d > (m.range + 5) * (m.range + 5)) { m.target_id = 0; m.attack_counter = 0; }
            }
        }
    }
    for(int id : dead_ids) jungle_mobs.erase(id);
}

bool GameRoom::handle_attack_logic(int attacker_fd) {
    PlayerState& att = players[attacker_fd];
    HeroData& tmpl = HERO_DB[att.hero_id];
    int range_sq = (tmpl.range + 1) * (tmpl.range + 1);
    
    int target_id = 0;
    int min_dist = 9999;
    
    // 寻找最近敌人 (Player, Minion, Tower, Jungle)
    for(auto& p : players) {
        if(p.second.color == att.color || !p.second.is_playing) continue;
        int d = dist_sq(att.x, att.y, p.second.x, p.second.y);
        if(d <= range_sq && d < min_dist) { min_dist=d; target_id=p.second.id; }
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
        att.last_aggressive_time = get_current_ms();
        
        long long now = get_current_ms();
        
        // 扣血逻辑
        PlayerState* p = get_player_by_id(target_id);
        if(p) {
            p->hp -= tmpl.dmg;
            p->current_effect = EFFECT_HIT;
             if(p->hp <= 0) {
                p->hp = p->max_hp; 
                p->x = (p->color == 1) ? 22 : 128; p->y = (p->color == 1) ? 128 : 22;
            }
        } else if (target_id >= JUNGLE_ID_START) {
            if (jungle_mobs.count(target_id)) {
                JungleObj& mob = jungle_mobs[target_id];
                mob.hp -= tmpl.dmg;
                mob.target_id = att.id;
                mob.last_hit_by_time = now;
            }
        } else if (target_id >= MINION_ID_START) {
            if (minions.count(target_id)) minions[target_id].hp -= tmpl.dmg;
        } else {
            if (towers.count(target_id)) towers[target_id].hp -= tmpl.dmg;
        }
        return true;
    }
    return false;
}

void GameRoom::broadcast_world(long long now) {
    std::vector<GamePacket> updates;
    
    // Pack Players
    for(auto& pair : players) {
        if(!pair.second.is_playing) continue;
        PlayerState& p = pair.second;
        int atk_target = (now < p.visual_end_time) ? p.current_target_id : 0;
        GamePacket pkt = { TYPE_UPDATE, p.id, p.x, p.y, p.hero_id, 0, p.color, p.hp, p.max_hp, HERO_DB[p.hero_id].range, p.current_effect, atk_target };
        updates.push_back(pkt);
        p.current_effect = EFFECT_NONE;
    }
    // Pack Towers
    for(auto& pair : towers) {
        if(pair.second.hp <= 0) continue;
        TowerObj& t = pair.second;
        int atk_target = (now < t.visual_end_time) ? t.target_id : 0;
        GamePacket pkt = { TYPE_UPDATE, t.id, t.x, t.y, 0, 0, t.team, t.hp, t.max_hp, 0, 0, atk_target };
        updates.push_back(pkt);
    }
    // Pack Minions
    for(auto& pair : minions) {
        MinionObj& m = pair.second;
        int atk_target = (now < m.visual_end_time) ? m.target_id : 0;
        GamePacket pkt = { TYPE_UPDATE, m.id, (int)m.x, (int)m.y, m.type, 0, m.team, m.hp, m.max_hp, 0, 0, atk_target };
        updates.push_back(pkt);
    }
    // Pack Jungle
    for(auto& pair : jungle_mobs) {
        JungleObj& m = pair.second;
        if(m.hp <= 0) continue;
        int atk_target = (now < m.visual_end_time) ? m.target_id : 0;
        GamePacket pkt = { TYPE_UPDATE, m.id, m.x, m.y, m.type, 0, 0, m.hp, m.max_hp, 0, 0, atk_target };
        if (m.type == BOSS_TYPE_TYRANT && m.boss_state == 2) { // ACTIVE
             GamePacket eff = { TYPE_EFFECT, 0, m.x, m.y, VFX_TYRANT_WAVE, 0, 0, 0, 0, TYRANT_RANGE, 0, 0 };
             updates.push_back(eff);
        }
        updates.push_back(pkt);
    }
    // Pack Effects
    for(auto& ef : active_effects) {
        if(now < ef.end_time) {
            GamePacket pkt = { TYPE_EFFECT, 0, ef.x, ef.y, ef.type, 0, 0, 0, 0, ef.radius, 0, 0 };
            updates.push_back(pkt);
        }
    }
    
    GamePacket end_pkt; memset(&end_pkt, 0, sizeof(end_pkt));
    end_pkt.type = TYPE_FRAME;
    end_pkt.extra = (int)((now - game_start_time) / 1000);

    // 发送
    for(auto& pair : players) {
        write(pair.first, updates.data(), updates.size() * sizeof(GamePacket)); 
        write(pair.first, &end_pkt, sizeof(end_pkt));
    }
}

// 辅助工具
PlayerState* GameRoom::get_player_by_id(int id) {
    for(auto& pair : players) {
        if(pair.second.id == id && pair.second.is_playing) return &pair.second;
    }
    return nullptr;
}

bool GameRoom::is_valid_move(int x, int y) {
    if (x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE) return false;
    return (game_map[y][x] != TILE_WALL);
}

bool GameRoom::is_blocked_by_tower(int x, int y) {
    if (!is_valid_move(x, y)) return true;
    int t = game_map[y][x];
    if ((t >= 11 && t <= 23) || t == TILE_BASE) {
        for (auto& pair : towers) {
            if (pair.second.x == x && pair.second.y == y) return (pair.second.hp > 0);
        }
    }
    return false;
}