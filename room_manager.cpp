#include "room_manager.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono> // [修复] 必须包含此头文件

static long long get_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

RoomManager::RoomManager(UserManager* um) : user_mgr(um) {
    room_id_counter = 1;
}

RoomManager::~RoomManager() {
    for (auto& pair : rooms) {
        delete pair.second;
    }
}

void RoomManager::update_all() {
    // 1. 驱动所有房间的游戏逻辑
    // 同时清理空房间
    auto it = rooms.begin();
    while (it != rooms.end()) {
        GameRoom* room = it->second;
        
        // 运行逻辑
        room->update_logic();
        
        // 检查是否为空 (没人了就销毁)
        if (room->is_empty()) {
            std::cout << "[RoomMgr] Room " << it->first << " is empty, removing." << std::endl;
            delete room;
            it = rooms.erase(it);
        } else {
            ++it;
        }
    }

    // 2. 处理随机匹配队列
    process_matching();
}

void RoomManager::on_player_disconnect(int fd) {
    // 1. 如果在匹配队列，移除
    for (auto it = match_queue.begin(); it != match_queue.end(); ) {
        if (it->fd == fd) it = match_queue.erase(it);
        else ++it;
    }

    // 2. 如果在房间里，移除
    if (player_room_map.count(fd)) {
        int rid = player_room_map[fd];
        if (rooms.count(rid)) {
            rooms[rid]->remove_player(fd);
        }
        player_room_map.erase(fd);
    }
}

// === 包分发逻辑 ===

void RoomManager::handle_lobby_packet(int fd, int type, const void* data) {
    if (type == TYPE_CREATE_ROOM) {
        create_room(fd);
    }
    else if (type == TYPE_JOIN_ROOM) {
        // 客户端发来的 RoomControlPacket 中包含 room_id
        const RoomControlPacket* pkt = (const RoomControlPacket*)data;
        join_room(fd, pkt->room_id);
    }
    else if (type == TYPE_LEAVE_ROOM) {
        leave_room(fd);
    }
    else if (type == TYPE_ROOM_LIST_REQ) {
        send_room_list(fd);
    }
    else if (type == TYPE_MATCH_REQ) {
        add_to_match(fd);
    }
    else if (type == TYPE_GAME_START) {
        // 房主请求开始游戏
        if (player_room_map.count(fd)) {
            int rid = player_room_map[fd];
            if (rooms.count(rid)) {
                bool ok = rooms[rid]->start_game(fd);
                if (ok) {
                    // [修改] 广播给房间所有人，并附带各自的 Game ID
                    GamePacket start_pkt; 
                    memset(&start_pkt, 0, sizeof(start_pkt));
                    start_pkt.type = TYPE_GAME_START;
                    
                    std::vector<int> mems = rooms[rid]->get_player_fds();
                    for(int mfd : mems) {
                        // 获取该玩家在游戏中的真实ID
                        start_pkt.id = rooms[rid]->get_player_id(mfd);
                        write(mfd, &start_pkt, sizeof(start_pkt));
                    }
                }
            }
        }
    }
}

void RoomManager::handle_room_control(int fd, const RoomControlPacket& pkt) {
    if (player_room_map.count(fd) == 0) return;
    int rid = player_room_map[fd];
    if (rooms.count(rid) == 0) return;
    
    GameRoom* room = rooms[rid];
    
    // TYPE_ROOM_UPDATE 复用于客户端发送 "Ready" / "ChangeSlot" 意图
    // 这里我们约定: 
    // pkt.slot_index != -1 -> 请求换座
    // pkt.extra_data -> 1=Ready, 0=Unready (如果 slot_index == -1)
    
    if (pkt.slot_index != -1) {
        room->change_slot(fd, pkt.slot_index);
    } else {
        room->set_ready(fd, pkt.extra_data == 1);
    }
    
    // 广播最新状态给房间内所有人
    RoomStatePacket state = room->get_room_state_packet();
    std::vector<int> mems = room->get_player_fds();
    for(int mfd : mems) write(mfd, &state, sizeof(state));
}

void RoomManager::handle_game_packet(int fd, const GamePacket& pkt) {
    // 必须在房间里
    if (player_room_map.count(fd) == 0) return;
    int rid = player_room_map[fd];
    if (rooms.count(rid)) {
        rooms[rid]->handle_game_packet(fd, pkt);
    }
}

// === 内部实现 ===

void RoomManager::create_room(int fd) {
    // 如果已经在房间，先退
    if (player_room_map.count(fd)) return; 

    std::string name = user_mgr->get_username(fd);
    int new_id = room_id_counter++;
    
    GameRoom* room = new GameRoom(new_id, name);
    rooms[new_id] = room;
    
    room->add_player(fd, name);
    player_room_map[fd] = new_id;
    
    std::cout << "[RoomMgr] Player " << name << " created Room " << new_id << std::endl;
    
    // 发送房间状态
    RoomStatePacket state = room->get_room_state_packet();
    write(fd, &state, sizeof(state));
}

void RoomManager::join_room(int fd, int room_id) {
    if (player_room_map.count(fd)) return; // 已经在房间
    if (rooms.count(room_id) == 0) {
        // 房间不存在
        return; 
    }
    
    GameRoom* room = rooms[room_id];
    std::string name = user_mgr->get_username(fd);
    
    if (room->add_player(fd, name)) {
        player_room_map[fd] = room_id;
        std::cout << "[RoomMgr] Player " << name << " joined Room " << room_id << std::endl;
        
        // 广播新状态给房间所有人
        RoomStatePacket state = room->get_room_state_packet();
        std::vector<int> mems = room->get_player_fds();
        for(int mfd : mems) write(mfd, &state, sizeof(state));
    } else {
        // 房间满或游戏中，发送错误包(这里简化，不发了)
    }
}

void RoomManager::leave_room(int fd) {
    if (player_room_map.count(fd) == 0) return;
    int rid = player_room_map[fd];
    
    if (rooms.count(rid)) {
        GameRoom* room = rooms[rid];
        room->remove_player(fd);
        
        // 如果房间还有人，通知剩下的人
        if (!room->is_empty()) {
            RoomStatePacket state = room->get_room_state_packet();
            std::vector<int> mems = room->get_player_fds();
            for(int mfd : mems) write(mfd, &state, sizeof(state));
        }
    }
    player_room_map.erase(fd);
    
    // 回复一个空的列表包作为“回到大厅”的确认
    send_room_list(fd);
}

void RoomManager::send_room_list(int fd) {
    RoomListPacket pkt;
    pkt.type = TYPE_ROOM_LIST_RESP;
    pkt.count = 0;
    
    // 取前5个房间 (简单实现)
    int i = 0;
    for (auto& pair : rooms) {
        if (i >= 5) break;
        pkt.rooms[i] = pair.second->get_room_info();
        pkt.count++;
        i++;
    }
    write(fd, &pkt, sizeof(pkt));
}

// === 匹配逻辑 ===

void RoomManager::add_to_match(int fd) {
    // 已经在匹配队列?
    for(auto& mp : match_queue) if(mp.fd == fd) return;
    // 已经在房间?
    if (player_room_map.count(fd)) return;

    MatchPlayer mp;
    mp.fd = fd;
    mp.join_time = get_ms();
    match_queue.push_back(mp);
    
    // 发个回执? 这里省略，客户端显示"Matching..."即可
    std::cout << "[Match] Player " << fd << " added to queue. Size: " << match_queue.size() << std::endl;
}

void RoomManager::process_matching() {
    if (match_queue.empty()) return;

    // 1. 检查是否满10人
    if (match_queue.size() >= 10) {
        // 创建新房间
        int new_id = room_id_counter++;
        // 取第一个人为"名义房主"
        int owner_fd = match_queue[0].fd;
        std::string owner_name = user_mgr->get_username(owner_fd);
        
        GameRoom* room = new GameRoom(new_id, owner_name);
        rooms[new_id] = room;
        
        // 移动前10人进房间
        std::cout << "[Match] Full 10 players! Auto creating Room " << new_id << std::endl;
        for (int i = 0; i < 10; i++) {
            int fd = match_queue[i].fd;
            std::string name = user_mgr->get_username(fd);
            room->add_player(fd, name);
            player_room_map[fd] = new_id;
            room->set_ready(fd, true); // 自动准备
        }
        
        // 移除队列
        match_queue.erase(match_queue.begin(), match_queue.begin() + 10);
        
        // 自动开始游戏
        room->start_game(owner_fd); 
        
        // [修改] 通知所有人，附带真实 Entity ID
        GamePacket start_pkt; 
        memset(&start_pkt, 0, sizeof(start_pkt));
        start_pkt.type = TYPE_GAME_START;
        
        std::vector<int> mems = room->get_player_fds();
        for(int mfd : mems) {
            start_pkt.id = room->get_player_id(mfd);
            write(mfd, &start_pkt, sizeof(start_pkt));
        }
        return;
    }

    // 2. 检查超时 (假设第1个人等了超过10秒)
    // 你的需求: "全服人数不足10人时有多少人就算多少人，划到一个房间"
    long long now = get_ms();
    if (now - match_queue[0].join_time > 10000) { // 10秒超时
        std::cout << "[Match] Timeout. Grouping " << match_queue.size() << " players." << std::endl;
        
        int new_id = room_id_counter++;
        int owner_fd = match_queue[0].fd;
        std::string owner_name = user_mgr->get_username(owner_fd);
        
        GameRoom* room = new GameRoom(new_id, owner_name);
        rooms[new_id] = room;
        
        // 全部移动进房间
        for (auto& mp : match_queue) {
            int fd = mp.fd;
            std::string name = user_mgr->get_username(fd);
            room->add_player(fd, name);
            player_room_map[fd] = new_id;
            // 不自动开始，让房主决定
        }
        match_queue.clear();
        
        // 广播房间状态，让大家进入"房间界面"
        RoomStatePacket state = room->get_room_state_packet();
        std::vector<int> mems = room->get_player_fds();
        for(int mfd : mems) write(mfd, &state, sizeof(state));
    }
}