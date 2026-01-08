#ifndef GAME_ROOM_H
#define GAME_ROOM_H

#include <vector>
#include <map>
#include <iostream>
#include <string>
#include "protocol.h"

// --------------------------------------------------------
// 辅助结构体
// --------------------------------------------------------
struct PlayerState {
    int fd;
    int id; // 运行时实体ID (Entity ID)
    int x, y;
    int hp, max_hp;
    int hero_id;
    int color; // 1:上方, 2:下方
    bool is_playing; 
    int current_effect;
    int current_target_id; 
    long long last_aggressive_time; 
    long long visual_end_time;
    
    // 房间相关
    int room_slot; // 0-9
    bool is_ready;
    std::string name;
};

struct TowerObj {
    int id, x, y, team; 
    int hp, max_hp;
    int target_id;              
    int consecutive_hits;       
    long long last_attack_time; 
    long long visual_end_time;  
};

struct MinionObj {
    int id, team, type; 
    float x, y; 
    int hp, max_hp, dmg, range;
    int lane, wp_idx; 
    int state; // 0:MARCHING, 1:CHASING, 2:RETURNING
    int target_id; 
    float anchor_x, anchor_y; 
    long long last_attack_time;
    long long visual_end_time; 
};

struct JungleObj {
    int id, type; 
    int x, y; 
    int hp, max_hp, dmg, range;
    int target_id; 
    long long last_hit_by_time; 
    long long last_attack_time; 
    long long last_regen_time;  
    long long visual_end_time;  

    // Boss专用
    int attack_counter;         
    int boss_state; // 0:IDLE, 1:PREPARE, 2:ACTIVE
    long long skill_start_time; 
    long long next_tick_time;   
    struct Pt { int x, y; };
    std::vector<Pt> skill_targets; 
};

struct SkillEffectObj {
    int x, y;
    int type; 
    long long start_time;
    long long end_time;
    int radius;
    int owner_id; 
};

// --------------------------------------------------------
// GameRoom 类定义
// --------------------------------------------------------
class GameRoom {
public:
    int room_id;
    int status; // 0: Waiting, 1: Playing
    
    // 构造/析构
    GameRoom(int id, const std::string& owner_name);
    ~GameRoom();

    // === 大厅管理接口 ===
    bool add_player(int fd, const std::string& name);
    void remove_player(int fd);
    void set_ready(int fd, bool ready);
    void change_slot(int fd, int target_slot);
    bool start_game(int fd_requester);
    
    RoomInfo get_room_info();
    RoomStatePacket get_room_state_packet();
    
    // === 游戏逻辑接口 ===
    void handle_game_packet(int fd, const GamePacket& pkt);
    void update_logic();

    bool is_empty();
    std::vector<int> get_player_fds();
    
    // [新增] 获取指定 fd 玩家在游戏内的实体 ID
    int get_player_id(int fd);

private:
    // === 游戏数据 ===
    int game_map[MAP_SIZE][MAP_SIZE]; 
    long long game_start_time;
    int wave_count;
    int last_spawn_minute;
    
    // ID 计数器
    int global_id_counter; 
    int tower_id_counter;
    int minion_id_counter;
    int jungle_id_counter;
    int boss_id_counter;

    // 实体容器
    std::map<int, PlayerState> players; 
    std::map<int, MinionObj> minions;
    std::map<int, TowerObj> towers; 
    std::map<int, JungleObj> jungle_mobs; 
    std::vector<SkillEffectObj> active_effects; 

    // === 内部辅助逻辑 ===
    void init_map_and_units(); 
    void spawn_wave();
    void update_towers(long long now);
    void update_minions(long long now);
    void update_jungle(long long now);
    bool handle_attack_logic(int attacker_fd);
    void broadcast_world(long long now);
    
    // 工具
    bool is_valid_move(int x, int y);
    bool is_blocked_by_tower(int x, int y);
    PlayerState* get_player_by_id(int id);
};

#endif