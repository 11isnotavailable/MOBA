#ifndef MAP_H
#define MAP_H

#include <cmath>
#include <algorithm>
#include <cstring>
#include "protocol.h" 

class MapGenerator {
private:
    static int (*_map)[MAP_SIZE];

    static void clear_rect(int x, int y, int w, int h) {
        for(int dy=0; dy<h; dy++) for(int dx=0; dx<w; dx++) {
            int nx = x+dx, ny = y+dy;
            if(nx>=0 && nx<MAP_SIZE && ny>=0 && ny<MAP_SIZE) _map[ny][nx] = TILE_EMPTY;
        }
    }

    static void fill_rect(int x, int y, int w, int h) {
        for(int dy=0; dy<h; dy++) for(int dx=0; dx<w; dx++) {
            int nx = x+dx, ny = y+dy;
            if(nx>=0 && nx<MAP_SIZE && ny>=0 && ny<MAP_SIZE) _map[ny][nx] = TILE_WALL;
        }
    }

    static void carve_path(int x1, int y1, int x2, int y2, int width, int type = TILE_EMPTY) {
        int steps = std::max(abs(x2 - x1), abs(y2 - y1));
        if (steps == 0) return;
        float dx = (float)(x2 - x1) / steps;
        float dy = (float)(y2 - y1) / steps;
        float x = x1, y = y1;
        int offset = width / 2;
        for (int i = 0; i <= steps; i++) {
            int cx = (int)x, cy = (int)y;
            for(int iy = 0; iy < width; iy++) {
                for(int ix = 0; ix < width; ix++) {
                    int wy = iy - offset;
                    int wx = ix - offset;
                    int nx = cx + wx, ny = cy + wy;
                    if(nx >= 1 && nx < MAP_SIZE-1 && ny >= 1 && ny < MAP_SIZE-1) _map[ny][nx] = type;
                }
            }
            x += dx; y += dy;
        }
    }

    // [新增] 智能挖掘：遇到河道自动停止，且不覆盖河道
    static void carve_safe_line(int x1, int y1, int x2, int y2, int width) {
        int steps = std::max(abs(x2 - x1), abs(y2 - y1));
        if (steps == 0) return;
        float dx = (float)(x2 - x1) / steps;
        float dy = (float)(y2 - y1) / steps;
        float x = x1, y = y1;
        int offset = width / 2;

        for (int i = 0; i <= steps; i++) {
            int cx = (int)x, cy = (int)y;
            
            // 核心检测：如果笔触中心碰到了河道，立即停止延伸
            if(cx>=0 && cx<MAP_SIZE && cy>=0 && cy<MAP_SIZE) {
                if (_map[cy][cx] == TILE_RIVER) break; 
            }

            for(int iy = 0; iy < width; iy++) {
                for(int ix = 0; ix < width; ix++) {
                    int wy = iy - offset;
                    int wx = ix - offset;
                    int nx = cx + wx, ny = cy + wy;
                    if(nx >= 1 && nx < MAP_SIZE-1 && ny >= 1 && ny < MAP_SIZE-1) {
                        // 边缘检测：即使中心没碰河道，边缘也不能覆盖河道
                        if (_map[ny][nx] != TILE_RIVER) {
                            _map[ny][nx] = TILE_EMPTY;
                        }
                    }
                }
            }
            x += dx; y += dy;
        }
    }

    static void place_tower(int x, int y, int type_center) {
        if(x>=0 && x<MAP_SIZE && y>=0 && y<MAP_SIZE) _map[y][x] = type_center;
        int dx[] = {0, 0, -1, 1};
        int dy[] = {-1, 1, 0, 0};
        for(int i=0; i<4; i++) {
            int nx = x + dx[i];
            int ny = y + dy[i];
            if(nx>=0 && nx<MAP_SIZE && ny>=0 && ny<MAP_SIZE) {
                if(_map[ny][nx] != type_center) _map[ny][nx] = TILE_TOWER_WALL;
            }
        }
    }

    // 正方形环生成器
    static void create_square_ring(int x, int y, int size, int ring_width) {
        clear_rect(x, y, size, size);
        int inner_size = size - 2 * ring_width;
        if (inner_size > 0) {
            fill_rect(x + ring_width, y + ring_width, inner_size, inner_size);
        }
    }

    // [新增] 贯穿型十字：从中心向四个方向延伸，直到撞墙或指定长度
    static void create_penetrating_cross(int x, int y, int size, int width) {
        int cx = x + size / 2;
        int cy = y + size / 2;
        // 延伸长度：设为 size (即正方形边长)。
        // 之前半径是 size/2，现在翻倍就是 size。
        // 这足以穿透周围的墙壁进入分路，但 safe_line 会保证不进河道。
        int arm_len = size; 

        // 上
        carve_safe_line(cx, cy, cx, cy - arm_len, width);
        // 下
        carve_safe_line(cx, cy, cx, cy + arm_len, width);
        // 左
        carve_safe_line(cx, cy, cx - arm_len, cy, width);
        // 右
        carve_safe_line(cx, cy, cx + arm_len, cy, width);
    }

    static void build_jungle() {
        int size = 26; 
        int width = 4;
        int cross_w = 3;

        // === 1. 下方野区 (South) ===
        create_square_ring(56, 96, size, width);
        create_penetrating_cross(56, 96, size, cross_w);

        // === 2. 上方野区 (North) ===
        create_square_ring(68, 28, size, width);
        create_penetrating_cross(68, 28, size, cross_w);

        // === 3. 左侧野区 (West) ===
        create_square_ring(28, 62, size, width);
        create_penetrating_cross(28, 62, size, cross_w);

        // === 4. 右侧野区 (East) ===
        create_square_ring(96, 62, size, width);
        create_penetrating_cross(96, 62, size, cross_w);
        
        // 注意：之前的显式 carve_path 入口已删除，
        // 因为 penetrating_cross 会自动向外延伸打通墙壁。
    }

public:
    static void init(int game_map[MAP_SIZE][MAP_SIZE]) {
        _map = game_map;

        // 1. 底板全墙
        for(int y=0; y<MAP_SIZE; y++) for(int x=0; x<MAP_SIZE; x++) _map[y][x] = TILE_WALL;

        // 参数
        int top_bot_w = 12;   
        int river_w = 19;     
        int margin = 22;      
        int corner_safe = 25; 

        // 2. 河道
        int river_limit = 13; 
        for(int y=0; y<MAP_SIZE; y++) for(int x=0; x<MAP_SIZE; x++) if (abs(y - x) < river_limit) _map[y][x] = TILE_RIVER;

        // 3. 堵漏 (角落)
        for(int y=0; y<corner_safe; y++) for(int x=0; x<corner_safe; x++) 
            if (x + y < corner_safe * 1.5) _map[y][x] = TILE_WALL;
        for(int y=MAP_SIZE-corner_safe; y<MAP_SIZE; y++) for(int x=MAP_SIZE-corner_safe; x<MAP_SIZE; x++) 
             if (x + y > (MAP_SIZE - corner_safe) * 2 + (corner_safe/2)) _map[y][x] = TILE_WALL;

        // 4. 中路
        for(int y=0; y<MAP_SIZE; y++) {
            for(int x=0; x<MAP_SIZE; x++) {
                if (abs((x + y) - MAP_SIZE) <= 8) { 
                    if (x > corner_safe/2 && x < MAP_SIZE - corner_safe/2)
                        _map[y][x] = TILE_EMPTY;
                }
            }
        }

        // 5. 上路
        carve_path(margin, MAP_SIZE-margin, margin, margin, top_bot_w);
        carve_path(margin, margin, MAP_SIZE-margin, margin, top_bot_w);
        clear_rect(margin - top_bot_w/2, margin - top_bot_w/2, top_bot_w+1, top_bot_w+1);

        // 6. 下路
        carve_path(MAP_SIZE-margin, margin, MAP_SIZE-margin, MAP_SIZE-margin, top_bot_w);
        carve_path(margin, MAP_SIZE-margin, MAP_SIZE-margin, MAP_SIZE-margin, top_bot_w);
        clear_rect(MAP_SIZE-margin - top_bot_w/2, MAP_SIZE-margin - top_bot_w/2, top_bot_w+1, top_bot_w+1);

        // 7. 基地
        int base_size = 20;
        clear_rect(2, MAP_SIZE-base_size-2, base_size, base_size); 
        _map[MAP_SIZE-margin][margin] = TILE_BASE; 
        clear_rect(MAP_SIZE-base_size-2, 2, base_size, base_size); 
        _map[margin][MAP_SIZE-margin] = TILE_BASE; 

        // 8. 挖掘野区 (正方形环 + 贯穿十字)
        build_jungle();

        // 9. 防御塔部署
        int center = MAP_SIZE / 2;
        int spacing_side = top_bot_w * 2; 
        int p_low = center - spacing_side; int p_mid = center; int p_high = center + spacing_side;

        // 边路塔
        place_tower(margin, p_high, TILE_TOWER_B_3); place_tower(margin, p_mid,  TILE_TOWER_B_2); place_tower(margin, p_low,  TILE_TOWER_B_1);
        place_tower(p_low,  MAP_SIZE-margin, TILE_TOWER_B_3); place_tower(p_mid,  MAP_SIZE-margin, TILE_TOWER_B_2); place_tower(p_high, MAP_SIZE-margin, TILE_TOWER_B_1);
        place_tower(p_high, margin, TILE_TOWER_R_3); place_tower(p_mid,  margin, TILE_TOWER_R_2); place_tower(p_low,  margin, TILE_TOWER_R_1);
        place_tower(MAP_SIZE-margin, p_low,  TILE_TOWER_R_3); place_tower(MAP_SIZE-margin, p_mid,  TILE_TOWER_R_2); place_tower(MAP_SIZE-margin, p_high, TILE_TOWER_R_1);

        // 中路塔
        int delta = 13;
        int b_t1_x = 64, b_t1_y = 86;
        place_tower(b_t1_x, b_t1_y, TILE_TOWER_B_1);
        place_tower(b_t1_x - delta, b_t1_y + delta, TILE_TOWER_B_2);
        place_tower(b_t1_x - delta*2, b_t1_y + delta*2, TILE_TOWER_B_3);

        int r_t1_x = 86, r_t1_y = 64;
        place_tower(r_t1_x, r_t1_y, TILE_TOWER_R_1);
        place_tower(r_t1_x + delta, r_t1_y - delta, TILE_TOWER_R_2);
        place_tower(r_t1_x + delta*2, r_t1_y - delta*2, TILE_TOWER_R_3);
    }
};

int (*MapGenerator::_map)[MAP_SIZE] = nullptr;

#endif