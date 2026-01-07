#ifndef PROTOCOL_H
#define PROTOCOL_H

#define MAP_SIZE 150 

// === 地图元素 ===
#define TILE_EMPTY   0  
#define TILE_WALL    1  
#define TILE_BASE    2  
#define TILE_RIVER   4  

// === 防御塔专用定义 ===
#define TILE_TOWER_WALL  10 // 防御塔的底座/外壳 (十字架的四肢)

// 蓝方防御塔 (Blue)
#define TILE_TOWER_B_1   11 // 先锋塔 (V)
#define TILE_TOWER_B_2   12 // 中塔   (M)
#define TILE_TOWER_B_3   13 // 高地塔 (H)

// 红方防御塔 (Red)
#define TILE_TOWER_R_1   21 // 先锋塔 (V)
#define TILE_TOWER_R_2   22 // 中塔   (M)
#define TILE_TOWER_R_3   23 // 高地塔 (H)

// 旧定义保留，防止报错，但不再使用
#define TILE_TOWER       3 

// ... 其他定义保持不变 ...
#define HERO_WARRIOR 1
#define HERO_MAGE    2
#define HERO_TANK    3

#define TYPE_MOVE    1
#define TYPE_UPDATE  2
#define TYPE_SELECT  3
#define TYPE_FRAME   4
#define TYPE_ATTACK  5  
#define TYPE_SKILL1  6  
#define TYPE_SKILL2  7  
#define TYPE_SKILL3  8  
#define TYPE_SPELL   9  

#define EFFECT_NONE 0
#define EFFECT_HIT  1 

struct GamePacket {
    int type;       
    int id;         
    int x, y;       
    int hero_id;    
    int input;      
    int color;      
    int hp;         
    int max_hp;     
    int attack_range; 
    int effect;            
    int attack_target_id;  
};

#endif