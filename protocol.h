#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstring>

// ==========================================
// [Part 1] 基础配置与游戏枚举
// ==========================================
#define MAP_SIZE 150 

// === 地图元素 ===
#define TILE_EMPTY   0  
#define TILE_WALL    1  
#define TILE_BASE    2  
#define TILE_RIVER   4  

// === 防御塔类型 ===
#define TILE_TOWER_WALL  10 
#define TILE_TOWER_B_1   11 
#define TILE_TOWER_B_2   12 
#define TILE_TOWER_B_3   13 
#define TILE_TOWER_R_1   21 
#define TILE_TOWER_R_2   22 
#define TILE_TOWER_R_3   23 

// === 英雄类型 ===
#define HERO_WARRIOR 1
#define HERO_MAGE    2
#define HERO_TANK    3

// === 房间状态 (新功能) ===
#define ROOM_STATUS_WAITING 0  //在大厅/房间等待
#define ROOM_STATUS_PICKING 1  // 选人阶段
#define ROOM_STATUS_PLAYING 2  // 游戏进行中

// === 实体 ID 分段 ===
#define TOWER_ID_START        101
#define MINION_ID_START       10000   
#define JUNGLE_ID_START       50000   
#define BOSS_ID_START         90000   

// === 野怪/BOSS类型 ===
#define MONSTER_TYPE_STD      1  
#define MONSTER_TYPE_RED      2  
#define MONSTER_TYPE_BLUE     3  
#define BOSS_TYPE_OVERLORD    4  
#define BOSS_TYPE_TYRANT      5  

// ==========================================
// [Part 2] 网络协议包定义
// ==========================================

// --- 包类型 ---
// 登录/注册
#define TYPE_LOGIN_REQ      10
#define TYPE_LOGIN_RESP     11
#define TYPE_REG_REQ        12
#define TYPE_REG_RESP       13

// 大厅/房间管理
#define TYPE_ROOM_LIST_REQ  20
#define TYPE_ROOM_LIST_RESP 21
#define TYPE_CREATE_ROOM    22
#define TYPE_JOIN_ROOM      23
#define TYPE_LEAVE_ROOM     24
#define TYPE_MATCH_REQ      25
#define TYPE_ROOM_UPDATE    26 // 房间状态更新(包含选人信息)
#define TYPE_GAME_START     27 // 正式开始游戏

// 游戏逻辑
#define TYPE_MOVE           1
#define TYPE_UPDATE         2  
#define TYPE_SELECT         3  // 选人指令
#define TYPE_FRAME          4  
#define TYPE_ATTACK         5  
#define TYPE_SPELL          6  
#define TYPE_EFFECT         7  

// --- 返回码 ---
#define RET_SUCCESS      0
#define RET_FAIL_DUP     1
#define RET_FAIL_PWD     2
#define RET_FAIL_NONAME  3

// --- 特效状态 ---
#define EFFECT_NONE  0
#define EFFECT_HIT   1
// Boss技能特效
#define VFX_OVERLORD_WARN  10 // 浅紫圈 (预警)
#define VFX_OVERLORD_DMG   11 // 喷泉爆发 (伤害)
#define VFX_TYRANT_WAVE    12 // 暴君冲击波

// ------------------------------------------
// 结构体定义
// ------------------------------------------

// 1. 登录包
struct LoginPacket {
    int type; 
    char username[32];
    char password[32];
};

struct LoginResponsePacket {
    int type;
    int result; 
    int user_id;
};

// 2. 房间信息 (列表用)
struct RoomInfo {
    int room_id;
    int player_count;
    int max_player;
    int status; // 0:Waiting, 1:Picking, 2:Playing
    char owner_name[32];
};

struct RoomListPacket {
    int type;
    int count;
    RoomInfo rooms[10]; 
};

// 3. 房间详细状态 (房间内/选人用)
struct RoomSlot {
    int is_taken;
    char name[32];
    int is_ready;
    int is_owner;
    int team;     // 1 or 2
    int hero_id;  // [新增] 0:未选, 1:Warrior, 2:Mage, 3:Tank
};

struct RoomStatePacket {
    int type;      // TYPE_ROOM_UPDATE
    int room_id;
    int status;    // [新增] 房间当前状态 (WAITING/PICKING/PLAYING)
    RoomSlot slots[10];
};

// 4. 房间控制包 (加入/踢人/换位/准备)
struct RoomControlPacket {
    int type;
    int room_id;
    int slot_index; // -1 表示通用操作
    int extra_data; // 如 ready状态
};

// 5. 游戏内数据包
struct GamePacket {
    int type;       
    int id;         
    int x, y;       
    int input;      
    int extra;      
    int color;      
    int hp;
    int max_hp;
    int attack_range;
    int effect;            
    int attack_target_id;  
};

// ==========================================
// [Part 3] 数值平衡配置
// ==========================================

// 野怪数值
#define MONSTER_STD_HP        8000
#define MONSTER_STD_DMG       100
#define MONSTER_STD_RANGE     6
#define MONSTER_BUFF_HP       15000
#define MONSTER_BUFF_DMG      150
#define MONSTER_BUFF_RANGE    7

#define MONSTER_ATK_COOLDOWN  2000    
#define MONSTER_AGGRO_TIMEOUT 5000    
#define MONSTER_REGEN_TICK    5000    

// BOSS 数值
// 主宰: 6w血, 200攻, 2.5s/次
#define OVERLORD_HP           60000
#define OVERLORD_DMG          200
#define OVERLORD_ATK_CD       2500
#define OVERLORD_RANGE        8
#define OVERLORD_SKILL_DELAY  1500 
#define OVERLORD_SKILL_RADIUS 4 

// 暴君: 4w血, 300攻, 2.0s/次
#define TYRANT_HP             40000
#define TYRANT_DMG            300
#define TYRANT_ATK_CD         2000
#define TYRANT_RANGE          7
#define TYRANT_SKILL_DUR      2000 

// 防御塔 & 小兵
#define TOWER_HP_TIER_1       10000
#define TOWER_HP_TIER_2       12000
#define TOWER_HP_TIER_3       15000
#define TOWER_ATK_RANGE       8       
#define TOWER_ATK_COOLDOWN    2000    
#define TOWER_BASE_DMG_MINION 300     
#define TOWER_BASE_DMG_HERO   300     

// [修改] 小兵速度调整
// 服务端 5ms 一帧 (200 FPS)。要每秒走 2 格 -> 2 / 200 = 0.01 格/帧
#define MINION_MOVE_SPEED     0.01f    

#define MINION_ATK_COOLDOWN   2000    
#define MINION_TYPE_MELEE     1       
#define MINION_TYPE_RANGED    2       
#define MELEE_BASE_HP         1000    
#define MELEE_BASE_DMG        100     
#define MELEE_RANGE           1       
#define RANGED_BASE_HP        600     
#define RANGED_BASE_DMG       100     
#define RANGED_RANGE          5       
#define MINION_VISION_RANGE   4       
#define MINION_CHASE_LIMIT    10      

#define HERO_HP_DEFAULT       2000
#define HERO_DMG_DEFAULT      500

#endif