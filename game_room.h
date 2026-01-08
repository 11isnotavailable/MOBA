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

// [新增] 技能实体结构体 (用于管理法师的多段技能等)
struct SpellObj {
    int id;               // 唯一ID (暂用 global counter 分配)
    int owner_id;         // 释放者 ID
    int stage;            // 阶段: 1, 2, 3
    int x, y;             // 中心坐标
    int dir_x, dir_y;     // 传递给下一段的方向
    int radius;           // 伤害半径
    float dmg_mult;       // 伤害倍率 (1.0 或 2.0)
    
    long long create_time;        // 创建时间
    long long active_time;        // 激活/开始造成伤害的时间 (用于Stage1,2的延迟爆发)
    long long next_stage_time;    // 什么时候生成下一段 (-1表示没有)
    long long end_time;           // 技能彻底消失时间
    
    // 状态标记
    bool next_spawned;    // 下一段是否已生成
    bool dmg_dealt;       // 单次伤害是否已结算 (Stage 1 & 2)
    long long last_dot_time; // 持续伤害上次触发时间 (Stage 3)
};

struct PlayerState {
    int fd;
    int id; // 运行时实体ID
    int x, y;
    
    // [新增] 玩家朝向 (用于技能施放方向)
    int dir_x, dir_y; 

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

    // 战斗属性与经济系统
    int gold;
    int base_def;                       // 基础防御力
    std::vector<int> inventory;         // 物品ID列表
    long long last_regen_passive_time;  // 霸者之装回血计时器

    // 个人战绩
    int kills;
    int deaths;

    // [新增] 技能冷却
    long long last_skill_u_time;
    long long last_skill_i_time;
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
    int status; // 0:Waiting, 1:Picking, 2:Playing
    
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
    
    // 获取指定 fd 玩家在游戏内的实体 ID
    int get_player_id(int fd);

private:
    // === 游戏数据 ===
    int game_map[MAP_SIZE][MAP_SIZE]; 
    long long game_start_time;
    int wave_count;
    int last_spawn_minute;
    
    // 队伍击杀比分
    int team1_kills;
    int team2_kills;

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
    
    // 视觉特效 (Boss等使用)
    std::vector<SkillEffectObj> active_effects; 

    // [新增] 英雄逻辑技能 (法师大招等需要持续判定的技能)
    std::vector<SpellObj> hero_spells;

    // === 内部辅助逻辑 ===
    void init_map_and_units(); 
    void spawn_wave();
    void update_towers(long long now);
    void update_minions(long long now);
    void update_jungle(long long now);
    // [新增] 更新英雄技能逻辑
    void update_spells(long long now);

    bool handle_attack_logic(int attacker_fd);
    void broadcast_world(long long now);

    void start_battle(); 

    // 商店与属性计算逻辑
    void handle_buy_item(int fd, int item_id);
    int get_total_atk(const PlayerState& p);
    int get_total_def(const PlayerState& p);
    int get_total_max_hp(const PlayerState& p);
    bool has_item(const PlayerState& p, int item_id);
    void add_gold(int player_id, int amount);
    
    // 工具
    bool is_valid_move(int x, int y);
    bool is_blocked_by_tower(int x, int y);
    PlayerState* get_player_by_id(int id);
};

#endif