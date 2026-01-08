#ifndef ROOM_MANAGER_H
#define ROOM_MANAGER_H

#include <map>
#include <vector>
#include <string>
#include "game_room.h"
#include "user_manager.h"
#include "protocol.h"

struct MatchPlayer {
    int fd;
    long long join_time;
};

class RoomManager {
private:
    UserManager* user_mgr; // 引用，用于获取名字
    
    // 房间容器: room_id -> Room*
    std::map<int, GameRoom*> rooms;
    int room_id_counter;

    // 匹配队列
    std::vector<MatchPlayer> match_queue;

    // 玩家当前所在的房间映射: fd -> room_id (0表示在大厅)
    std::map<int, int> player_room_map;

public:
    RoomManager(UserManager* um);
    ~RoomManager();

    // === 核心循环 ===
    // 每帧调用，驱动所有房间的逻辑 + 匹配逻辑
    void update_all();

    // === 外部事件处理 ===
    // 处理玩家断线
    void on_player_disconnect(int fd);
    
    // 处理大厅相关包 (创建、加入、匹配、列表)
    void handle_lobby_packet(int fd, int type, const void* data);
    
    // 处理房间内/游戏内包 (Move, Attack, RoomUpdate)
    // 如果玩家在房间里，转发给 GameRoom；否则忽略或报错
    void handle_game_packet(int fd, const GamePacket& pkt);
    
    // 转发房间控制包 (Ready, ChangeSlot, Kick)
    void handle_room_control(int fd, const RoomControlPacket& pkt);

private:
    // 内部逻辑
    void create_room(int fd);
    void join_room(int fd, int room_id);
    void leave_room(int fd);
    void send_room_list(int fd);
    
    // 匹配逻辑
    void add_to_match(int fd);
    void process_matching(); // 检查队列
};

#endif