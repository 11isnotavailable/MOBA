#include "game_room.h"
#include "map.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono> 

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

// 英雄属性表 (与 server.cpp 保持一致)
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
    this->status = ROOM_STATUS_WAITING;
    this->game_start_time = 0;
    this->wave_count = 0;
    this->last_spawn_minute = -1;
    
    this->global_id_counter = 1;
    this->tower_id_counter = TOWER_ID_START;
    this->minion_id_counter = MINION_ID_START;
    this->jungle_id_counter = JUNGLE_ID_START;
    this->boss_id_counter = BOSS_ID_START;
    
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
    if (status != ROOM_STATUS_WAITING) return false; 

    PlayerState p;
    p.fd = fd;
    p.name = name;
    p.id = global_id_counter++;
    p.is_ready = false;
    p.is_playing = false;
    p.hero_id = 0; // 未选
    
    // 自动分配空闲座位
    std::vector<int> taken_slots;
    for(auto& pair : players) taken_slots.push_back(pair.second.room_slot);
    for(int i=0; i<10; i++) {
        bool taken = false;
        for(int s : taken_slots) if(s == i) taken = true;
        if(!taken) { p.room_slot = i; break; }
    }
    
    p.color = (p.room_slot < 5) ? 1 : 2;
    players[fd] = p;
    return true;
}

void GameRoom::remove_player(int fd) {
    if (players.count(fd)) players.erase(fd);
}

void GameRoom::set_ready(int fd, bool ready) {
    if (players.count(fd)) players[fd].is_ready = ready;
}

void GameRoom::change_slot(int fd, int target_slot) {
    if (target_slot < 0 || target_slot > 9) return;
    if (players.count(fd) == 0) return;
    for(auto& pair : players) {
        if (pair.second.room_slot == target_slot) return; 
    }
    players[fd].room_slot = target_slot;
    players[fd].color = (target_slot < 5) ? 1 : 2;
}

// 开始游戏 -> 进入选人阶段 (ROOM_STATUS_PICKING)
bool GameRoom::start_game(int fd_requester) {
    if (status != ROOM_STATUS_WAITING) return false;
    
    status = ROOM_STATUS_PICKING; // 进入选人
    
    // 重置所有人的选择
    for(auto& pair : players) {
        pair.second.hero_id = 0; 
    }
    
    // 广播状态更新
    RoomStatePacket pkt = get_room_state_packet();
    for(auto& pair : players) {
        write(pair.first, &pkt, sizeof(pkt));
    }
    return true;
}

// [修改] 真正开始战斗的初始化
void GameRoom::start_battle() {
    status = ROOM_STATUS_PLAYING; 
    game_start_time = get_current_ms();
    
    init_map_and_units(); 
    
    for(auto& pair : players) {
        PlayerState& p = pair.second;
        p.is_playing = true;
        
        // 初始化血量 (使用 get_total_max_hp 确保逻辑一致，虽此时 inventory 为空)
        if (HERO_DB.count(p.hero_id)) {
            p.max_hp = HERO_DB[p.hero_id].base_hp;
        } else {
            p.max_hp = HERO_HP_DEFAULT;
        }
        
        // [新增] 初始化经济与物品
        p.gold = 0;
        p.inventory.clear();
        p.last_regen_passive_time = 0;

        // [新增] 初始化防御力
        if (p.hero_id == HERO_TANK) p.base_def = 120;
        else if (p.hero_id == HERO_WARRIOR) p.base_def = 80;
        else if (p.hero_id == HERO_MAGE) p.base_def = 50;
        else p.base_def = 50;

        p.hp = p.max_hp;
        p.x = (p.color == 1) ? 22 : 128;
        p.y = (p.color == 1) ? 128 : 22;
        p.last_aggressive_time = 0;
        p.visual_end_time = 0;
    }
}

RoomInfo GameRoom::get_room_info() {
    RoomInfo info;
    info.room_id = room_id;
    info.player_count = players.size();
    info.max_player = 10;
    info.status = status; 
    
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
    pkt.status = status;
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
            pkt.slots[s].hero_id = p.hero_id;
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

int GameRoom::get_player_id(int fd) {
    if (players.count(fd)) return players[fd].id;
    return -1;
}

bool GameRoom::is_empty() { return players.empty(); }

// =========================================
// 属性计算与商店辅助函数 (NEW)
// =========================================

int GameRoom::get_total_atk(const PlayerState& p) {
    int atk = HERO_DB[p.hero_id].dmg;
    for (int item : p.inventory) {
        if (item == ITEM_IRON_SWORD) atk += 100;
        if (item == ITEM_LIFESTEAL) atk += 300;
        if (item == ITEM_ARMY_BREAKER) atk += 500;
    }
    return atk;
}

int GameRoom::get_total_def(const PlayerState& p) {
    int def = p.base_def;
    for (int item : p.inventory) {
        if (item == ITEM_CLOTH_ARMOR) def += 50;
        if (item == ITEM_REGEN_ARMOR) def += 200;
    }
    return def;
}

int GameRoom::get_total_max_hp(const PlayerState& p) {
    int hp = HERO_DB[p.hero_id].base_hp;
    for (int item : p.inventory) {
        if (item == ITEM_CLOTH_ARMOR) hp += 500;
        if (item == ITEM_REGEN_ARMOR) hp += 2000;
    }
    return hp;
}

bool GameRoom::has_item(const PlayerState& p, int item_id) {
    for (int id : p.inventory) if (id == item_id) return true;
    return false;
}

void GameRoom::add_gold(int player_id, int amount) {
    PlayerState* p = get_player_by_id(player_id);
    if (p) {
        p->gold += amount;
    }
}

// 购买逻辑处理
void GameRoom::handle_buy_item(int fd, int item_id) {
    if (!players.count(fd)) return;
    PlayerState& p = players[fd];
    if (!p.is_playing) return;

    int cost = 0;
    if (item_id == ITEM_CLOTH_ARMOR || item_id == ITEM_IRON_SWORD) cost = PRICE_NORMAL;
    else if (item_id >= ITEM_LIFESTEAL && item_id <= ITEM_ARMY_BREAKER) cost = PRICE_SPECIAL;
    else return;

    if (p.gold >= cost) {
        p.gold -= cost;
        p.inventory.push_back(item_id);
        
        // 重新计算血量上限，并增加血量
        int old_max = p.max_hp;
        p.max_hp = get_total_max_hp(p);
        if (p.max_hp > old_max) {
            p.hp += (p.max_hp - old_max); 
        }
        std::cout << "[Shop] Player " << p.name << " bought item " << item_id << ". Gold left: " << p.gold << std::endl;
    }
}

// =========================================
// 游戏核心逻辑
// =========================================

void GameRoom::handle_game_packet(int fd, const GamePacket& pkt) {
    if (players.count(fd) == 0) return;
    PlayerState& p = players[fd];

    // === 阶段1：选人 ===
    if (status == ROOM_STATUS_PICKING) { 
        if (pkt.type == TYPE_SELECT) {
            if (HERO_DB.count(pkt.input)) {
                p.hero_id = pkt.input;
                std::cout << "[ROOM] Player " << p.name << " selected " << p.hero_id << std::endl;

                RoomStatePacket state = get_room_state_packet();
                for(auto& pair : players) write(pair.first, &state, sizeof(state));

                bool all_selected = true;
                for(auto& pair : players) if(pair.second.hero_id == 0) all_selected = false;
                
                if(all_selected) {
                    start_battle(); 
                    GamePacket start_pkt; memset(&start_pkt, 0, sizeof(start_pkt));
                    start_pkt.type = TYPE_GAME_START;
                    for(auto& pair : players) {
                        start_pkt.id = pair.second.id;
                        write(pair.first, &start_pkt, sizeof(start_pkt));
                    }
                }
            }
        }
        return;
    }

    // === 阶段2：战斗 ===
    if (status == ROOM_STATUS_PLAYING && p.is_playing) { 
        if (pkt.type == TYPE_MOVE) {
            int dx = pkt.x; int dy = pkt.y;
            if (dx < -1) dx = -1; if (dx > 1) dx = 1;
            if (dy < -1) dy = -1; if (dy > 1) dy = 1;
            int nx = p.x + dx; int ny = p.y + dy;
            if (!is_blocked_by_tower(nx, ny)) { 
                p.x = nx; p.y = ny; 
            }
        }
        else if (pkt.type == TYPE_ATTACK) {
            handle_attack_logic(fd);
        }
        else if (pkt.type == TYPE_SPELL) {
            p.hp += 100; 
            if(p.hp > p.max_hp) p.hp = p.max_hp;
        }
        // [新增] 商店购买
        else if (pkt.type == TYPE_BUY_ITEM) {
            handle_buy_item(fd, pkt.input);
        }
    }
}

void GameRoom::update_logic() {
    if (status != ROOM_STATUS_PLAYING) return; 
    
    long long now = get_current_ms();
    
    // [新增] 物品被动逻辑 (霸者之装回血)
    for (auto& pair : players) {
        PlayerState& p = pair.second;
        if (!p.is_playing) continue;
        if (has_item(p, ITEM_REGEN_ARMOR)) {
            if (now - p.last_regen_passive_time >= 5000) {
                p.hp += 300;
                if (p.hp > p.max_hp) p.hp = p.max_hp;
                p.last_regen_passive_time = now;
            }
        }
    }

    int current_sec = (now - game_start_time) / 1000;
    if (current_sec >= 30 && (current_sec - 30) % 60 == 0) {
        if (current_sec != last_spawn_minute) { 
            spawn_wave();
            last_spawn_minute = current_sec;
        }
    }

    update_towers(now);
    update_minions(now);
    update_jungle(now);
    broadcast_world(now);
}

// =========================================
// 私有辅助逻辑
// =========================================

void GameRoom::init_map_and_units() {
    // 1. Init Towers
    towers.clear();
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
                        if(t==11) tower.max_hp=TOWER_HP_TIER_1; else if(t==12) tower.max_hp=TOWER_HP_TIER_2; else tower.max_hp=TOWER_HP_TIER_3;
                    } else {
                        tower.team = 2;
                        if(t==21) tower.max_hp=TOWER_HP_TIER_1; else if(t==22) tower.max_hp=TOWER_HP_TIER_2; else tower.max_hp=TOWER_HP_TIER_3;
                    }
                }
                tower.hp = tower.max_hp;
                towers[tower.id] = tower;
            }
        }
    }

    // 2. Init Jungle
    jungle_mobs.clear();
    
    // Bosses
    JungleObj overlord;
    overlord.id = boss_id_counter++; overlord.type = BOSS_TYPE_OVERLORD;
    overlord.x = 55; overlord.y = 55; 
    overlord.hp = OVERLORD_HP; overlord.max_hp = OVERLORD_HP;
    overlord.dmg = OVERLORD_DMG; overlord.range = OVERLORD_RANGE;
    overlord.target_id = 0; overlord.last_hit_by_time = 0; overlord.last_attack_time = 0; overlord.last_regen_time = 0;
    overlord.attack_counter = 0; overlord.boss_state = 0; 
    jungle_mobs[overlord.id] = overlord;

    JungleObj tyrant;
    tyrant.id = boss_id_counter++; tyrant.type = BOSS_TYPE_TYRANT;
    tyrant.x = 95; tyrant.y = 95; 
    tyrant.hp = TYRANT_HP; tyrant.max_hp = TYRANT_HP;
    tyrant.dmg = TYRANT_DMG; tyrant.range = TYRANT_RANGE;
    tyrant.target_id = 0; tyrant.last_hit_by_time = 0; tyrant.last_attack_time = 0; tyrant.last_regen_time = 0;
    tyrant.attack_counter = 0; tyrant.boss_state = 0; 
    jungle_mobs[tyrant.id] = tyrant;

    // Normal Mobs
    struct Zone { int x, y, size; int buff_type; };
    std::vector<Zone> zones = {
        {56, 96, 26, MONSTER_TYPE_BLUE}, {68, 28, 26, MONSTER_TYPE_RED}, 
        {28, 62, 26, MONSTER_TYPE_RED}, {96, 62, 26, MONSTER_TYPE_BLUE} 
    };

    for (const auto& z : zones) {
        int cx = z.x + z.size / 2;
        int cy = z.y + z.size / 2;
        
        JungleObj buff;
        buff.id = jungle_id_counter++; buff.type = z.buff_type; 
        buff.x = cx; buff.y = cy;
        buff.hp = MONSTER_BUFF_HP; buff.max_hp = MONSTER_BUFF_HP;
        buff.dmg = MONSTER_BUFF_DMG; buff.range = MONSTER_BUFF_RANGE;
        buff.target_id = 0; buff.last_hit_by_time = 0; buff.last_attack_time = 0; buff.last_regen_time = 0;
        buff.boss_state = 0;
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
        
        // 1. 仇恨机制
        for (auto& p : players) {
            if (!p.second.is_playing || p.second.color == t.team) continue;
            int d = dist_sq(t.x, t.y, p.second.x, p.second.y);
            if (d > tower_range_sq) continue;
            if (now - p.second.last_aggressive_time < 2000) {
                best_target = p.second.id;
                break; 
            }
        }
        // 2. 锁定与索敌
        if (best_target == 0 && t.target_id != 0) {
            bool valid = false;
            int tx = 0, ty = 0;
            PlayerState* p = get_player_by_id(t.target_id);
            if (p) { tx = p->x; ty = p->y; valid = true; }
            else if (minions.count(t.target_id)) {
                if (minions[t.target_id].hp > 0) {
                    tx = (int)minions[t.target_id].x; ty = (int)minions[t.target_id].y; valid = true;
                }
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

        if (best_target != t.target_id) { 
            t.consecutive_hits = 0; 
            t.target_id = best_target; 
        }

        if (t.target_id != 0 && now - t.last_attack_time >= TOWER_ATK_COOLDOWN) {
            t.last_attack_time = now;
            t.visual_end_time = now + 200;
            
            PlayerState* p = get_player_by_id(t.target_id);
            if (p) {
                int base_dmg = TOWER_BASE_DMG_HERO * (int)pow(2, t.consecutive_hits);
                // [修改] 塔打人也计算防御
                int def = get_total_def(*p);
                int final_dmg = base_dmg - def;
                if (final_dmg < 1) final_dmg = 1;
                
                t.consecutive_hits++;
                p->hp -= final_dmg;
                p->current_effect = EFFECT_HIT;
                
                if(p->hp <= 0) { 
                    p->hp = p->max_hp; 
                    p->x = (p->color == 1) ? 22 : 128; p->y = (p->color == 1) ? 128 : 22;
                    t.target_id = 0; t.consecutive_hits = 0;
                }
            } else if (minions.count(t.target_id)) {
                minions[t.target_id].hp -= (TOWER_BASE_DMG_MINION + 100 * wave_count);
            }
        }
    }
}

void GameRoom::update_minions(long long now) {
    std::vector<int> dead_ids;
    const std::vector<Pt>* paths[] = { &PATH_TOP, &PATH_MID, &PATH_BOT };

    for (auto& pair : minions) {
        MinionObj& m = pair.second;
        if (m.hp <= 0) { dead_ids.push_back(m.id); continue; }

        if (m.state == 0) { // MARCHING
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
                m.state = 1; m.target_id = found; 
                m.anchor_x = m.x; m.anchor_y = m.y;
            } else {
                const auto& path = *paths[m.lane];
                if (m.wp_idx >= 0 && m.wp_idx < (int)path.size()) {
                    Pt target = path[m.wp_idx];
                    float dx = target.x - m.x, dy = target.y - m.y;
                    float dist = sqrt(dx*dx + dy*dy);
                    if (dist < 2.0f) {
                        if (m.team == 1 && m.wp_idx < (int)path.size()-1) m.wp_idx++;
                        else if (m.team == 2 && m.wp_idx > 0) m.wp_idx--;
                    } else {
                        m.x += (dx/dist) * MINION_MOVE_SPEED;
                        m.y += (dy/dist) * MINION_MOVE_SPEED;
                    }
                }
            }
        }
        else if (m.state == 1) { // CHASING
            int dist_anchor = dist_sq((int)m.x, (int)m.y, (int)m.anchor_x, (int)m.anchor_y);
            if (dist_anchor > MINION_CHASE_LIMIT * MINION_CHASE_LIMIT) {
                m.state = 2; m.target_id = 0; 
            } else {
                int tx = 0, ty = 0; bool exists = false;
                PlayerState* p = get_player_by_id(m.target_id);
                if (p) { tx = p->x; ty = p->y; exists = true; }
                else if (minions.count(m.target_id) && minions[m.target_id].hp > 0) {
                    tx = (int)minions[m.target_id].x; ty = (int)minions[m.target_id].y; exists = true;
                } else if (towers.count(m.target_id) && towers[m.target_id].hp > 0) {
                    tx = towers[m.target_id].x; ty = towers[m.target_id].y; exists = true;
                }

                if (!exists) m.state = 2; 
                else {
                    int dist_target = dist_sq((int)m.x, (int)m.y, tx, ty);
                    int atk_range_bonus = (m.target_id >= 100 && m.target_id < 1000) ? 2 : 0;
                    if (dist_target <= (m.range + atk_range_bonus) * (m.range + atk_range_bonus)) {
                        if (now - m.last_attack_time >= MINION_ATK_COOLDOWN) {
                            m.last_attack_time = now;
                            m.visual_end_time = now + 200; 
                            
                            // [修改] 小兵打人计算防御
                            if (p) {
                                int def = get_total_def(*p);
                                int dmg = m.dmg - def;
                                if (dmg < 1) dmg = 1;
                                p->hp -= dmg;
                            }
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
        else if (m.state == 2) { // RETURNING
            float dx = m.anchor_x - m.x, dy = m.anchor_y - m.y;
            float dist = sqrt(dx*dx + dy*dy);
            if (dist < 1.0f) m.state = 0; 
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

        if (m.target_id == 0 && m.hp < m.max_hp) {
            if (now - m.last_regen_time >= 1000) {
                m.hp += MONSTER_REGEN_TICK;
                if (m.hp > m.max_hp) m.hp = m.max_hp;
                m.last_regen_time = now;
                m.attack_counter = 0;
                m.boss_state = 0; 
            }
        }
        if (m.target_id != 0 && now - m.last_hit_by_time > MONSTER_AGGRO_TIMEOUT) {
            m.target_id = 0; m.attack_counter = 0; m.boss_state = 0;
        }

        if (m.target_id != 0) {
            if (m.boss_state == 1) { // PREPARE
                if (m.type == BOSS_TYPE_OVERLORD) {
                    if (now - m.skill_start_time >= OVERLORD_SKILL_DELAY) {
                        for (auto& target_pos : m.skill_targets) {
                            SkillEffectObj eff = {target_pos.x, target_pos.y, VFX_OVERLORD_DMG, now, now + 500, OVERLORD_SKILL_RADIUS, m.id};
                            active_effects.push_back(eff);
                            
                            for (auto& p : players) {
                                if (!p.second.is_playing) continue;
                                if (dist_sq(p.second.x, p.second.y, target_pos.x, target_pos.y) <= OVERLORD_SKILL_RADIUS * OVERLORD_SKILL_RADIUS) {
                                    int def = get_total_def(p.second);
                                    int dmg = (m.dmg * 3) - def;
                                    if(dmg < 1) dmg = 1;
                                    
                                    p.second.hp -= dmg;
                                    p.second.current_effect = EFFECT_HIT;
                                    if(p.second.hp <= 0) { 
                                        p.second.hp = p.second.max_hp; 
                                        p.second.x = (p.second.color == 1)?22:128; p.second.y = (p.second.color == 1)?128:22;
                                        m.target_id = 0; m.attack_counter = 0; m.boss_state = 0;
                                    }
                                }
                            }
                        }
                        m.boss_state = 0; 
                        m.last_attack_time = now; 
                    }
                }
                continue; 
            }
            else if (m.boss_state == 2) { // ACTIVE
                if (m.type == BOSS_TYPE_TYRANT) {
                    if (now - m.skill_start_time >= TYRANT_SKILL_DUR) {
                        m.boss_state = 0;
                        m.last_attack_time = now;
                    } else {
                        if (now >= m.next_tick_time) {
                            m.next_tick_time += 500;
                            for (auto& p : players) {
                                if (!p.second.is_playing) continue;
                                int d = dist_sq(m.x, m.y, p.second.x, p.second.y);
                                if (d <= TYRANT_RANGE * TYRANT_RANGE) {
                                    int def = get_total_def(p.second);
                                    int dmg = (m.dmg * 2) - def;
                                    if(dmg < 1) dmg = 1;
                                    
                                    p.second.hp -= dmg;
                                    p.second.current_effect = EFFECT_HIT;
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
                continue; 
            }

            int tx=0, ty=0; bool valid=false;
            PlayerState* p = get_player_by_id(m.target_id);
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
                            m.boss_state = 1; 
                            m.skill_start_time = now;
                            m.skill_targets.clear();
                            for(auto& pl : players) {
                                if(pl.second.is_playing && dist_sq(m.x, m.y, pl.second.x, pl.second.y) <= OVERLORD_RANGE * OVERLORD_RANGE) {
                                    m.skill_targets.push_back({pl.second.x, pl.second.y});
                                    SkillEffectObj eff = {pl.second.x, pl.second.y, VFX_OVERLORD_WARN, now, now + OVERLORD_SKILL_DELAY, OVERLORD_SKILL_RADIUS, m.id};
                                    active_effects.push_back(eff);
                                }
                            }
                        } 
                        else if (m.type == BOSS_TYPE_TYRANT) {
                            m.boss_state = 2; 
                            m.skill_start_time = now;
                            m.next_tick_time = now + 500;
                        }
                    } 
                    else {
                        m.last_attack_time = now;
                        m.visual_end_time = now + 200;
                        m.attack_counter++;
                        if (p) { 
                            int def = get_total_def(*p);
                            int dmg = m.dmg - def;
                            if(dmg < 1) dmg = 1;
                            p->hp -= dmg; 
                            p->current_effect = EFFECT_HIT; 
                        }
                    }
                }
            } else {
                if (d > (m.range + 5) * (m.range + 5)) { m.target_id = 0; m.attack_counter = 0; }
            }
        }
    }
    for(int id : dead_ids) jungle_mobs.erase(id);
}

// [修改] 核心攻击逻辑：应用属性计算、防御力、吸血与金币
bool GameRoom::handle_attack_logic(int attacker_fd) {
    PlayerState& att = players[attacker_fd];
    HeroData& tmpl = HERO_DB[att.hero_id];
    int range_sq = (tmpl.range + 1) * (tmpl.range + 1);
    
    int target_id = 0;
    int min_dist = 9999;
    
    // 索敌
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
        int atk_dmg = get_total_atk(att);
        
        // 吸血 (泣血之刃)
        if (has_item(att, ITEM_LIFESTEAL)) {
            int heal = (int)(atk_dmg * 0.2f);
            att.hp += heal;
            if (att.hp > att.max_hp) att.hp = att.max_hp;
        }
        
        // 结算
        PlayerState* p = get_player_by_id(target_id);
        if(p) {
            int def = get_total_def(*p);
            int final_dmg = atk_dmg - def;
            if(final_dmg < 1) final_dmg = 1;
            
            p->hp -= final_dmg;
            p->current_effect = EFFECT_HIT;
             if(p->hp <= 0) {
                add_gold(att.id, 300); // 击杀英雄金币
                p->hp = p->max_hp; 
                p->x = (p->color == 1) ? 22 : 128; p->y = (p->color == 1) ? 128 : 22;
            }
        } else if (target_id >= JUNGLE_ID_START) {
            if (jungle_mobs.count(target_id)) {
                JungleObj& mob = jungle_mobs[target_id];
                mob.hp -= atk_dmg; // 野怪暂无防御
                mob.target_id = att.id;
                mob.last_hit_by_time = now;
                if(mob.hp <= 0) {
                    int reward = 100;
                    if(mob.type == MONSTER_TYPE_RED || mob.type == MONSTER_TYPE_BLUE) reward = 300;
                    if(mob.type == BOSS_TYPE_OVERLORD || mob.type == BOSS_TYPE_TYRANT) reward = 1000;
                    add_gold(att.id, reward);
                }
            }
        } else if (target_id >= MINION_ID_START) {
            if (minions.count(target_id)) {
                minions[target_id].hp -= atk_dmg;
                if(minions[target_id].hp <= 0) {
                    add_gold(att.id, 80);
                }
            }
        } else {
            if (towers.count(target_id)) towers[target_id].hp -= atk_dmg;
        }
        return true;
    }
    return false;
}

void GameRoom::broadcast_world(long long now) {
    std::vector<GamePacket> updates;
    
    // Pack Players (新增 gold)
    for(auto& pair : players) {
        if(!pair.second.is_playing) continue;
        PlayerState& p = pair.second;
        int atk_target = (now < p.visual_end_time) ? p.current_target_id : 0;
        GamePacket pkt = { TYPE_UPDATE, p.id, p.x, p.y, p.hero_id, 0, p.color, p.hp, p.max_hp, HERO_DB[p.hero_id].range, p.current_effect, atk_target, p.gold };
        updates.push_back(pkt);
        p.current_effect = EFFECT_NONE;
    }
    // Pack Towers
    for(auto& pair : towers) {
        if(pair.second.hp <= 0) continue;
        TowerObj& t = pair.second;
        int atk_target = (now < t.visual_end_time) ? t.target_id : 0;
        GamePacket pkt = { TYPE_UPDATE, t.id, t.x, t.y, 0, 0, t.team, t.hp, t.max_hp, 0, 0, atk_target, 0 };
        updates.push_back(pkt);
    }
    // Pack Minions
    for(auto& pair : minions) {
        MinionObj& m = pair.second;
        int atk_target = (now < m.visual_end_time) ? m.target_id : 0;
        GamePacket pkt = { TYPE_UPDATE, m.id, (int)m.x, (int)m.y, m.type, 0, m.team, m.hp, m.max_hp, 0, 0, atk_target, 0 };
        updates.push_back(pkt);
    }
    // Pack Jungle
    for(auto& pair : jungle_mobs) {
        JungleObj& m = pair.second;
        if(m.hp <= 0) continue;
        int atk_target = (now < m.visual_end_time) ? m.target_id : 0;
        GamePacket pkt = { TYPE_UPDATE, m.id, m.x, m.y, m.type, 0, 0, m.hp, m.max_hp, 0, 0, atk_target, 0 };
        if (m.type == BOSS_TYPE_TYRANT && m.boss_state == 2) { 
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