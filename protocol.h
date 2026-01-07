#ifndef PROTOCOL_H
#define PROTOCOL_H

#define MAP_SIZE 150 

// === 地图元素 ===
#define TILE_EMPTY   0  
#define TILE_WALL    1  
#define TILE_BASE    2  
#define TILE_RIVER   4  

// === 防御塔 ===
#define TILE_TOWER_WALL  10 
#define TILE_TOWER_B_1   11 
#define TILE_TOWER_B_2   12 
#define TILE_TOWER_B_3   13 
#define TILE_TOWER_R_1   21 
#define TILE_TOWER_R_2   22 
#define TILE_TOWER_R_3   23 

// === 英雄 ===
#define HERO_WARRIOR 1
#define HERO_MAGE    2
#define HERO_TANK    3

// === 网络包类型 ===
#define TYPE_MOVE    1
#define TYPE_UPDATE  2  
#define TYPE_SELECT  3
#define TYPE_FRAME   4  // 帧结束包 (input=0, extra=游戏时间秒数)
#define TYPE_ATTACK  5  
#define TYPE_SPELL   6  

// === 特效 ===
#define EFFECT_NONE  0
#define EFFECT_HIT   1

// === 数据包 ===
// 注意字段顺序：type, id, x, y, input, extra...
struct GamePacket {
    int type;       
    int id;         
    int x, y;       
    int input;      // [关键] 对于小兵，这里存 MINION_TYPE (1或2)
    int extra;      // TYPE_FRAME时存时间
    int color;      
    int hp;
    int max_hp;
    int attack_range;
    int effect;            
    int attack_target_id;  
};

// ==========================================
// [数值平衡 2.0] 小兵系统配置
// ==========================================

#define MINION_ID_START       10000   

// 1. 移动速度 (2格/秒)
// 服务器Tick=50ms (每秒20帧)
// 每帧移动 = 2.0 / 20 = 0.1
#define MINION_MOVE_SPEED     0.1f    

// 2. 攻击冷却 (2秒)
#define MINION_ATK_COOLDOWN   2000    

// 3. 基础属性
#define MINION_TYPE_MELEE     1       
#define MINION_TYPE_RANGED    2       

#define MELEE_HP              300
#define MELEE_DMG             8
#define MELEE_RANGE           1       

#define RANGED_HP             150
#define RANGED_DMG            15
#define RANGED_RANGE          5       

// 4. 行为逻辑
#define MINION_VISION_RANGE   4       
#define MINION_CHASE_LIMIT    10      

#endif