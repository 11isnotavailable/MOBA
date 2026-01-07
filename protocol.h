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
#define TYPE_EFFECT  7  

// === 特效状态 ===
#define EFFECT_NONE  0
#define EFFECT_HIT   1
// Boss技能特效
#define VFX_OVERLORD_WARN  10 // 浅紫圈 (预警)
#define VFX_OVERLORD_DMG   11 // 喷泉爆发 (伤害)
#define VFX_TYRANT_WAVE    12 // 暴君冲击波

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
// [数值平衡 8.0 - Boss Adjustments] 
// ==========================================

// ID 分段
#define TOWER_ID_START        101
#define MINION_ID_START       10000   
#define JUNGLE_ID_START       50000   
#define BOSS_ID_START         90000   

// 野怪/BOSS类型
#define MONSTER_TYPE_STD      1  
#define MONSTER_TYPE_RED      2  
#define MONSTER_TYPE_BLUE     3  
#define BOSS_TYPE_OVERLORD    4  
#define BOSS_TYPE_TYRANT      5  

// 野怪数值
#define MONSTER_STD_HP        8000
#define MONSTER_STD_DMG       100
#define MONSTER_STD_RANGE     6
#define MONSTER_BUFF_HP       15000
#define MONSTER_BUFF_DMG      150
#define MONSTER_BUFF_RANGE    7

#define MONSTER_ATK_COOLDOWN  2000    
// [修改] 脱战时间改为 5秒
#define MONSTER_AGGRO_TIMEOUT 5000    
// [修改] 脱战回血改为 5000
#define MONSTER_REGEN_TICK    5000    

// BOSS 数值
// 主宰: 6w血, 200攻, 2.5s/次
#define OVERLORD_HP           60000
#define OVERLORD_DMG          200
#define OVERLORD_ATK_CD       2500
#define OVERLORD_RANGE        8
#define OVERLORD_SKILL_DELAY  1500 
#define OVERLORD_SKILL_RADIUS 4 // 这个用于判定范围，视觉上画喷泉

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

#define MINION_MOVE_SPEED     0.1f    
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