#include <ncurses.h>
#include <locale.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <chrono> 
#include "protocol.h"
#include "map.h" 

// === 全局 ===
int game_map[MAP_SIZE][MAP_SIZE];
std::vector<GamePacket> world_state;
std::vector<GamePacket> effects_state; 
GamePacket my_status;
bool has_my_data = false;
int my_player_id = -1;
int cam_x=0, cam_y=0, current_view_w=0, current_view_h=0;
std::vector<std::string> combat_logs;
int current_game_time = 0;

// 鼠标/自动移动
bool is_auto_moving = false;
int target_dest_x = 0, target_dest_y = 0;
int last_pos_x = -1, last_pos_y = -1;
int stuck_frames = 0;
long long last_auto_move_time = 0;

const int UI_TOP_HEIGHT = 1;
const int UI_BOTTOM_HEIGHT = 6;

// === 辅助工具 ===
long long get_current_ms() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

void add_log(std::string msg) {
    combat_logs.push_back(msg);
    if (combat_logs.size() > 5) combat_logs.erase(combat_logs.begin());
}

bool is_walkable(int x, int y) {
    if (x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE) return false;
    int t = game_map[y][x];
    if (t == TILE_WALL) return false;
    return true;
}

void update_camera(int hero_x, int hero_y) {
    current_view_w = COLS / 2; 
    current_view_h = LINES - UI_TOP_HEIGHT - UI_BOTTOM_HEIGHT;
    if (current_view_w > MAP_SIZE) current_view_w = MAP_SIZE;
    if (current_view_h > MAP_SIZE) current_view_h = MAP_SIZE;
    cam_x = hero_x - (current_view_w / 2);
    cam_y = hero_y - (current_view_h / 2);
    if (cam_x < 0) cam_x = 0; if (cam_y < 0) cam_y = 0;
    if (cam_x > MAP_SIZE - current_view_w) cam_x = MAP_SIZE - current_view_w;
    if (cam_y > MAP_SIZE - current_view_h) cam_y = MAP_SIZE - current_view_h;
}

// === 绘图函数 ===
void draw_laser_line(int x1, int y1, int x2, int y2) {
    int dx = x2 - x1, dy = y2 - y1;
    int steps = std::max(abs(dx), abs(dy));
    if (steps == 0) return;
    float xInc = dx/(float)steps, yInc = dy/(float)steps;
    float x = x1, y = y1;
    
    attron(COLOR_PAIR(21) | A_BOLD);
    for (int i=0; i<=steps; i++) {
        int wx = (int)round(x), wy = (int)round(y);
        int sx = wx - cam_x, sy = wy - cam_y;
        if (sx>=0 && sx<current_view_w && sy>=0 && sy<current_view_h)
            if (!((wx==x1 && wy==y1) || (wx==x2 && wy==y2)))
                mvprintw(sy + UI_TOP_HEIGHT, sx * 2, "*");
        x += xInc; y += yInc;
    }
    attroff(COLOR_PAIR(21) | A_BOLD);
}

void draw_destination_marker() {
    if (!is_auto_moving) return;
    int sx = target_dest_x - cam_x;
    int sy = target_dest_y - cam_y;
    if (sx >= 0 && sx < current_view_w && sy >= 0 && sy < current_view_h) {
        attron(COLOR_PAIR(20) | A_BLINK); mvprintw(sy + UI_TOP_HEIGHT, sx * 2, "><"); attroff(COLOR_PAIR(20) | A_BLINK);
    }
}

void draw_range_circle(int world_x, int world_y, int range, int base_color_idx) {
    int r = range; 
    int ui_offset = UI_TOP_HEIGHT;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int wx = world_x + dx; int wy = world_y + dy;
            if (dx*dx + dy*dy <= range * range) {
                int sx = wx - cam_x; int sy = wy - cam_y;
                if (sx < 0 || sx >= current_view_w || sy < 0 || sy >= current_view_h) continue;
                if (wx>=0 && wx<MAP_SIZE && wy>=0 && wy<MAP_SIZE) {
                     int tile = game_map[wy][wx];
                     int final_color = base_color_idx; 
                     if (tile == TILE_EMPTY) {
                        attron(COLOR_PAIR(final_color)); mvprintw(sy + ui_offset, sx * 2, "·"); attroff(COLOR_PAIR(final_color));
                     }
                     else if (tile == TILE_RIVER) {
                        if (base_color_idx == 30) final_color = 33;
                        else if (base_color_idx == 31) final_color = 34; 
                        else if (base_color_idx == 32) final_color = 35;
                        attron(COLOR_PAIR(final_color)); mvprintw(sy + ui_offset, sx * 2, "·"); attroff(COLOR_PAIR(final_color));
                     }
                }
            }
        }
    }
}

void draw_hero_visual(int world_x, int world_y, int hero_id, int color_id, bool is_me, int effect) {
    int sx = world_x - cam_x; int sy = world_y - cam_y;
    int ui_offset = UI_TOP_HEIGHT;
    if (sx < 0 || sx >= current_view_w || sy < 0 || sy >= current_view_h) return;
    int attr;
    if (effect == EFFECT_HIT) { attr = COLOR_PAIR(20) | A_BOLD; }
    else { attr = COLOR_PAIR(color_id) | A_BOLD; if (is_me) attr |= A_REVERSE; }
    attron(attr);
    const char *head="?", *body="??";
    switch(hero_id) {
        case HERO_WARRIOR: head="武"; body="]["; break;
        case HERO_MAGE:    head="法"; body="/\\"; break;
        case HERO_TANK:    head="坦"; body="##"; break;
    }
    mvprintw(sy + ui_offset, sx * 2, "%s", head); mvprintw(sy + ui_offset + 1, sx * 2, "%s", body); 
    attroff(attr);
}

void draw_mini_hp_bar(int sx, int sy, int hp, int max_hp, int color_pair) {
    int ui_offset = UI_TOP_HEIGHT;
    if (sx < 0 || sx >= current_view_w || sy < 0 || sy >= current_view_h) return;
    float pct = (float)hp / max_hp; if(pct<0) pct=0; if(pct>1) pct=1;
    int filled = (int)(pct * 5.0f); 
    int screen_x = sx * 2 - 2; 
    attron(COLOR_PAIR(color_pair) | A_BOLD);
    mvprintw(sy + ui_offset, screen_x, "[");
    for(int i=0; i<5; i++) { if(i < filled) addch('='); else addch(' '); }
    addch(']');
    attroff(COLOR_PAIR(color_pair) | A_BOLD);
}

// [新增] 绘制喷泉式技能特效
void draw_effect_fountain(int cx, int cy) {
    int ui_offset = UI_TOP_HEIGHT;
    // 渲染模式：从中心向外，高度降低
    // 中心 (x, y) 高度4 (y-3 到 y)
    // 左右1 (x-1, x+1) 高度2 (y-1 到 y)
    // 左右2 (x-2, x+2) 高度1 (y)
    
    // 结构: (dx, height, color_attr)
    // 41:深紫(中心), 35:紫(中), 40:浅紫(外)
    struct Col { int dx; int h; int color; };
    std::vector<Col> cols = {
        {0, 4, COLOR_PAIR(41) | A_REVERSE | A_BOLD}, 
        {-1, 2, COLOR_PAIR(35) | A_BOLD}, {1, 2, COLOR_PAIR(35) | A_BOLD},
        {-2, 1, COLOR_PAIR(40) | A_BOLD}, {2, 1, COLOR_PAIR(40) | A_BOLD}
    };

    for (const auto& c : cols) {
        int wx = cx + c.dx;
        int sx = wx - cam_x;
        if (sx < 0 || sx >= current_view_w) continue;
        
        for (int h = 0; h < c.h; h++) {
            int wy = cy - h; // 向上延伸
            int sy = wy - cam_y;
            if (sy < 0 || sy >= current_view_h) continue;

            // 绘制
            attron(c.color);
            const char* ch = (h == c.h - 1) ? "^^" : "||"; // 顶端尖锐
            mvprintw(sy + ui_offset, sx * 2, "%s", ch);
            attroff(c.color);
        }
    }
}

void draw_effect_floor(int cx, int cy, int range, int type) {
    if (type == VFX_OVERLORD_DMG) {
        // [新增] 喷泉爆发特效，不再画圆，而是画立体柱
        draw_effect_fountain(cx, cy);
        return;
    }

    int r = range;
    int ui_offset = UI_TOP_HEIGHT;
    int color = 0;
    
    if (type == VFX_OVERLORD_WARN) color = 40; // 浅紫
    else if (type == VFX_TYRANT_WAVE) color = 40; // 浅紫

    int wave_r = -1;
    if (type == VFX_TYRANT_WAVE) {
        long long ms = get_current_ms();
        wave_r = (ms / 150) % (range + 1); 
    }

    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx*dx + dy*dy <= r*r) {
                int wx = cx + dx, wy = cy + dy;
                int sx = wx - cam_x, sy = wy - cam_y;
                if (sx < 0 || sx >= current_view_w || sy < 0 || sy >= current_view_h) continue;
                
                attron(COLOR_PAIR(color));
                char c = '.'; 
                if (type == VFX_TYRANT_WAVE && abs((int)sqrt(dx*dx + dy*dy) - wave_r) < 1) {
                    attron(A_BOLD | A_REVERSE); c = 'O'; 
                }
                mvprintw(sy + ui_offset, sx * 2, "%c ", c);
                if (type == VFX_TYRANT_WAVE && abs((int)sqrt(dx*dx + dy*dy) - wave_r) < 1) attroff(A_BOLD | A_REVERSE);
                attroff(COLOR_PAIR(color));
            }
        }
    }
}

// === 核心渲染函数 ===
void draw_map() {
    erase(); 
    int ui_offset = UI_TOP_HEIGHT;
    
    // --- Layer 1: 静态地形 ---
    for (int dy = 0; dy < current_view_h; dy++) {
        for (int dx = 0; dx < current_view_w; dx++) {
            int wx = cam_x + dx; int wy = cam_y + dy;
            if (wx >= MAP_SIZE || wy >= MAP_SIZE) continue;
            int tile = game_map[wy][wx];
            
            if (tile == TILE_WALL) { attron(COLOR_PAIR(4)); mvprintw(dy + ui_offset, dx * 2, "♣ "); attroff(COLOR_PAIR(4)); }
            else if (tile == TILE_RIVER) { attron(COLOR_PAIR(5)); mvprintw(dy + ui_offset, dx * 2, "~~"); attroff(COLOR_PAIR(5)); }
            else if (tile == TILE_BASE) { attron(COLOR_PAIR(7) | A_BOLD); mvprintw(dy + ui_offset, dx * 2, "★ "); attroff(COLOR_PAIR(7) | A_BOLD); }
            else if (tile == TILE_TOWER_WALL) { attron(COLOR_PAIR(8) | A_BOLD); mvprintw(dy + ui_offset, dx * 2, "##"); attroff(COLOR_PAIR(8) | A_BOLD); }
            else if (tile >= 11 && tile <= 23) { attron(COLOR_PAIR(16)); if((wx+wy)%9==0) mvprintw(dy + ui_offset, dx * 2, "."); attroff(COLOR_PAIR(16)); }
            else { attron(COLOR_PAIR(16)); if((wx+wy) % 9 == 0) mvprintw(dy + ui_offset, dx * 2, "·"); attroff(COLOR_PAIR(16)); }
        }
    }
    
    // [Updated] Layer 1.5: 技能地板特效
    for (const auto& eff : effects_state) {
        draw_effect_floor(eff.x, eff.y, eff.attack_range, eff.input); 
    }

    draw_destination_marker();

    // --- Layer 2: 范围圈 ---
    if(has_my_data) { draw_range_circle(my_status.x, my_status.y, my_status.attack_range, 30); }
    for(const auto& p : world_state) {
        if (p.id >= 101 && p.id < 1000) { 
            int range_color = (p.color == 1) ? 31 : 32;
            draw_range_circle(p.x, p.y, 6, range_color);
        }
    }
    for (int dy = 0; dy < current_view_h; dy++) {
        for (int dx = 0; dx < current_view_w; dx++) {
            int wx = cam_x + dx; int wy = cam_y + dy;
            if (wx < MAP_SIZE && wy < MAP_SIZE && game_map[wy][wx] == TILE_BASE) {
                int range_color = (wx < 75) ? 31 : 32;
                draw_range_circle(wx, wy, 8, range_color);
            }
        }
    }

    // --- Layer 3: 普通单位 ---
    for(const auto& p : world_state) {
        int sx = p.x - cam_x; int sy = p.y - cam_y;
        if (sx < 0 || sx >= current_view_w || sy < 0 || sy >= current_view_h) continue;

        if (p.id >= MINION_ID_START && p.id < JUNGLE_ID_START) { 
            int color = (p.color == 1) ? 36 : 32; 
            char symbol = (p.input == MINION_TYPE_MELEE) ? 'o' : 'i';
            attron(COLOR_PAIR(color) | A_BOLD); mvprintw(sy + ui_offset, sx * 2, "%c ", symbol); attroff(COLOR_PAIR(color) | A_BOLD);
        } 
        else if (p.id >= JUNGLE_ID_START && p.id < 90000) { 
            int hp_color = 20; draw_mini_hp_bar(sx, sy - 1, p.hp, p.max_hp, hp_color);
            if (p.input == MONSTER_TYPE_STD) { attron(COLOR_PAIR(20)|A_BOLD); mvprintw(sy + ui_offset, sx * 2, "(``)"); attroff(COLOR_PAIR(20)|A_BOLD); }
            else {
                int color_pair = (p.input == MONSTER_TYPE_RED) ? 13 : 36;
                attron(COLOR_PAIR(color_pair) | A_REVERSE | A_BOLD);
                char label = (p.input == MONSTER_TYPE_RED) ? 'R' : 'B';
                mvprintw(sy + ui_offset, sx * 2, "%c ", label); 
                mvprintw(sy + ui_offset + 1, sx * 2, "!!");
                attroff(COLOR_PAIR(color_pair) | A_REVERSE | A_BOLD);
            }
        }
        else if (p.id >= 90000) { 
            int hp_color = 12; 
            if (p.input == BOSS_TYPE_OVERLORD) { 
                // [修改] 主宰体型 3x4
                draw_mini_hp_bar(sx, sy - 3, p.hp, p.max_hp, hp_color);
                attron(COLOR_PAIR(35) | A_BOLD); 
                // 绘制: 宽3(x-1,0,1) 高4(y-3,y-2,y-1,y)
                for(int dy = -3; dy <= 0; dy++) {
                    int draw_sy = sy + ui_offset + dy;
                    if (draw_sy >= 0 && draw_sy < LINES) {
                        mvprintw(draw_sy, sx * 2 - 2, dy==-3?"/^^\\":"|{}|");
                    }
                }
                attroff(COLOR_PAIR(35) | A_BOLD);
            } else if (p.input == BOSS_TYPE_TYRANT) { 
                // 暴君体型 4x2
                draw_mini_hp_bar(sx, sy - 2, p.hp, p.max_hp, hp_color);
                attron(COLOR_PAIR(20) | A_BOLD); 
                mvprintw(sy + ui_offset, sx * 2 - 2, "<[TYRANT]>");
                mvprintw(sy + ui_offset + 1, sx * 2 - 2, " /_||_\\ ");
                attroff(COLOR_PAIR(20) | A_BOLD);
            }
        }
        else if (p.id >= 101 && p.id < 1000) { 
            int color = (p.color == 1) ? 4 : 2; 
            int bar_color = (p.color == 1) ? 14 : 13; 
            draw_mini_hp_bar(sx, sy - 2, p.hp, p.max_hp, bar_color);
            const char* label = "?"; if (p.max_hp == 10000) label = "V "; else if (p.max_hp == 12000) label = "M "; else label = "H ";
            attron(COLOR_PAIR(color) | A_BOLD | A_REVERSE); mvprintw(sy + ui_offset, sx * 2, "%s", label); attroff(COLOR_PAIR(color) | A_BOLD | A_REVERSE);
        }
    }

    // --- Layer 4: 英雄 ---
    for(const auto& p : world_state) {
        if (p.id < 100) { 
            bool is_me = (p.id == my_player_id);
            draw_hero_visual(p.x, p.y, p.input, p.color, is_me, p.effect);
        }
    }

    // --- Layer 5: 激光 ---
    for(const auto& p : world_state) {
        if (p.attack_target_id > 0) {
            int tx = 0, ty = 0;
            for(const auto& t : world_state) { if (t.id == p.attack_target_id) { tx = t.x; ty = t.y; break; } }
            if (tx != 0) draw_laser_line(p.x, p.y, tx, ty);
        }
    }
}

void draw_bar(int y, int x, int width, int cur, int max, int color_pair, const char* label) {
    mvprintw(y, x, "%s", label);
    int bar_w = width - 6; float percent = (float)cur / max;
    if(percent < 0) percent = 0; if(percent > 1) percent = 1;
    int fill = (int)(bar_w * percent);
    attron(COLOR_PAIR(11)); mvaddch(y, x+4, '['); attroff(COLOR_PAIR(11));
    attron(COLOR_PAIR(color_pair));
    for(int i=0; i<bar_w; i++) { if(i < fill) addch('='); else addch('-'); }
    attroff(COLOR_PAIR(color_pair));
    attron(COLOR_PAIR(11)); addch(']'); attroff(COLOR_PAIR(11));
    mvprintw(y, x + width + 2, "%d/%d", cur, max);
}
void draw_skill_box(int y, int x, char key, const char* name, int color) {
    attron(COLOR_PAIR(color)); mvprintw(y, x, "┌───┐"); mvprintw(y+1, x, "│ %c │", key); mvprintw(y+2, x, "└───┘"); attroff(COLOR_PAIR(color)); mvprintw(y+3, x, " %s", name);
}
void draw_ui() {
    int H = LINES; int W = COLS; int bottom_y = H - UI_BOTTOM_HEIGHT;
    attron(COLOR_PAIR(10)); mvhline(0, 0, ' ', W); 
    int m = current_game_time / 60; int s = current_game_time % 60;
    attron(COLOR_PAIR(10) | A_BOLD); mvprintw(0, 2, "Time: %02d:%02d", m, s); attroff(COLOR_PAIR(10) | A_BOLD);
    mvprintw(0, W - 15, "[Q] QUIT"); attroff(COLOR_PAIR(10));
    attron(COLOR_PAIR(11)); mvhline(bottom_y - 1, 0, ACS_HLINE, W); attroff(COLOR_PAIR(11));
    if (!has_my_data) return;
    attron(COLOR_PAIR(11) | A_BOLD); mvprintw(bottom_y, 2, "HERO INFO"); attroff(COLOR_PAIR(11) | A_BOLD);
    const char* hname = "Unknown";
    if(my_status.input == HERO_WARRIOR) hname = "Warrior (武)";
    if(my_status.input == HERO_MAGE)    hname = "Mage    (法)";
    if(my_status.input == HERO_TANK)    hname = "Tank    (坦)";
    mvprintw(bottom_y + 1, 2, "%s", hname); mvprintw(bottom_y + 2, 2, "Range: %d", my_status.attack_range);
    int cx = W / 2 - 20;
    draw_bar(bottom_y, cx, 20, my_status.hp, my_status.max_hp, 12, "HP:");
    draw_bar(bottom_y + 1, cx, 20, 100, 100, 14, "MP:");
    int sy = bottom_y + 2;
    draw_skill_box(sy, cx, 'J', "ATK", 15); draw_skill_box(sy, cx+8, 'K', "HEAL", 14);
    draw_skill_box(sy, cx+16, 'U', "SK1", 14); draw_skill_box(sy, cx+24, 'I', "SK2", 14);
    int log_x = W - 35;
    attron(COLOR_PAIR(11)); mvprintw(bottom_y, log_x, "COMBAT LOG"); attroff(COLOR_PAIR(11));
    for(size_t i=0; i<combat_logs.size(); i++) mvprintw(bottom_y + 1 + i, log_x, "> %s", combat_logs[i].c_str());
}

int select_hero_screen() {
    erase(); 
    attron(COLOR_PAIR(4)); box(stdscr, 0, 0); attroff(COLOR_PAIR(4));
    mvprintw(5, 10, "=== Summoner's Rift ===");
    attron(COLOR_PAIR(1)); mvprintw(9, 10, "[1] Warrior (Melee/High DMG)"); attroff(COLOR_PAIR(1));
    attron(COLOR_PAIR(2)); mvprintw(11, 10, "[2] Mage    (Range/Mid DMG)"); attroff(COLOR_PAIR(2));
    attron(COLOR_PAIR(3)); mvprintw(13, 10, "[3] Tank    (Melee/High HP)"); attroff(COLOR_PAIR(3));
    refresh();
    while(true) {
        int ch = getch();
        if (ch == '1') return HERO_WARRIOR; if (ch == '2') return HERO_MAGE; if (ch == '3') return HERO_TANK; if (ch == 'q') return -1;
    }
}

int main() {
    setlocale(LC_ALL, ""); 
    MapGenerator::init(game_map);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in s_addr = {AF_INET, htons(8888)};
    inet_pton(AF_INET, "127.0.0.1", &s_addr.sin_addr);
    if (connect(sock, (struct sockaddr*)&s_addr, sizeof(s_addr)) < 0) return -1;
    GamePacket welcome; if (read(sock, &welcome, sizeof(welcome)) > 0) my_player_id = welcome.id; else return -1;

    initscr(); cbreak(); noecho(); curs_set(0); keypad(stdscr, TRUE);
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    mouseinterval(0); 
    printf("\033[?1003h\n"); 

    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_GREEN, COLOR_WHITE); bkgd(COLOR_PAIR(1)); 
        init_pair(2, COLOR_RED, COLOR_WHITE); init_pair(3, COLOR_MAGENTA, COLOR_WHITE); 
        init_pair(4, COLOR_BLUE, COLOR_WHITE); 
        init_pair(5, COLOR_WHITE, COLOR_CYAN); init_pair(6, COLOR_MAGENTA, COLOR_WHITE);
        init_pair(7, COLOR_WHITE, COLOR_RED); init_pair(8, COLOR_BLACK, COLOR_WHITE);
        init_pair(9, COLOR_WHITE, COLOR_BLUE); init_pair(10, COLOR_WHITE, COLOR_BLUE);
        init_pair(11, COLOR_BLACK, COLOR_WHITE); init_pair(12, COLOR_GREEN, COLOR_WHITE); 
        init_pair(13, COLOR_RED, COLOR_WHITE); init_pair(14, COLOR_BLUE, COLOR_WHITE); 
        init_pair(15, COLOR_WHITE, COLOR_BLACK); init_pair(16, COLOR_CYAN, COLOR_WHITE); 
        init_pair(20, COLOR_YELLOW, COLOR_RED); init_pair(21, COLOR_RED, COLOR_WHITE); 
        
        init_pair(30, COLOR_GREEN, COLOR_WHITE); init_pair(31, COLOR_CYAN, COLOR_WHITE);  
        init_pair(32, COLOR_RED, COLOR_WHITE); init_pair(33, COLOR_GREEN, COLOR_CYAN); 
        init_pair(34, COLOR_BLACK, COLOR_CYAN); init_pair(35, COLOR_RED, COLOR_CYAN);   
        
        init_pair(36, COLOR_BLUE, COLOR_WHITE); 

        // [新增] Boss特效色 (紫)
        init_pair(40, COLOR_MAGENTA, COLOR_BLACK); // 浅紫(地板)
        init_pair(41, COLOR_MAGENTA, COLOR_WHITE); // 深紫
        init_pair(35, COLOR_MAGENTA, COLOR_WHITE); // 主宰身躯
    }

    int sel = select_hero_screen();
    if (sel == -1) { endwin(); return 0; }
    GamePacket pkt = {TYPE_SELECT, 0, 0, 0, sel, sel, 0, 0, 0, 0, 0, 0}; write(sock, &pkt, sizeof(pkt));

    timeout(0); std::vector<GamePacket> buf; int last_hp = -1;
    add_log("Welcome! Optimized Client.");

    while (true) {
        int dx = 0, dy = 0; bool trying_to_move = false;
        long long current_time = get_current_ms();

        int ch; MEVENT event;
        while ((ch = getch()) != ERR) {
            if (ch == KEY_RESIZE) { erase(); refresh(); continue; } 
            if (ch == 'q') goto end_game;
            if (ch == KEY_MOUSE) {
                if (getmouse(&event) == OK) {
                    if (event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_DOUBLE_CLICKED)) {
                        int screen_y = event.y - UI_TOP_HEIGHT; int screen_x = event.x / 2;
                        if (screen_y >= 0 && screen_y < current_view_h && screen_x >= 0 && screen_x < current_view_w) {
                            target_dest_x = screen_x + cam_x; target_dest_y = screen_y + cam_y;
                            is_auto_moving = true; last_pos_x = -1; stuck_frames = 0;
                            add_log("[CMD] Moving...");
                        } 
                    }
                }
            }
            if (ch == 'w' || ch == 's' || ch == 'a' || ch == 'd') {
                if (is_auto_moving) is_auto_moving = false;
                if (ch == 'w') { dy = -1; trying_to_move = true; }
                if (ch == 's') { dy = 1;  trying_to_move = true; }
                if (ch == 'a') { dx = -1; trying_to_move = true; }
                if (ch == 'd') { dx = 1;  trying_to_move = true; }
            }
            if (ch == 'j' || ch == 'J') { GamePacket atk = {TYPE_ATTACK, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; write(sock, &atk, sizeof(atk)); }
            if (ch == 'k' || ch == 'K') { GamePacket spl = {TYPE_SPELL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; write(sock, &spl, sizeof(spl)); }
        }

        if (trying_to_move) { GamePacket mv = {TYPE_MOVE, 0, dx, dy, 0, 0, 0, 0, 0, 0, 0, 0}; write(sock, &mv, sizeof(mv)); } 
        else if (is_auto_moving && has_my_data) {
            if (current_time - last_auto_move_time > 60) {
                last_auto_move_time = current_time;
                if (my_status.x == last_pos_x && my_status.y == last_pos_y) {
                    stuck_frames++; if (stuck_frames > 5) { is_auto_moving = false; stuck_frames = 0; continue; }
                } else { stuck_frames = 0; last_pos_x = my_status.x; last_pos_y = my_status.y; }

                int diff_x = target_dest_x - my_status.x; int diff_y = target_dest_y - my_status.y;
                if (abs(diff_x) <= 1 && abs(diff_y) <= 1) is_auto_moving = false;
                else {
                    int move_dx = 0, move_dy = 0;
                    bool move_x_first = abs(diff_x) > abs(diff_y);
                    if (move_x_first) {
                        if (diff_x > 0) move_dx = 1; else if (diff_x < 0) move_dx = -1;
                        if (!is_walkable(my_status.x + move_dx, my_status.y)) { move_dx = 0; if (diff_y > 0) move_dy = 1; else if (diff_y < 0) move_dy = -1; }
                    } else {
                        if (diff_y > 0) move_dy = 1; else if (diff_y < 0) move_dy = -1;
                        if (!is_walkable(my_status.x, my_status.y + move_dy)) { move_dy = 0; if (diff_x > 0) move_dx = 1; else if (diff_x < 0) move_dx = -1; }
                    }
                    if (move_dx == 0 && move_dy == 0) { if (diff_x > 0) move_dx = 1; else if (diff_x < 0) move_dx = -1; if (diff_y > 0) move_dy = 1; else if (diff_y < 0) move_dy = -1; }
                    GamePacket mv = {TYPE_MOVE, 0, move_dx, move_dy, 0, 0, 0, 0, 0, 0, 0, 0}; write(sock, &mv, sizeof(mv));
                }
            }
        }

        bool need_render = false;
        while(true) {
            GamePacket recv_pkt;
            int n = recv(sock, &recv_pkt, sizeof(recv_pkt), MSG_DONTWAIT);
            if (n <= 0) break;
            if (recv_pkt.type == TYPE_FRAME) { 
                world_state = buf; 
                buf.clear(); 
                effects_state.clear(); 
                std::vector<GamePacket> entities;
                for(const auto& p : world_state) {
                   if (p.type == TYPE_EFFECT) effects_state.push_back(p);
                   else entities.push_back(p);
                }
                world_state = entities; 

                has_my_data = false;
                current_game_time = recv_pkt.extra;
                for(const auto& p : world_state) {
                    if (p.id == my_player_id) { my_status = p; has_my_data = true; update_camera(my_status.x, my_status.y); last_hp = my_status.hp; break; }
                }
                need_render = true;
            } else if (recv_pkt.type == TYPE_UPDATE || recv_pkt.type == TYPE_EFFECT) buf.push_back(recv_pkt);
        }
        
        if (need_render) { draw_map(); draw_ui(); refresh(); }
        usleep(10000); 
    }
    end_game:
    printf("\033[?1003l\n"); endwin(); close(sock); return 0;
}