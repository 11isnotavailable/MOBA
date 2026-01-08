#include <ncurses.h>
#include <locale.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h> 
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <chrono> 
#include <algorithm>

#include "protocol.h"
#include "map.h" 

// ==========================================
// 1. 全局状态管理 (AppContext)
// ==========================================

enum AppState {
    STATE_LOGIN,
    STATE_LOBBY,
    STATE_ROOM,
    STATE_PICK, // 选人阶段
    STATE_GAME
};

struct AppContext {
    AppState state;
    int sock;
    int my_id; 
    std::string username;
    
    // Login
    char input_user[32]; char input_pass[32]; int input_focus; char login_msg[64];
    
    // Lobby & Room
    std::vector<RoomInfo> room_list; 
    bool is_matching;
    RoomStatePacket current_room; 
    int my_slot_idx; 

    // Hero Selection
    int pick_cursor; // 0:Warrior, 1:Mage, 2:Tank

    // Game Data
    int game_map[MAP_SIZE][MAP_SIZE];
    
    // [双缓冲机制]
    std::vector<GamePacket> world_state;          
    std::vector<GamePacket> effects_state;
    std::vector<GamePacket> pending_world_state;  
    std::vector<GamePacket> pending_effects_state;

    GamePacket my_hero_status;
    bool has_hero_data;
    
    // [新增] 经济系统状态
    int my_gold;
    bool show_shop;

    int cam_x, cam_y;
    int view_w, view_h;
    
    std::vector<std::string> logs;
    int game_time;
    
    // Auto Move
    bool is_auto_moving;
    int target_dest_x, target_dest_y;
    int last_pos_x, last_pos_y;
    int stuck_frames;
    long long last_auto_move_time;
};

// 全局上下文实例
AppContext ctx;

const int UI_TOP_H = 1;
const int UI_BOT_H = 6;

// ==========================================
// 2. 辅助工具
// ==========================================

long long get_ms() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

void add_log(const std::string& msg) {
    ctx.logs.push_back(msg);
    if(ctx.logs.size() > 5) ctx.logs.erase(ctx.logs.begin());
}

bool is_walkable(int x, int y) {
    if (x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE) return false;
    return (ctx.game_map[y][x] != TILE_WALL);
}

void update_camera(int hx, int hy) {
    ctx.view_w = COLS / 2;
    ctx.view_h = LINES - UI_TOP_H - UI_BOT_H;
    
    ctx.cam_x = hx - ctx.view_w / 2;
    ctx.cam_y = hy - ctx.view_h / 2;
    
    if (ctx.cam_x < 0) ctx.cam_x = 0;
    if (ctx.cam_y < 0) ctx.cam_y = 0;
    if (ctx.cam_x > MAP_SIZE - ctx.view_w) ctx.cam_x = MAP_SIZE - ctx.view_w;
    if (ctx.cam_y > MAP_SIZE - ctx.view_h) ctx.cam_y = MAP_SIZE - ctx.view_h;
}

// ==========================================
// 3. 界面绘制 (Login/Lobby/Room/Pick)
// ==========================================

void draw_login() {
    erase();
    int cx = COLS / 2; int cy = LINES / 2;
    attron(A_BOLD); mvprintw(cy - 8, cx - 10, "=== LINUX MOBA ==="); attroff(A_BOLD);
    mvprintw(cy - 4, cx - 15, "Username:"); mvprintw(cy - 2, cx - 15, "Password:");

    attron(COLOR_PAIR(1)); 
    if(ctx.input_focus == 0) attron(A_REVERSE);
    mvprintw(cy - 4, cx - 5, " %-16s ", ctx.input_user);
    if(ctx.input_focus == 0) attroff(A_REVERSE);

    if(ctx.input_focus == 1) attron(A_REVERSE);
    char mask[32]={0}; for(size_t i=0; i<strlen(ctx.input_pass); i++) mask[i]='*';
    mvprintw(cy - 2, cx - 5, " %-16s ", mask);
    if(ctx.input_focus == 1) attroff(A_REVERSE);
    attroff(COLOR_PAIR(1));

    if(ctx.input_focus == 2) attron(A_REVERSE); mvprintw(cy + 1, cx - 10, "[ LOGIN ]"); if(ctx.input_focus == 2) attroff(A_REVERSE);
    if(ctx.input_focus == 3) attron(A_REVERSE); mvprintw(cy + 1, cx + 2, "[ REGISTER ]"); if(ctx.input_focus == 3) attroff(A_REVERSE);

    attron(COLOR_PAIR(2)); mvprintw(cy + 4, cx - 15, "%s", ctx.login_msg); attroff(COLOR_PAIR(2));
    mvprintw(LINES-1, 1, "TAB: Switch | ENTER: Select");
}

void draw_lobby() {
    erase();
    mvprintw(1, 2, "Welcome, %s!  [Lobby]", ctx.username.c_str());
    mvhline(2, 0, ACS_HLINE, COLS);
    mvprintw(4, 4, "ID   Owner           Players   Status");
    for (size_t i = 0; i < ctx.room_list.size(); i++) {
        RoomInfo& r = ctx.room_list[i];
        const char* st = (r.status == ROOM_STATUS_WAITING) ? "Waiting" : ((r.status == ROOM_STATUS_PICKING) ? "Picking" : "Playing");
        mvprintw(6 + i, 4, "#%-3d %-15s %d/10     %s", r.room_id, r.owner_name, r.player_count, st);
    }
    int by = LINES - 4;
    attron(A_BOLD);
    if (ctx.is_matching) { attron(COLOR_PAIR(2) | A_BLINK); mvprintw(by, 2, ">>> MATCHING... <<<"); attroff(COLOR_PAIR(2) | A_BLINK); }
    else mvprintw(by, 2, "[C] Create   [J] Join ID   [M] Match   [R] Refresh   [Q] Quit");
    attroff(A_BOLD);
}

void draw_room() {
    erase();
    mvprintw(1, 2, "Room #%d  User: %s", ctx.current_room.room_id, ctx.username.c_str());
    mvhline(2, 0, ACS_HLINE, COLS);
    int cx = COLS / 2; int start_y = 6;
    
    attron(COLOR_PAIR(4) | A_BOLD); mvprintw(start_y - 2, cx - 20, "=== TEAM BLUE (TOP) ==="); attroff(COLOR_PAIR(4) | A_BOLD);
    for(int i=0; i<5; i++) {
        int x = cx - 30 + i * 14; int y = start_y;
        auto& slot = ctx.current_room.slots[i];
        if (slot.is_taken) {
            if (slot.is_ready) attron(COLOR_PAIR(1)); else attron(COLOR_PAIR(3)); 
            mvprintw(y, x, "+----------+"); mvprintw(y+1, x, "| %-8s |", slot.name); mvprintw(y+2, x, "| %s |", slot.is_owner?"OWNER":(slot.is_ready?"READY":"WAIT")); mvprintw(y+3, x, "+----------+");
            attroff(COLOR_PAIR(1) | COLOR_PAIR(3));
        } else { attron(A_DIM); mvprintw(y, x, "+----------+"); mvprintw(y+1, x, "|  Empty   |"); mvprintw(y+3, x, "+----------+"); attroff(A_DIM); }
    }

    int y2 = start_y + 6;
    attron(COLOR_PAIR(2) | A_BOLD); mvprintw(y2 - 1, cx - 20, "=== TEAM RED (BOT) ==="); attroff(COLOR_PAIR(2) | A_BOLD);
    for(int i=0; i<5; i++) {
        int x = cx - 30 + i * 14; int y = y2 + 1;
        auto& slot = ctx.current_room.slots[i+5];
        if (slot.is_taken) {
            if (slot.is_ready) attron(COLOR_PAIR(1)); else attron(COLOR_PAIR(3)); 
            mvprintw(y, x, "+----------+"); mvprintw(y+1, x, "| %-8s |", slot.name); mvprintw(y+2, x, "| %s |", slot.is_owner?"OWNER":(slot.is_ready?"READY":"WAIT")); mvprintw(y+3, x, "+----------+");
            attroff(COLOR_PAIR(1) | COLOR_PAIR(3));
        } else { attron(A_DIM); mvprintw(y, x, "+----------+"); mvprintw(y+1, x, "|  Empty   |"); mvprintw(y+3, x, "+----------+"); attroff(A_DIM); }
    }
    int by = LINES - 3;
    bool am_i_owner = (ctx.my_slot_idx >= 0) ? ctx.current_room.slots[ctx.my_slot_idx].is_owner : false;
    mvprintw(by, 2, "[Q] Leave Room   [Click] Switch Slot");
    if (am_i_owner) { attron(A_BOLD | A_BLINK); mvprintw(by, 30, "[ENTER] START GAME (Go to Pick)"); attroff(A_BOLD | A_BLINK); }
    else mvprintw(by, 30, "[R] Toggle Ready");
}

void draw_pick_screen() {
    erase();
    int cx = COLS / 2;
    int cy = LINES / 2;

    attron(A_BOLD | COLOR_PAIR(4));
    mvprintw(2, cx - 10, "=== SELECT YOUR HERO ===");
    attroff(A_BOLD | COLOR_PAIR(4));

    // --- 左侧：英雄列表 ---
    int left_x = cx - 25;
    int start_y = cy - 5;
    
    const char* heroes[] = { "1. Warrior (Melee)", "2. Mage    (Range)", "3. Tank    (Shield)" };
    
    attron(COLOR_PAIR(1)); 
    mvprintw(start_y - 2, left_x, "Available Heroes:");
    attroff(COLOR_PAIR(1));

    for (int i = 0; i < 3; i++) {
        if (ctx.pick_cursor == i) attron(A_REVERSE | A_BOLD);
        mvprintw(start_y + i * 2, left_x, "%s", heroes[i]);
        if (ctx.pick_cursor == i) attroff(A_REVERSE | A_BOLD);
    }
    
    // --- 中间：英雄详情 ---
    int mid_x = cx;
    attron(COLOR_PAIR(14));
    mvprintw(start_y - 2, mid_x, "Details:");
    
    const char* name = (ctx.pick_cursor==0?"Warrior":(ctx.pick_cursor==1?"Mage":"Tank"));
    const char* role = (ctx.pick_cursor==0?"Fighter":(ctx.pick_cursor==1?"Caster":"Defender"));
    int hp = (ctx.pick_cursor==0?2000:(ctx.pick_cursor==1?1500:3000));
    int dmg = (ctx.pick_cursor==0?500:(ctx.pick_cursor==1?600:300));
    int def = (ctx.pick_cursor==0?80:(ctx.pick_cursor==1?50:120));
    
    mvprintw(start_y, mid_x,   "Name: %s", name);
    mvprintw(start_y+1, mid_x, "Role: %s", role);
    mvprintw(start_y+2, mid_x, "HP:   %d", hp);
    mvprintw(start_y+3, mid_x, "DMG:  %d", dmg);
    mvprintw(start_y+4, mid_x, "DEF:  %d", def);
    
    // 简易ASCII头像
    mvprintw(start_y+6, mid_x, "Visual:");
    if(ctx.pick_cursor==0) { mvprintw(start_y+7, mid_x, "  O  "); mvprintw(start_y+8, mid_x, " /|\\ "); mvprintw(start_y+9, mid_x, " / \\ "); }
    if(ctx.pick_cursor==1) { mvprintw(start_y+7, mid_x, "  * "); mvprintw(start_y+8, mid_x, " /|\\~"); mvprintw(start_y+9, mid_x, " / \\ "); }
    if(ctx.pick_cursor==2) { mvprintw(start_y+7, mid_x, " [=] "); mvprintw(start_y+8, mid_x, " ||| "); mvprintw(start_y+9, mid_x, " ||| "); }
    attroff(COLOR_PAIR(14));

    // --- 右侧：队友状态 ---
    int right_x = cx + 25;
    mvprintw(start_y - 2, right_x, "--- Team Pick ---");
    
    int start_slot = (ctx.my_slot_idx < 5) ? 0 : 5;
    
    for (int i = 0; i < 5; i++) {
        int slot_idx = start_slot + i;
        auto& slot = ctx.current_room.slots[slot_idx];
        
        mvprintw(start_y + i * 2, right_x, "[%d] ", i+1);
        
        if (slot.is_taken) {
            attron(A_BOLD);
            printw("%-8s", slot.name);
            attroff(A_BOLD);
            
            if (slot.hero_id == 0) {
                attron(A_DIM); printw(" ..."); attroff(A_DIM);
            } else {
                int color = 0;
                const char* hname = "?";
                if(slot.hero_id == 1) { hname="WAR"; color=12; } // Green
                if(slot.hero_id == 2) { hname="MAG"; color=14; } // Blue
                if(slot.hero_id == 3) { hname="TNK"; color=13; } // Red
                
                printw(" ");
                attron(COLOR_PAIR(color)); printw("[%s]", hname); attroff(COLOR_PAIR(color));
            }
            if (slot_idx == ctx.my_slot_idx) printw(" <");
        } else {
            attron(A_DIM); printw("Empty"); attroff(A_DIM);
        }
    }

    int my_choice = ctx.current_room.slots[ctx.my_slot_idx].hero_id;
    
    if (my_choice == 0) {
        mvprintw(LINES - 4, cx - 20, "Controls: [W/S] or [UP/DOWN] to Move, [ENTER] to Lock In");
    } else {
        attron(COLOR_PAIR(2) | A_BLINK | A_BOLD);
        mvprintw(LINES - 4, cx - 15, "LOCKED IN! WAITING FOR OTHERS...");
        attroff(COLOR_PAIR(2) | A_BLINK | A_BOLD);
    }
}

// ==========================================
// 4. 游戏绘制 (Game Scene)
// ==========================================

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

void draw_effect_fountain(int cx, int cy) {
    int ui_offset = UI_TOP_H;
    struct Col { int dx; int h; int color; };
    std::vector<Col> cols = {
        {0, 4, COLOR_PAIR(41) | A_REVERSE | A_BOLD}, 
        {-1, 2, COLOR_PAIR(35) | A_BOLD}, {1, 2, COLOR_PAIR(35) | A_BOLD},
        {-2, 1, COLOR_PAIR(40) | A_BOLD}, {2, 1, COLOR_PAIR(40) | A_BOLD}
    };

    for (const auto& c : cols) {
        int wx = cx + c.dx;
        int sx = wx - ctx.cam_x;
        if (sx < 0 || sx >= ctx.view_w) continue;
        for (int h = 0; h < c.h; h++) {
            int wy = cy - h; // 向上延伸
            int sy = wy - ctx.cam_y;
            if (sy < 0 || sy >= ctx.view_h) continue;
            attron(c.color);
            const char* ch = (h == c.h - 1) ? "^^" : "||"; 
            mvprintw(sy + ui_offset, sx * 2, "%s", ch);
            attroff(c.color);
        }
    }
}

void draw_laser_line(int x1, int y1, int x2, int y2) {
    int dx = x2 - x1, dy = y2 - y1; int steps = std::max(abs(dx), abs(dy)); if (steps == 0) return;
    float xInc = dx/(float)steps, yInc = dy/(float)steps; float x = x1, y = y1;
    attron(COLOR_PAIR(21) | A_BOLD);
    for (int i=0; i<=steps; i++) {
        int wx = (int)round(x), wy = (int)round(y); int sx = wx - ctx.cam_x, sy = wy - ctx.cam_y;
        if (sx>=0 && sx<ctx.view_w && sy>=0 && sy<ctx.view_h) if (!((wx==x1 && wy==y1) || (wx==x2 && wy==y2))) mvprintw(sy + UI_TOP_H, sx * 2, "*");
        x += xInc; y += yInc;
    }
    attroff(COLOR_PAIR(21) | A_BOLD);
}
void draw_mini_hp_bar(int sx, int sy, int hp, int max_hp, int c) {
    if (sx < 0 || sx >= ctx.view_w || sy < 0 || sy >= ctx.view_h) return;
    float pct = (float)hp / max_hp; if(pct<0) pct=0; if(pct>1) pct=1; int filled = (int)(pct * 5.0f); 
    attron(COLOR_PAIR(c) | A_BOLD); mvprintw(sy + UI_TOP_H, sx * 2 - 2, "[");
    for(int i=0; i<5; i++) addch(i<filled ? '=' : ' ');
    addch(']'); attroff(COLOR_PAIR(c) | A_BOLD);
}
void draw_hero_visual(int wx, int wy, int hid, int cid, bool me, int eff) {
    int sx = wx - ctx.cam_x; int sy = wy - ctx.cam_y;
    if (sx < 0 || sx >= ctx.view_w || sy < 0 || sy >= ctx.view_h) return;
    int attr = (eff == EFFECT_HIT) ? (COLOR_PAIR(20) | A_BOLD) : (COLOR_PAIR(cid) | A_BOLD);
    if (me && eff != EFFECT_HIT) attr |= A_REVERSE;
    attron(attr);
    const char *h="?", *b="??";
    if(hid==HERO_WARRIOR){h="武";b="][";} else if(hid==HERO_MAGE){h="法";b="/\\";} else if(hid==HERO_TANK){h="坦";b="##";}
    mvprintw(sy + UI_TOP_H, sx * 2, "%s", h); mvprintw(sy + UI_TOP_H + 1, sx * 2, "%s", b); 
    attroff(attr);
}
void draw_effect_floor(int cx, int cy, int range, int type) {
    if (type == VFX_OVERLORD_DMG) {
        draw_effect_fountain(cx, cy);
        return;
    }

    int r = range; int color = (type==VFX_OVERLORD_WARN || type==VFX_TYRANT_WAVE) ? 40 : 0;
    int wave_r = -1; if (type == VFX_TYRANT_WAVE) wave_r = (get_ms() / 150) % (range + 1); 
    for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {
        if (dx*dx + dy*dy <= r*r) {
            int wx = cx + dx, wy = cy + dy; int sx = wx - ctx.cam_x, sy = wy - ctx.cam_y;
            if (sx < 0 || sx >= ctx.view_w || sy < 0 || sy >= ctx.view_h) continue;
            attron(COLOR_PAIR(color)); char c = '.'; 
            if (type == VFX_TYRANT_WAVE && abs((int)sqrt(dx*dx + dy*dy) - wave_r) < 1) { attron(A_BOLD | A_REVERSE); c = 'O'; }
            mvprintw(sy + UI_TOP_H, sx * 2, "%c ", c);
            if (type == VFX_TYRANT_WAVE && abs((int)sqrt(dx*dx + dy*dy) - wave_r) < 1) attroff(A_BOLD | A_REVERSE);
            attroff(COLOR_PAIR(color));
        }
    }
}
void draw_range_circle(int world_x, int world_y, int range, int base_color_idx) {
    int r = range; 
    for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {
        if (dx*dx + dy*dy <= r * r) {
            int wx = world_x + dx; int wy = world_y + dy;
            int sx = wx - ctx.cam_x; int sy = wy - ctx.cam_y;
            if (sx < 0 || sx >= ctx.view_w || sy < 0 || sy >= ctx.view_h) continue;
            if (wx>=0 && wx<MAP_SIZE && wy>=0 && wy<MAP_SIZE) {
                 int t = ctx.game_map[wy][wx];
                 int fc = base_color_idx; 
                 if (t == TILE_EMPTY) { attron(COLOR_PAIR(fc)); mvprintw(sy + UI_TOP_H, sx * 2, "·"); attroff(COLOR_PAIR(fc)); }
                 else if (t == TILE_RIVER) {
                    if (base_color_idx == 30) fc = 33; else if (base_color_idx == 31) fc = 34; else if (base_color_idx == 32) fc = 35;
                    attron(COLOR_PAIR(fc)); mvprintw(sy + UI_TOP_H, sx * 2, "·"); attroff(COLOR_PAIR(fc));
                 }
            }
        }
    }
}

void draw_game_scene() {
    erase();
    
    // Layer 1: 地形
    for (int dy = 0; dy < ctx.view_h; dy++) for (int dx = 0; dx < ctx.view_w; dx++) {
        int wx = ctx.cam_x + dx; int wy = ctx.cam_y + dy;
        if (wx >= MAP_SIZE || wy >= MAP_SIZE) continue;
        int t = ctx.game_map[wy][wx];
        if (t == TILE_WALL) { attron(COLOR_PAIR(4)); mvprintw(dy + UI_TOP_H, dx * 2, "♣ "); attroff(COLOR_PAIR(4)); }
        else if (t == TILE_RIVER) { attron(COLOR_PAIR(5)); mvprintw(dy + UI_TOP_H, dx * 2, "~~"); attroff(COLOR_PAIR(5)); }
        else if (t == TILE_BASE) { attron(COLOR_PAIR(7) | A_BOLD); mvprintw(dy + UI_TOP_H, dx * 2, "★ "); attroff(COLOR_PAIR(7) | A_BOLD); }
        else if (t == TILE_TOWER_WALL) { attron(COLOR_PAIR(8) | A_BOLD); mvprintw(dy + UI_TOP_H, dx * 2, "##"); attroff(COLOR_PAIR(8) | A_BOLD); }
        else if (t >= 11 && t <= 23) { attron(COLOR_PAIR(16)); if((wx+wy)%9==0) mvprintw(dy + UI_TOP_H, dx * 2, "."); attroff(COLOR_PAIR(16)); }
        else { attron(COLOR_PAIR(16)); if((wx+wy) % 9 == 0) mvprintw(dy + UI_TOP_H, dx * 2, "·"); attroff(COLOR_PAIR(16)); }
    }
    // Layer 2: 特效
    for (const auto& eff : ctx.effects_state) draw_effect_floor(eff.x, eff.y, eff.attack_range, eff.input); 
    
    // Layer 3: UI辅助 (移动标记)
    if (ctx.is_auto_moving) {
        int sx = ctx.target_dest_x - ctx.cam_x; int sy = ctx.target_dest_y - ctx.cam_y;
        if (sx >= 0 && sx < ctx.view_w && sy >= 0 && sy < ctx.view_h) { attron(COLOR_PAIR(20) | A_BLINK); mvprintw(sy + UI_TOP_H, sx * 2, "><"); attroff(COLOR_PAIR(20) | A_BLINK); }
    }
    // Layer 4: 范围圈
    if(ctx.has_hero_data) draw_range_circle(ctx.my_hero_status.x, ctx.my_hero_status.y, ctx.my_hero_status.attack_range, 30);
    for(const auto& p : ctx.world_state) {
        if (p.id >= 101 && p.id < 1000) { int range_color = (p.color == 1) ? 31 : 32; draw_range_circle(p.x, p.y, 6, range_color); }
    }
    
    // Layer 5: 实体绘制
    for(const auto& p : ctx.world_state) {
        int sx = p.x - ctx.cam_x; int sy = p.y - ctx.cam_y;
        if (sx < 0 || sx >= ctx.view_w || sy < 0 || sy >= ctx.view_h) continue;
        
        // --- Minions ---
        if (p.id >= MINION_ID_START && p.id < JUNGLE_ID_START) { 
            int color = (p.color == 1) ? 36 : 32; char symbol = (p.input == MINION_TYPE_MELEE) ? 'o' : 'i';
            attron(COLOR_PAIR(color) | A_BOLD); mvprintw(sy + UI_TOP_H, sx * 2, "%c ", symbol); attroff(COLOR_PAIR(color) | A_BOLD);
        } 
        // --- Jungle & Boss ---
        else if (p.id >= JUNGLE_ID_START && p.id < 90000) { 
            draw_mini_hp_bar(sx, sy - 1, p.hp, p.max_hp, 20);
            
            if (p.input == MONSTER_TYPE_RED) {
                attron(COLOR_PAIR(13) | A_BOLD | A_REVERSE);
                mvprintw(sy + UI_TOP_H, sx * 2, "R "); mvprintw(sy + UI_TOP_H + 1, sx * 2, "!!");
                attroff(COLOR_PAIR(13) | A_BOLD | A_REVERSE);
            }
            else if (p.input == MONSTER_TYPE_BLUE) {
                attron(COLOR_PAIR(36) | A_BOLD | A_REVERSE);
                mvprintw(sy + UI_TOP_H, sx * 2, "B "); mvprintw(sy + UI_TOP_H + 1, sx * 2, "??");
                attroff(COLOR_PAIR(36) | A_BOLD | A_REVERSE);
            }
            else {
                attron(COLOR_PAIR(20)|A_BOLD); mvprintw(sy + UI_TOP_H, sx * 2, "(``)"); attroff(COLOR_PAIR(20)|A_BOLD); 
            }
        } 
        else if (p.id >= 90000) { 
            int hp_color = 12; 
            if (p.input == BOSS_TYPE_OVERLORD) { 
                draw_mini_hp_bar(sx, sy - 3, p.hp, p.max_hp, hp_color);
                attron(COLOR_PAIR(35) | A_BOLD); 
                for(int dy = -3; dy <= 0; dy++) {
                    int draw_sy = sy + UI_TOP_H + dy;
                    if (draw_sy >= 0 && draw_sy < LINES) mvprintw(draw_sy, sx * 2 - 2, dy==-3?"/^^\\":"|{}|");
                }
                attroff(COLOR_PAIR(35) | A_BOLD);
            } else if (p.input == BOSS_TYPE_TYRANT) { 
                draw_mini_hp_bar(sx, sy - 2, p.hp, p.max_hp, hp_color);
                attron(COLOR_PAIR(20) | A_BOLD); 
                mvprintw(sy + UI_TOP_H, sx * 2 - 2, "<[TYRANT]>");
                mvprintw(sy + UI_TOP_H + 1, sx * 2 - 2, " /_||_\\ ");
                attroff(COLOR_PAIR(20) | A_BOLD);
            }
        } 
        // --- Towers ---
        else if (p.id >= 101 && p.id < 1000) { 
            int color = (p.color == 1) ? 4 : 2; draw_mini_hp_bar(sx, sy - 2, p.hp, p.max_hp, (p.color == 1) ? 14 : 13);
            attron(COLOR_PAIR(color) | A_BOLD | A_REVERSE); mvprintw(sy + UI_TOP_H, sx * 2, (p.max_hp > 12000) ? "H " : "T "); attroff(COLOR_PAIR(color) | A_BOLD | A_REVERSE);
        }
    }

    // Layer 6: 英雄 (画在最上层)
    for(const auto& p : ctx.world_state) { if (p.id < 100) draw_hero_visual(p.x, p.y, p.input, p.color, (p.id == ctx.my_id), p.effect); }
    
    // Layer 7: 激光
    for(const auto& p : ctx.world_state) {
        if (p.attack_target_id > 0) {
            int tx = 0, ty = 0; for(const auto& t : ctx.world_state) { if (t.id == p.attack_target_id) { tx = t.x; ty = t.y; break; } }
            if (tx != 0) draw_laser_line(p.x, p.y, tx, ty);
        }
    }
}

// [新增] 绘制商店
void draw_shop() {
    if (!ctx.show_shop) return;
    
    int w = 60, h = 18;
    int x = (COLS - w) / 2;
    int y = (LINES - h) / 2;
    
    // 画背景框
    attron(COLOR_PAIR(15)); // 白底黑字
    for(int i=0; i<h; i++) mvhline(y+i, x, ' ', w);
    
    // 标题与金币
    mvprintw(y+1, x+20, "=== 装备商店 ===");
    mvprintw(y+2, x+2, "当前金币: %d", ctx.my_gold);
    mvhline(y+3, x, ACS_HLINE, w);

    // 普通装备
    mvprintw(y+5, x+2, "[1] 布甲 ($500)");
    mvprintw(y+6, x+6, "防御 +50, 生命 +500");
    
    mvprintw(y+8, x+2, "[2] 铁剑 ($500)");
    mvprintw(y+9, x+6, "攻击 +100");

    // 特殊装备
    mvprintw(y+11, x+2, "[3] 泣血之刃 ($2000)");
    mvprintw(y+12, x+6, "攻击 +300, 吸血 20%%");

    mvprintw(y+13, x+2, "[4] 霸者重装 ($2000)");
    mvprintw(y+14, x+6, "生命+2000, 防御+200, 回血++");

    mvprintw(y+15, x+2, "[5] 破军 ($2000)");
    mvprintw(y+16, x+6, "攻击 +500");
    
    mvprintw(y+17, x+20, "按 B 键关闭商店");

    attroff(COLOR_PAIR(15));
    
    // 简单边框
    attron(COLOR_PAIR(15));
    mvhline(y, x, ACS_HLINE, w);
    mvhline(y+h-1, x, ACS_HLINE, w);
    mvvline(y, x, ACS_VLINE, h);
    mvvline(y, x+w-1, ACS_VLINE, h);
    mvaddch(y, x, ACS_ULCORNER); mvaddch(y, x+w-1, ACS_URCORNER);
    mvaddch(y+h-1, x, ACS_LLCORNER); mvaddch(y+h-1, x+w-1, ACS_LRCORNER);
    attroff(COLOR_PAIR(15));
}

void draw_game_ui() {
    int W = COLS; int bottom_y = LINES - UI_BOT_H;
    
    // 顶部状态条
    attron(COLOR_PAIR(10)); mvhline(0, 0, ' ', W); 
    int m = ctx.game_time / 60; int s = ctx.game_time % 60;
    
    // [修改] 顶部显示金币
    attron(COLOR_PAIR(10) | A_BOLD); 
    mvprintw(0, 2, "Time: %02d:%02d   User: %s   GOLD: %d", m, s, ctx.username.c_str(), ctx.my_gold); 
    attroff(COLOR_PAIR(10) | A_BOLD);
    
    mvprintw(0, W - 15, "[Q] QUIT"); attroff(COLOR_PAIR(10));
    
    // 底部背景线
    attron(COLOR_PAIR(11)); mvhline(bottom_y - 1, 0, ACS_HLINE, W); attroff(COLOR_PAIR(11));
    
    // 英雄信息面板
    if (!ctx.has_hero_data) return;
    
    attron(COLOR_PAIR(11) | A_BOLD); mvprintw(bottom_y, 2, "HERO INFO"); attroff(COLOR_PAIR(11) | A_BOLD);
    const char* hname = "Unknown";
    if(ctx.my_hero_status.input == HERO_WARRIOR) hname = "Warrior (武)";
    if(ctx.my_hero_status.input == HERO_MAGE)    hname = "Mage    (法)";
    if(ctx.my_hero_status.input == HERO_TANK)    hname = "Tank    (坦)";
    mvprintw(bottom_y + 1, 2, "%s", hname); 
    mvprintw(bottom_y + 2, 2, "Range: %d", ctx.my_hero_status.attack_range);
    
    // 绘制血条/蓝条 (居中)
    int cx = W / 2 - 20;
    draw_bar(bottom_y, cx, 20, ctx.my_hero_status.hp, ctx.my_hero_status.max_hp, 12, "HP:");
    draw_bar(bottom_y + 1, cx, 20, 100, 100, 14, "MP:"); 
    
    // 绘制技能框
    int sy = bottom_y + 2;
    draw_skill_box(sy, cx, 'J', "ATK", 15); draw_skill_box(sy, cx+8, 'K', "HEAL", 14);
    draw_skill_box(sy, cx+16, 'U', "SK1", 14); draw_skill_box(sy, cx+24, 'I', "SK2", 14);
    
    // [新增] 商店提示
    attron(COLOR_PAIR(20) | A_BOLD);
    mvprintw(bottom_y + 1, cx + 35, "[B] SHOP");
    attroff(COLOR_PAIR(20) | A_BOLD);

    // 战斗日志 (右侧)
    int log_x = W - 35;
    attron(COLOR_PAIR(11)); mvprintw(bottom_y, log_x, "COMBAT LOG"); attroff(COLOR_PAIR(11));
    for(size_t i=0; i<ctx.logs.size(); i++) mvprintw(bottom_y + 1 + i, log_x, "> %s", ctx.logs[i].c_str());

    // 最后绘制商店图层，保证覆盖在最上层
    draw_shop();
}

// ==========================================
// 5. 核心网络处理
// ==========================================

void process_network() {
    static char buf[10240]; 
    static int buf_len = 0; 
    int n = recv(ctx.sock, buf + buf_len, sizeof(buf) - buf_len, MSG_DONTWAIT);
    if (n > 0) buf_len += n; else if (buf_len == 0) return;

    int ptr = 0;
    while (ptr < buf_len) {
        if (buf_len - ptr < 4) break; 
        int type = *(int*)(buf + ptr);
        size_t pkt_len = 0;

        if (type == TYPE_LOGIN_RESP || type == TYPE_REG_RESP) pkt_len = sizeof(LoginResponsePacket);
        else if (type == TYPE_ROOM_LIST_RESP) pkt_len = sizeof(RoomListPacket);
        else if (type == TYPE_ROOM_UPDATE) pkt_len = sizeof(RoomStatePacket);
        else if (type == TYPE_GAME_START || type == TYPE_FRAME || type == TYPE_UPDATE || type == TYPE_EFFECT) pkt_len = sizeof(GamePacket);
        else if (type >= 10 && type <= 30) pkt_len = sizeof(int); 
        
        if (pkt_len == 0) { ptr = buf_len; break; } 
        if (buf_len - ptr < pkt_len) break; 
        void* pdata = buf + ptr;

        if (ctx.state == STATE_LOGIN) {
            if (type == TYPE_LOGIN_RESP || type == TYPE_REG_RESP) {
                LoginResponsePacket* pkt = (LoginResponsePacket*)pdata;
                if (pkt->result == RET_SUCCESS) {
                    ctx.state = STATE_LOBBY; ctx.my_id = pkt->user_id; ctx.username = ctx.input_user; 
                    int t = TYPE_ROOM_LIST_REQ; write(ctx.sock, &t, sizeof(t));
                } else snprintf(ctx.login_msg, 64, "Error Code: %d", pkt->result);
            }
        } 
        else if (ctx.state == STATE_LOBBY) {
            if (type == TYPE_ROOM_LIST_RESP) {
                RoomListPacket* l = (RoomListPacket*)pdata; 
                ctx.room_list.clear(); for(int i=0; i<l->count; i++) ctx.room_list.push_back(l->rooms[i]); 
            }
            else if (type == TYPE_ROOM_UPDATE) {
                ctx.current_room = *(RoomStatePacket*)pdata; 
                if (ctx.current_room.status == ROOM_STATUS_PICKING) ctx.state = STATE_PICK;
                else ctx.state = STATE_ROOM;
                ctx.is_matching = false; 
            }
        } 
        else if (ctx.state == STATE_ROOM || ctx.state == STATE_PICK) {
            if (type == TYPE_ROOM_UPDATE) { 
                ctx.current_room = *(RoomStatePacket*)pdata; 
                ctx.my_slot_idx = -1; 
                for(int i=0; i<10; i++) if(ctx.current_room.slots[i].is_taken && strcmp(ctx.current_room.slots[i].name, ctx.username.c_str())==0) ctx.my_slot_idx=i;
                
                // 状态跳转
                if (ctx.current_room.status == ROOM_STATUS_PICKING) ctx.state = STATE_PICK;
                else if (ctx.current_room.status == ROOM_STATUS_WAITING) ctx.state = STATE_ROOM;
            } 
            else if (type == TYPE_GAME_START) {
                ctx.state = STATE_GAME; MapGenerator::init(ctx.game_map); 
                GamePacket* gp = (GamePacket*)pdata; ctx.my_id = gp->id; update_camera(0,0); 
                ctx.my_gold = 0; ctx.show_shop = false; // 重置金币和商店
            }
            else if (type == TYPE_ROOM_LIST_RESP) {
                ctx.state = STATE_LOBBY; RoomListPacket* l = (RoomListPacket*)pdata; 
                ctx.room_list.clear(); for(int i=0; i<l->count; i++) ctx.room_list.push_back(l->rooms[i]); 
            }
        } 
        else if (ctx.state == STATE_GAME) {
            GamePacket* pkt = (GamePacket*)pdata;
            if (type == TYPE_FRAME) { 
                ctx.game_time = pkt->extra; 
                ctx.world_state = ctx.pending_world_state; 
                ctx.effects_state = ctx.pending_effects_state;
                ctx.has_hero_data = false;
                
                int old_x = ctx.my_hero_status.x; 
                int old_y = ctx.my_hero_status.y;
                
                for(const auto& p : ctx.world_state) {
                    if (p.id == ctx.my_id) { 
                        ctx.my_hero_status = p; ctx.has_hero_data = true; 
                        
                        int dist_sq = (p.x - old_x)*(p.x - old_x) + (p.y - old_y)*(p.y - old_y);
                        if (dist_sq > 200) { 
                            ctx.is_auto_moving = false;
                            update_camera(p.x, p.y);
                        } else {
                            update_camera(p.x, p.y); 
                        }
                        break; 
                    }
                }
                ctx.pending_world_state.clear(); ctx.pending_effects_state.clear();
            }
            else if (type == TYPE_UPDATE) { 
                ctx.pending_world_state.push_back(*pkt); 
                // [新增] 同步金币
                if (pkt->id == ctx.my_id) {
                    ctx.my_gold = pkt->gold;
                }
            }
            else if (type == TYPE_EFFECT) { ctx.pending_effects_state.push_back(*pkt); }
        }
        ptr += pkt_len;
    }
    if (ptr > 0) {
        if (ptr < buf_len) memmove(buf, buf + ptr, buf_len - ptr);
        buf_len -= ptr;
    }
}

// ==========================================
// 6. 输入处理与自动移动
// ==========================================

void update_auto_move() {
    if (!ctx.is_auto_moving || !ctx.has_hero_data) return;
    long long now = get_ms();
    if (now - ctx.last_auto_move_time < 60) return; 
    ctx.last_auto_move_time = now;

    int mx = ctx.my_hero_status.x; int my = ctx.my_hero_status.y;
    if (mx == ctx.last_pos_x && my == ctx.last_pos_y) { if (++ctx.stuck_frames > 5) ctx.is_auto_moving = false; }
    else { ctx.stuck_frames = 0; ctx.last_pos_x = mx; ctx.last_pos_y = my; }

    int diff_x = ctx.target_dest_x - mx; int diff_y = ctx.target_dest_y - my;
    if (abs(diff_x) <= 1 && abs(diff_y) <= 1) { ctx.is_auto_moving = false; return; }

    int mdx = 0, mdy = 0;
    if (abs(diff_x) > abs(diff_y)) { mdx = (diff_x > 0) ? 1 : -1; if (!is_walkable(mx+mdx, my)) { mdx=0; mdy=(diff_y>0)?1:-1; } }
    else { mdy = (diff_y > 0) ? 1 : -1; if (!is_walkable(mx, my+mdy)) { mdy=0; mdx=(diff_x>0)?1:-1; } }
    
    if (mdx != 0 || mdy != 0) {
        GamePacket mv = {TYPE_MOVE, 0, mdx, mdy}; write(ctx.sock, &mv, sizeof(mv));
    }
}

void handle_inputs() {
    int ch = getch();
    if (ch == ERR) return;
    if (ch == 27) { } // ESC

    if (ctx.state == STATE_LOGIN) {
        if (ch == '\t') ctx.input_focus = (ctx.input_focus + 1) % 4;
        else if (ch == KEY_BACKSPACE || ch == 127) {
            char* buf = (ctx.input_focus == 0) ? ctx.input_user : (ctx.input_focus == 1 ? ctx.input_pass : nullptr);
            if (buf && strlen(buf) > 0) buf[strlen(buf)-1] = '\0';
        } else if (ch == '\n') {
            if (ctx.input_focus >= 2) {
                LoginPacket pkt; memset(&pkt, 0, sizeof(pkt));
                pkt.type = (ctx.input_focus == 2) ? TYPE_LOGIN_REQ : TYPE_REG_REQ;
                strncpy(pkt.username, ctx.input_user, 31); strncpy(pkt.password, ctx.input_pass, 31);
                write(ctx.sock, &pkt, sizeof(pkt));
            }
        } else if (ch >= 32 && ch <= 126) {
            char* buf = (ctx.input_focus == 0) ? ctx.input_user : (ctx.input_focus == 1 ? ctx.input_pass : nullptr);
            if (buf && strlen(buf) < 15) { buf[strlen(buf)] = (char)ch; buf[strlen(buf)+1] = '\0'; }
        }
    } 
    else if (ctx.state == STATE_LOBBY && !ctx.is_matching) {
        if (ch == 'c' || ch == 'C') { int t = TYPE_CREATE_ROOM; write(ctx.sock, &t, sizeof(t)); }
        else if (ch == 'j' || ch == 'J') { 
            echo(); curs_set(1); mvprintw(LINES-2, 2, "Enter Room ID: "); char buf[16]; getnstr(buf, 10); noecho(); curs_set(0);
            RoomControlPacket pkt = { TYPE_JOIN_ROOM, atoi(buf), 0, 0 }; write(ctx.sock, &pkt, sizeof(pkt));
        }
        else if (ch == 'm' || ch == 'M') { int t = TYPE_MATCH_REQ; write(ctx.sock, &t, sizeof(t)); ctx.is_matching = true; }
        else if (ch == 'r' || ch == 'R') { int t = TYPE_ROOM_LIST_REQ; write(ctx.sock, &t, sizeof(t)); }
    } 
    else if (ctx.state == STATE_ROOM) {
        bool owner = (ctx.my_slot_idx >= 0 && ctx.current_room.slots[ctx.my_slot_idx].is_owner);
        if (ch == 'q' || ch == 'Q') { int t = TYPE_LEAVE_ROOM; write(ctx.sock, &t, sizeof(t)); ctx.state = STATE_LOBBY; int r = TYPE_ROOM_LIST_REQ; write(ctx.sock, &r, sizeof(r)); }
        else if (ch == 'r' || ch == 'R') { if(!owner) { RoomControlPacket pkt = { TYPE_ROOM_UPDATE, ctx.current_room.room_id, -1, !ctx.current_room.slots[ctx.my_slot_idx].is_ready }; write(ctx.sock, &pkt, sizeof(pkt)); } }
        else if (ch == '\n' && owner) { 
            int t = TYPE_GAME_START; write(ctx.sock, &t, sizeof(t)); 
        }
        else if (ch == KEY_MOUSE) {
            MEVENT e; if (getmouse(&e) == OK && (e.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_DOUBLE_CLICKED))) {
                for (int i=0; i<10; i++) {
                    int x = (COLS/2) - 30 + (i%5) * 14; int y = (i<5) ? 6 : 13;
                    if (e.y >= y && e.y <= y+3 && e.x >= x && e.x <= x+12) { RoomControlPacket pkt = { TYPE_ROOM_UPDATE, ctx.current_room.room_id, i, 0 }; write(ctx.sock, &pkt, sizeof(pkt)); }
                }
            }
        }
    } 
    else if (ctx.state == STATE_PICK) {
        if (ctx.my_slot_idx >= 0 && ctx.current_room.slots[ctx.my_slot_idx].hero_id != 0) return;

        if (ch == KEY_UP || ch == 'w') {
            ctx.pick_cursor--; if(ctx.pick_cursor < 0) ctx.pick_cursor = 2;
        } 
        else if (ch == KEY_DOWN || ch == 's') {
            ctx.pick_cursor++; if(ctx.pick_cursor > 2) ctx.pick_cursor = 0;
        }
        else if (ch == '\n') {
            int hid = ctx.pick_cursor + 1; // 1,2,3
            GamePacket pkt; memset(&pkt, 0, sizeof(pkt));
            pkt.type = TYPE_SELECT; 
            pkt.input = hid;
            write(ctx.sock, &pkt, sizeof(pkt));
        }
    }
    else if (ctx.state == STATE_GAME) {
        // [新增] 商店开关
        if (ch == 'b' || ch == 'B') {
            ctx.show_shop = !ctx.show_shop;
        }
        
        // [新增] 商店购买逻辑
        if (ctx.show_shop) {
            int buy_id = 0;
            if (ch == '1') buy_id = 1;
            else if (ch == '2') buy_id = 2;
            else if (ch == '3') buy_id = 3;
            else if (ch == '4') buy_id = 4;
            else if (ch == '5') buy_id = 5;
            
            if (buy_id > 0) {
                GamePacket pkt; memset(&pkt, 0, sizeof(pkt));
                pkt.type = TYPE_BUY_ITEM;
                pkt.input = buy_id;
                write(ctx.sock, &pkt, sizeof(pkt));
            }
            // 如果在商店界面，可以选择是否屏蔽移动，这里选择不屏蔽，允许一边跑一边买
        }

        if (ch == 'w' || ch == 's' || ch == 'a' || ch == 'd') {
            ctx.is_auto_moving = false;
            int dx=0, dy=0; if(ch=='w') dy=-1; if(ch=='s') dy=1; if(ch=='a') dx=-1; if(ch=='d') dx=1;
            GamePacket mv = {TYPE_MOVE, 0, dx, dy}; write(ctx.sock, &mv, sizeof(mv));
        } else if (ch == 'j' || ch == 'J') { GamePacket att = {TYPE_ATTACK}; write(ctx.sock, &att, sizeof(att)); }
        else if (ch == 'k' || ch == 'K') { GamePacket spl = {TYPE_SPELL}; write(ctx.sock, &spl, sizeof(spl)); }
        else if (ch == KEY_MOUSE) {
            MEVENT e; if (getmouse(&e) == OK && (e.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_DOUBLE_CLICKED))) {
                int sx = e.x / 2; int sy = e.y - UI_TOP_H;
                if (sx>=0 && sx<ctx.view_w && sy>=0 && sy<ctx.view_h) {
                    ctx.target_dest_x = sx + ctx.cam_x; ctx.target_dest_y = sy + ctx.cam_y;
                    ctx.is_auto_moving = true; ctx.stuck_frames = 0; add_log("[CMD] Moving...");
                }
            }
        }
    }
}

// ==========================================
// 7. 主函数 (Main)
// ==========================================

int main() {
    setlocale(LC_ALL, ""); 
    MapGenerator::init(ctx.game_map); 

    ctx.sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in s_addr = {AF_INET, htons(8888)};
    inet_pton(AF_INET, "127.0.0.1", &s_addr.sin_addr);
    int flag = 1; setsockopt(ctx.sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

    if (connect(ctx.sock, (struct sockaddr*)&s_addr, sizeof(s_addr)) < 0) {
        std::cerr << "Cannot connect to server!" << std::endl; return -1;
    }
    
    ctx.state = STATE_LOGIN;

    initscr(); cbreak(); noecho(); curs_set(0); keypad(stdscr, TRUE);
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    mouseinterval(0); printf("\033[?1003h\n"); 

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
        init_pair(40, COLOR_MAGENTA, COLOR_BLACK); init_pair(41, COLOR_MAGENTA, COLOR_WHITE); 
    }

    timeout(0); 

    while (true) {
        process_network();
        handle_inputs();
        if (ctx.state == STATE_GAME) update_auto_move();

        if (ctx.state == STATE_LOGIN) draw_login();
        else if (ctx.state == STATE_LOBBY) draw_lobby();
        else if (ctx.state == STATE_ROOM) draw_room();
        else if (ctx.state == STATE_PICK) draw_pick_screen(); 
        else if (ctx.state == STATE_GAME) { draw_game_scene(); draw_game_ui(); }

        refresh();
        usleep(10000); 
    }
    printf("\033[?1003l\n"); endwin(); close(ctx.sock); return 0;
}