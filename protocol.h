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

// === 网络包类型 (Type Definitions) ===
#define TYPE_HEARTBEAT     0  // 心跳包(可选)

// 1. 游戏内逻辑 (原有)
#define TYPE_MOVE          1
#define TYPE_UPDATE        2  
#define TYPE_SELECT        3
#define TYPE_FRAME         4  
#define TYPE_ATTACK        5  
#define TYPE_SPELL         6  
#define TYPE_EFFECT        7  

// 2. 用户与大厅逻辑 (新增)
#define TYPE_LOGIN_REQ     10 // 登录请求
#define TYPE_LOGIN_RESP    11 // 登录响应
#define TYPE_REG_REQ       12 // 注册请求
#define TYPE_REG_RESP      13 // 注册响应
#define TYPE_ROOM_LIST_REQ 14 // 请求房间列表
#define TYPE_ROOM_LIST_RESP 15 // 发送房间列表
#define TYPE_CREATE_ROOM   16 // 创建房间
#define TYPE_JOIN_ROOM     17 // 加入房间
#define TYPE_LEAVE_ROOM    18 // 离开/踢出
#define TYPE_MATCH_REQ     19 // 随机匹配请求
#define TYPE_ROOM_UPDATE   20 // 房间内状态更新(座位/准备)
#define TYPE_GAME_START    21 // 游戏开始(加载地图)

// === 响应状态码 ===
#define RET_SUCCESS        0
#define RET_FAIL_PWD       1 // 密码错误
#define RET_FAIL_DUP       2 // 用户已存在/已在线
#define RET_FAIL_NONAME    3 // 用户不存在
#define RET_FAIL_FULL      4 // 房间已满/不存在

// === 特效状态 ===
#define EFFECT_NONE        0
#define EFFECT_HIT         1
// Boss技能特效
#define VFX_OVERLORD_WARN  10 // 浅紫圈 (预警)
#define VFX_OVERLORD_DMG   11 // 喷泉爆发 (伤害)
#define VFX_TYRANT_WAVE    12 // 暴君冲击波

// ==========================================
// [Part 2] 数据包结构定义
// ==========================================

// 1. 游戏内实体同步包 (核心)
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

// 2. 登录/注册包
struct LoginPacket {
    int type;
    char username[32];
    char password[32];
};

struct LoginResponsePacket {
    int type;
    int result;       // RET_SUCCESS 等
    int user_id;      // 分配的运行时ID (非数据库ID)
    char msg[64];     // 错误信息或欢迎语
};

// 3. 房间信息单元 (用于列表显示)
struct RoomInfo {
    int room_id;
    int player_count;
    int max_player;   // 通常是 10
    int status;       // 0:等待中, 1:游戏中
    char owner_name[32];
};

// 4. 房间列表包 (Server -> Client)
// 简化处理：每次发包含最多5个房间的列表，客户端分页或滚动
struct RoomListPacket {
    int type;
    int count;
    RoomInfo rooms[5]; 
};

// 5. 房间控制包 (Client <-> Server)
struct RoomControlPacket {
    int type;         // TYPE_CREATE_ROOM, TYPE_JOIN_ROOM, TYPE_LEAVE_ROOM
    int room_id;
    int slot_index;   // 0-9 (座位号), -1表示自动/无所谓
    int extra_data;   // 1=准备, 0=取消准备 (用于 TYPE_ROOM_UPDATE)
};

// 6. 房间内状态同步 (Server -> Client)
// 包含10个座位的详细信息
struct RoomStatePacket {
    int type;         // TYPE_ROOM_UPDATE
    int room_id;
    struct Slot {
        int is_taken;     // 0:空, 1:有人
        char name[32];
        int is_owner;     // 1: 房主
        int is_ready;     // 1: 已准备
        int team;         // 1: 上方(Team 1), 2: 下方(Team 2)
    } slots[10];
};

// ==========================================
// [Part 3] 游戏数值平衡 (最新版)
// ==========================================

// --- 防御塔配置 ---
#define TOWER_HP_TIER_1       10000
#define TOWER_HP_TIER_2       12000
#define TOWER_HP_TIER_3       15000
#define TOWER_ATK_RANGE       8       
#define TOWER_ATK_COOLDOWN    2000    

// 塔的伤害
#define TOWER_BASE_DMG_MINION 300     
#define TOWER_BASE_DMG_HERO   300     

// --- 小兵配置 ---
#define MINION_MOVE_SPEED     0.01f    
#define MINION_ATK_COOLDOWN   2000    
#define MINION_TYPE_MELEE     1       
#define MINION_TYPE_RANGED    2       

// 近战小兵
#define MELEE_BASE_HP         1000    
#define MELEE_BASE_DMG        100     
#define MELEE_RANGE           1       

// 远程小兵
#define RANGED_BASE_HP        600     
#define RANGED_BASE_DMG       100     
#define RANGED_RANGE          5       

#define MINION_VISION_RANGE   4       
#define MINION_CHASE_LIMIT    10      

// --- 英雄基础数值 ---
#define HERO_HP_DEFAULT       2000
#define HERO_DMG_DEFAULT      500

// --- 野怪数值 ---
#define MONSTER_STD_HP        8000
#define MONSTER_STD_DMG       100
#define MONSTER_STD_RANGE     6
#define MONSTER_BUFF_HP       15000
#define MONSTER_BUFF_DMG      150
#define MONSTER_BUFF_RANGE    7

#define MONSTER_ATK_COOLDOWN  2000    
#define MONSTER_AGGRO_TIMEOUT 5000    // 脱战时间 5秒
#define MONSTER_REGEN_TICK    5000    // 脱战回血 5000/秒

// --- BOSS 数值 (主宰 & 暴君) ---

// 主宰 (Overlord)
#define OVERLORD_HP           60000
#define OVERLORD_DMG          200
#define OVERLORD_ATK_CD       2500
#define OVERLORD_RANGE        8
#define OVERLORD_SKILL_DELAY  1500 
#define OVERLORD_SKILL_RADIUS 4 

// 暴君 (Tyrant)
#define TYRANT_HP             40000
#define TYRANT_DMG            300
#define TYRANT_ATK_CD         2000
#define TYRANT_RANGE          7
#define TYRANT_SKILL_DUR      2000 

#endif