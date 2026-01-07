#ifndef ROOM_H
#define ROOM_H

#include <ncurses.h>
#include <string>
#include "protocol.h"

class RoomManager {
public:
    // 渲染房间界面
    // slots: 房间数据
    // my_idx: 我在哪个位置 (0-9)
    static void draw_room(const RoomSlot slots[MAX_PLAYERS], int my_idx) {
        clear();
        int H = LINES, W = COLS;
        
        // 1. 绘制标题
        attron(COLOR_PAIR(1) | A_BOLD);
        std::string title = "WAITING ROOM (HOST: SLOT 1)";
        mvprintw(2, (W - title.length())/2, "%s", title.c_str());
        attroff(COLOR_PAIR(1) | A_BOLD);

        // 2. 绘制操作提示
        std::string tip = "[R] Ready/Cancel   [ENTER] Start (Host Only)   [Q] Quit";
        mvprintw(H-2, (W - tip.length())/2, "%s", tip.c_str());

        // 3. 绘制 10 个座位 (2排 x 5个)
        // 计算布局
        int box_w = 12;
        int box_h = 6;
        int gap_x = 4;
        int start_x = (W - (5 * box_w + 4 * gap_x)) / 2;
        int start_y = (H - (2 * box_h + 4)) / 2;

        for (int i = 0; i < MAX_PLAYERS; i++) {
            int row = i / 5; // 0 或 1
            int col = i % 5;
            
            int x = start_x + col * (box_w + gap_x);
            int y = start_y + row * (box_h + 4);

            draw_slot(y, x, box_w, box_h, slots[i], i, my_idx);
        }
        
        // 4. 画 VS 标志
        mvprintw(start_y + box_h + 2, W/2 - 1, "VS");

        refresh();
    }

private:
    static void draw_slot(int y, int x, int w, int h, const RoomSlot& slot, int slot_idx, int my_idx) {
        // 判定颜色
        // 颜色定义在 client.cpp: 
        // 40: 己方(绿), 41: 队友(蓝), 42: 敌方(红), 43: 空位(黑框白底)
        
        int color_pair = 43; // 默认空位
        bool is_me = (slot_idx == my_idx);
        bool same_team = (slot_idx / 5) == (my_idx / 5);

        if (slot.player_id != 0) {
            if (is_me) color_pair = 40; // 自己: 绿色
            else if (same_team) color_pair = 41; // 队友: 蓝色
            else color_pair = 42; // 敌人: 红色
        }

        attron(COLOR_PAIR(color_pair));
        
        // 绘制矩形
        if (slot.player_id == 0) {
            // 空位：画框
            box(stdscr, 0, 0); // 这是一个 trick，我们需要手动画框或者利用 ncurses 窗口
            // 手动画空心框
            mvhline(y, x, 0, w);
            mvhline(y+h-1, x, 0, w);
            mvvline(y, x, 0, h);
            mvvline(y, x+w-1, 0, h);
            
            // 角落修复
            mvaddch(y, x, ACS_ULCORNER);
            mvaddch(y, x+w-1, ACS_URCORNER);
            mvaddch(y+h-1, x, ACS_LLCORNER);
            mvaddch(y+h-1, x+w-1, ACS_LRCORNER);

            mvprintw(y + h/2, x + 2, "EMPTY");
        } else {
            // 有人：填充实心块
            for(int dy=0; dy<h; dy++) {
                for(int dx=0; dx<w; dx++) {
                    mvaddch(y+dy, x+dx, ' '); // 打印空格，依靠背景色填充
                }
            }
            // 文字
            attron(A_BOLD);
            if (is_me) mvprintw(y+1, x+2, "YOU");
            else mvprintw(y+1, x+2, "P-%d", slot.player_id);
            
            // 状态
            if (slot.is_host) mvprintw(y+h-2, x+2, "[HOST]");
            else if (slot.is_ready) mvprintw(y+h-2, x+2, "READY");
            else mvprintw(y+h-2, x+2, "...");
            attroff(A_BOLD);
        }

        attroff(COLOR_PAIR(color_pair));
    }
};

#endif