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
#include <chrono> // [新增] 用于精确计时
#include "protocol.h"
#include "map.h" 

// === 全局 ===
int game_map[MAP_SIZE][MAP_SIZE];
std::vector<GamePacket> world_state;
GamePacket my_status;
bool has_my_data = false;
int my_player_id = -1;
int cam_x=0, cam_y=0, current_view_w=0, current_view_h=0;
std::vector<std::string> combat_logs;

// 鼠标/自动移动相关变量
bool is_auto_moving = false;
int target_dest_x = 0;
int target_dest_y = 0;
// 防卡死机制
int last_pos_x = -1, last_pos_y = -1;
int stuck_frames = 0;
// [新增] 发包限流
long long last_auto_move_time = 0;

char debug_msg[128] = "Ready."; 

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

// 简单的客户端地形判断
bool is_walkable(int x, int y) {
    if (x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE) return false;
    int t = game_map[y][x];
    if (t == TILE_WALL || t == TILE_TOWER_WALL || (t >= 11 && t <= 23)) return false;
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

// === 特效绘制 ===
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
        attron(COLOR_PAIR(20) | A_BLINK); 
        mvprintw(sy + UI_TOP_HEIGHT, sx * 2, "><"); 
        attroff(COLOR_PAIR(20) | A_BLINK);
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
                        attron(COLOR_PAIR(final_color)); 
                        mvprintw(sy + ui_offset, sx * 2, "·"); 
                        attroff(COLOR_PAIR(final_color));
                     }
                     else if (tile == TILE_RIVER) {
                        if (base_color_idx == 30) final_color = 33;
                        else if (base_color_idx == 31) final_color = 34; 
                        else if (base_color_idx == 32) final_color = 35;
                        attron(COLOR_PAIR(final_color)); 
                        mvprintw(sy + ui_offset, sx * 2, "·"); 
                        attroff(COLOR_PAIR(final_color));
                     }
                }
            }
        }
    }
}

// 英雄绘制函数
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
    mvprintw(sy + ui_offset, sx * 2, "%s", head);
    mvprintw(sy + ui_offset + 1, sx * 2, "%s", body); 
    attroff(attr);
}

// === 地图渲染 ===
void draw_map() {
    // 渲染前清理屏幕，避免残影
    erase(); 
    
    int ui_offset = UI_TOP_HEIGHT;
    for (int dy = 0; dy < current_view_h; dy++) {
        for (int dx = 0; dx < current_view_w; dx++) {
            int wx = cam_x + dx; int wy = cam_y + dy;
            if (wx >= MAP_SIZE || wy >= MAP_SIZE) continue;
            
            int tile = game_map[wy][wx];
            
            if (tile == TILE_WALL) {
                attron(COLOR_PAIR(4)); mvprintw(dy + ui_offset, dx * 2, "♣ "); attroff(COLOR_PAIR(4));
            }
            else if (tile == TILE_RIVER) {
                attron(COLOR_PAIR(5)); mvprintw(dy + ui_offset, dx * 2, "~~"); attroff(COLOR_PAIR(5));
            }
            else if (tile == TILE_BASE) {
                attron(COLOR_PAIR(7) | A_BOLD); mvprintw(dy + ui_offset, dx * 2, "★ "); attroff(COLOR_PAIR(7) | A_BOLD);
            }
            else if (tile == TILE_TOWER_WALL) {
                attron(COLOR_PAIR(8) | A_BOLD); mvprintw(dy + ui_offset, dx * 2, "##"); attroff(COLOR_PAIR(8) | A_BOLD);
            }
            else if (tile >= TILE_TOWER_B_1 && tile <= TILE_TOWER_R_3) {
                bool is_blue = (tile >= TILE_TOWER_B_1 && tile <= TILE_TOWER_B_3);
                int color = is_blue ? 4 : 2; 
                int range_color = is_blue ? 31 : 32;
                draw_range_circle(wx, wy, 6, range_color);
                const char* label = "?";
                if (tile == TILE_TOWER_B_1 || tile == TILE_TOWER_R_1) label = "V ";
                if (tile == TILE_TOWER_B_2 || tile == TILE_TOWER_R_2) label = "M ";
                if (tile == TILE_TOWER_B_3 || tile == TILE_TOWER_R_3) label = "H ";
                attron(COLOR_PAIR(color) | A_BOLD | A_REVERSE);
                mvprintw(dy + ui_offset, dx * 2, "%s", label); 
                attroff(COLOR_PAIR(color) | A_BOLD | A_REVERSE);
            }
            else {
                attron(COLOR_PAIR(16));
                if((wx+wy) % 9 == 0) mvprintw(dy + ui_offset, dx * 2, "·");
                attroff(COLOR_PAIR(16));
            }
        }
    }
    
    draw_destination_marker();

    if(has_my_data) {
        draw_range_circle(my_status.x, my_status.y, my_status.attack_range, 30);
    }
    for(const auto& p : world_state) {
        if (p.attack_target_id > 0) {
            for(const auto& t : world_state) {
                if (t.id == p.attack_target_id) {
                    draw_laser_line(p.x, p.y, t.x, t.y); break;
                }
            }
        }
    }
    for(const auto& p : world_state) {
        bool is_me = (p.id == my_player_id);
        draw_hero_visual(p.x, p.y, p.hero_id, p.color, is_me, p.effect);
    }
}

// === UI ===
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
    attron(COLOR_PAIR(color));
    mvprintw(y, x, "┌───┐"); mvprintw(y+1, x, "│ %c │", key); mvprintw(y+2, x, "└───┘");
    attroff(COLOR_PAIR(color)); mvprintw(y+3, x, " %s", name);
}
void draw_ui() {
    int H = LINES; int W = COLS; int bottom_y = H - UI_BOTTOM_HEIGHT;
    attron(COLOR_PAIR(10)); mvhline(0, 0, ' ', W); 
    
    // [DEBUG] 显示调试信息在右上角
    mvprintw(0, W - 40, "DEBUG: %s", debug_msg);
    
    mvprintw(0, W - 15, "[Q] QUIT"); attroff(COLOR_PAIR(10));
    attron(COLOR_PAIR(11)); mvhline(bottom_y - 1, 0, ACS_HLINE, W); attroff(COLOR_PAIR(11));
    if (!has_my_data) return;
    attron(COLOR_PAIR(11) | A_BOLD); mvprintw(bottom_y, 2, "HERO INFO"); attroff(COLOR_PAIR(11) | A_BOLD);
    const char* hname = "Unknown";
    if(my_status.hero_id == HERO_WARRIOR) hname = "Warrior (武)";
    if(my_status.hero_id == HERO_MAGE)    hname = "Mage    (法)";
    if(my_status.hero_id == HERO_TANK)    hname = "Tank    (坦)";
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
        init_pair(1, COLOR_GREEN, COLOR_WHITE); 
        bkgd(COLOR_PAIR(1)); 

        init_pair(1, COLOR_GREEN, COLOR_WHITE); 
        init_pair(2, COLOR_RED,   COLOR_WHITE);
        init_pair(3, COLOR_MAGENTA, COLOR_WHITE); 
        init_pair(4, COLOR_BLUE,  COLOR_WHITE); 

        init_pair(5, COLOR_WHITE, COLOR_CYAN); 
        init_pair(6, COLOR_MAGENTA, COLOR_WHITE);
        init_pair(7, COLOR_WHITE, COLOR_RED);  

        init_pair(8, COLOR_BLACK, COLOR_WHITE);

        init_pair(9, COLOR_WHITE, COLOR_BLUE); 
        init_pair(10, COLOR_WHITE, COLOR_BLUE);
        
        init_pair(11, COLOR_BLACK, COLOR_WHITE); 
        init_pair(12, COLOR_GREEN, COLOR_WHITE); 
        init_pair(13, COLOR_RED,   COLOR_WHITE); 
        init_pair(14, COLOR_BLUE,  COLOR_WHITE); 
        init_pair(15, COLOR_WHITE, COLOR_BLACK); 
        
        init_pair(16, COLOR_CYAN, COLOR_WHITE); 

        init_pair(20, COLOR_YELLOW, COLOR_RED);   
        init_pair(21, COLOR_RED,    COLOR_WHITE); 
        
        init_pair(30, COLOR_GREEN, COLOR_WHITE); 
        init_pair(31, COLOR_CYAN,  COLOR_WHITE);  
        init_pair(32, COLOR_RED,   COLOR_WHITE);   
        init_pair(33, COLOR_GREEN, COLOR_CYAN); 
        init_pair(34, COLOR_BLACK, COLOR_CYAN); 
        init_pair(35, COLOR_RED,   COLOR_CYAN);   
    }

    int sel = select_hero_screen();
    if (sel == -1) { endwin(); return 0; }
    GamePacket pkt = {TYPE_SELECT, 0, 0, 0, sel, sel, 0, 0, 0, 0, 0, 0}; write(sock, &pkt, sizeof(pkt));

    timeout(0); // 非阻塞输入
    std::vector<GamePacket> buf; int last_hp = -1;
    add_log("Welcome! Optimized Client.");

    while (true) {
        int dx = 0, dy = 0;
        bool trying_to_move = false;
        long long current_time = get_current_ms();

        // ==========================================
        // 1. 输入处理 (INPUT LOOP)
        // ==========================================
        int ch;
        MEVENT event;
        while ((ch = getch()) != ERR) {
            if (ch == KEY_RESIZE) { erase(); refresh(); continue; } 
            if (ch == 'q') goto end_game;

            // 鼠标处理
            if (ch == KEY_MOUSE) {
                if (getmouse(&event) == OK) {
                    if (event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_DOUBLE_CLICKED)) {
                        int screen_y = event.y - UI_TOP_HEIGHT;
                        int screen_x = event.x / 2;
                        
                        if (screen_y >= 0 && screen_y < current_view_h && screen_x >= 0 && screen_x < current_view_w) {
                            target_dest_x = screen_x + cam_x;
                            target_dest_y = screen_y + cam_y;
                            is_auto_moving = true;
                            last_pos_x = -1; stuck_frames = 0;
                            add_log("[CMD] Moving...");
                        } 
                    }
                }
            }

            // WASD 强制打断
            if (ch == 'w' || ch == 's' || ch == 'a' || ch == 'd') {
                if (is_auto_moving) {
                    is_auto_moving = false; 
                    add_log("[CMD] Manual Override!");
                }
                
                if (ch == 'w') { dy = -1; trying_to_move = true; }
                if (ch == 's') { dy = 1;  trying_to_move = true; }
                if (ch == 'a') { dx = -1; trying_to_move = true; }
                if (ch == 'd') { dx = 1;  trying_to_move = true; }
                sprintf(debug_msg, "Manual Input");
            }

            if (ch == 'j' || ch == 'J') {
                GamePacket atk = {TYPE_ATTACK, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; 
                write(sock, &atk, sizeof(atk));
            }
            if (ch == 'k' || ch == 'K') {
                GamePacket spl = {TYPE_SPELL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; 
                write(sock, &spl, sizeof(spl));
            }
        }

        // ==========================================
        // 2. 移动逻辑与寻路算法 (LOGIC LOOP)
        // ==========================================
        if (trying_to_move) {
            // 键盘操作：零延迟，立即发送
            GamePacket mv = {TYPE_MOVE, 0, dx, dy, 0, 0, 0, 0, 0, 0, 0, 0}; 
            write(sock, &mv, sizeof(mv));
        } 
        else if (is_auto_moving && has_my_data) {
            // [发包限流] 只有当距离上次发包超过 60ms 时才发送
            // 这能防止卡住时瞬间发送几百个包堵死缓冲区
            if (current_time - last_auto_move_time > 60) {
                last_auto_move_time = current_time;

                // [卡死检测]
                if (my_status.x == last_pos_x && my_status.y == last_pos_y) {
                    stuck_frames++;
                    if (stuck_frames > 5) { // 连续5次判定都没动，说明卡死了
                        is_auto_moving = false;
                        add_log("Blocked. Stopped.");
                        sprintf(debug_msg, "Blocked");
                        stuck_frames = 0;
                        continue; 
                    }
                } else {
                    stuck_frames = 0;
                    last_pos_x = my_status.x;
                    last_pos_y = my_status.y;
                }

                int diff_x = target_dest_x - my_status.x;
                int diff_y = target_dest_y - my_status.y;
                
                if (abs(diff_x) <= 1 && abs(diff_y) <= 1) {
                    is_auto_moving = false;
                    sprintf(debug_msg, "Arrived");
                } else {
                    int move_dx = 0, move_dy = 0;
                    bool move_x_first = abs(diff_x) > abs(diff_y);

                    if (move_x_first) {
                        if (diff_x > 0) move_dx = 1; else if (diff_x < 0) move_dx = -1;
                        if (!is_walkable(my_status.x + move_dx, my_status.y)) {
                             move_dx = 0;
                             if (diff_y > 0) move_dy = 1; else if (diff_y < 0) move_dy = -1;
                        }
                    } else {
                        if (diff_y > 0) move_dy = 1; else if (diff_y < 0) move_dy = -1;
                        if (!is_walkable(my_status.x, my_status.y + move_dy)) {
                            move_dy = 0;
                            if (diff_x > 0) move_dx = 1; else if (diff_x < 0) move_dx = -1;
                        }
                    }

                    if (move_dx == 0 && move_dy == 0) {
                        // 实在走投无路，尝试强行移动一步，交给服务器判定
                         if (diff_x > 0) move_dx = 1; else if (diff_x < 0) move_dx = -1;
                         if (diff_y > 0) move_dy = 1; else if (diff_y < 0) move_dy = -1;
                    }

                    GamePacket mv = {TYPE_MOVE, 0, move_dx, move_dy, 0, 0, 0, 0, 0, 0, 0, 0}; 
                    write(sock, &mv, sizeof(mv));
                }
            }
        }

        // ==========================================
        // 3. 网络接收与渲染 (RENDER LOOP)
        // ==========================================
        bool need_render = false;
        
        while(true) {
            GamePacket recv_pkt;
            int n = recv(sock, &recv_pkt, sizeof(recv_pkt), MSG_DONTWAIT);
            if (n <= 0) break;
            
            if (recv_pkt.type == TYPE_FRAME) { 
                world_state = buf; buf.clear();
                has_my_data = false;
                for(const auto& p : world_state) {
                    if (p.id == my_player_id) {
                        my_status = p; has_my_data = true;
                        update_camera(my_status.x, my_status.y);
                        if (last_hp != -1) {
                             if (my_status.hp < last_hp) { 
                             }
                        }
                        last_hp = my_status.hp; break;
                    }
                }
                need_render = true;
            } else if (recv_pkt.type == TYPE_UPDATE) {
                buf.push_back(recv_pkt);
            }
        }
        
        if (need_render) {
            draw_map(); 
            draw_ui();
            refresh();
        }
        
        // 主循环保持高帧率，以响应键盘
        usleep(10000); 
    }

    end_game:
    printf("\033[?1003l\n"); 
    endwin(); close(sock); return 0;
}