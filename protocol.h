#ifndef PROTOCOL_H
#define PROTOCOL_H

#define MAP_SIZE 150 

// === 地图元素 ===
#define TILE_EMPTY   0  
#define TILE_WALL    1  
#define TILE_BASE    2  
#define TILE_RIVER   4  

// === 防御塔 (地图块定义) ===
#define TILE_TOWER_WALL  10 
#define TILE_TOWER_B_1   11 // 蓝1塔 (10000 HP)
#define TILE_TOWER_B_2   12 // 蓝2塔 (12000 HP)
#define TILE_TOWER_B_3   13 // 蓝3塔 (15000 HP)
#define TILE_TOWER_R_1   21 // 红1塔
#define TILE_TOWER_R_2   22 // 红2塔
#define TILE_TOWER_R_3   23 // 红3塔

// === 英雄 ===
#define HERO_WARRIOR 1
#define HERO_MAGE    2
#define HERO_TANK    3

// === 网络包类型 ===
#define TYPE_MOVE    1
#define TYPE_UPDATE  2  
#define TYPE_SELECT  3
#define TYPE_FRAME   4  
#define TYPE_ATTACK  5  
#define TYPE_SPELL   6  

// === 特效 ===
#define EFFECT_NONE  0
#define EFFECT_HIT   1

// === 数据包 ===
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
// [数值平衡 3.0] 
// ==========================================

// ID 分段规则
// 1-100: 玩家
// 101-200: 防御塔/水晶 (新增)
// 10000+: 小兵
#define TOWER_ID_START        101
#define MINION_ID_START       10000   

// 防御塔血量
#define TOWER_HP_TIER_1       10000
#define TOWER_HP_TIER_2       12000
#define TOWER_HP_TIER_3       15000

// 小兵配置
#define MINION_MOVE_SPEED     0.1f    
#define MINION_ATK_COOLDOWN   2000    

#define MINION_TYPE_MELEE     1       
#define MINION_TYPE_RANGED    2       

#define MELEE_HP              300
#define MELEE_DMG             8
#define MELEE_RANGE           1       

#define RANGED_HP             150
#define RANGED_DMG            15
#define RANGED_RANGE          5       

#define MINION_VISION_RANGE   4       
#define MINION_CHASE_LIMIT    10      

#endif