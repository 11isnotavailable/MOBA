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
// [数值平衡 5.0] 
// ==========================================

// ID 分段
#define TOWER_ID_START        101
#define MINION_ID_START       10000   

// 防御塔配置
#define TOWER_HP_TIER_1       10000
#define TOWER_HP_TIER_2       12000
#define TOWER_HP_TIER_3       15000
#define TOWER_ATK_RANGE       8       
#define TOWER_ATK_COOLDOWN    2000    // 2秒一次

// 塔的伤害基数
#define TOWER_BASE_DMG_MINION 300     // +100n
#define TOWER_BASE_DMG_HERO   300     // 300 * 2^hits

// 小兵配置
#define MINION_MOVE_SPEED     0.1f    
#define MINION_ATK_COOLDOWN   2000    

#define MINION_TYPE_MELEE     1       
#define MINION_TYPE_RANGED    2       

// 小兵数值公式
// 近战: 1000 + 200n 血, 100 + 150n 攻
#define MELEE_BASE_HP         1000    
#define MELEE_BASE_DMG        100     
#define MELEE_RANGE           1       

// 远程: 600 + 150n 血, 100 + 200n 攻
#define RANGED_BASE_HP        600     
#define RANGED_BASE_DMG       100     
#define RANGED_RANGE          5       

#define MINION_VISION_RANGE   4       
#define MINION_CHASE_LIMIT    10      

// 英雄数值
#define HERO_HP_DEFAULT       2000
#define HERO_DMG_DEFAULT      500

#endif