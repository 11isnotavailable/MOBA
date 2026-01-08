#include "room_manager.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono> 

// 获取当前毫秒时间戳
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
    // 1. 驱动所有房间的游戏逻辑，并清理空房间
    auto it = rooms.begin();
    while (it != rooms.end()) {
        GameRoom* room = it->second;
        
        // 运行房间内部逻辑
        room->update_logic();
        
        // 检查是否为空
        if (room->is_empty()) {
            std::cout << "[RoomMgr] Room " << it->first << " is empty, removing." << std::endl;
            delete room;
            it = rooms.erase(it);
        } else {
            ++it;
        }
    }

    // 2. 处理匹配队列
    process_matching();
}

void RoomManager::on_player_disconnect(int fd) {
    // 1. 从匹配队列移除
    for (auto it = match_queue.begin(); it != match_queue.end(); ) {
        if (it->fd == fd) it = match_queue.erase(it);
        else ++it;
    }

    // 2. 从房间移除并清理映射
    if (player_room_map.count(fd)) {
        int rid = player_room_map[fd];
        if (rooms.count(rid)) {
            rooms[rid]->remove_player(fd);
        }
        player_room_map.erase(fd);
    }
}

// === 网络包分发逻辑 ===

void RoomManager::handle_lobby_packet(int fd, int type, const void* data) {
    if (type == TYPE_CREATE_ROOM) {
        create_room(fd);
    }
    else if (type == TYPE_JOIN_ROOM) {
        // 修正：直接转换指针读取完整的 RoomControlPacket
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
        if (player_room_map.count(fd)) {
            int rid = player_room_map[fd];
            if (rooms.count(rid)) {
                // 进入选人阶段
                rooms[rid]->start_game(fd);
            }
        }
    }
}

void RoomManager::handle_room_control(int fd, const RoomControlPacket& pkt) {
    if (player_room_map.count(fd) == 0) return;
    int rid = player_room_map[fd];
    if (rooms.count(rid) == 0) return;
    
    GameRoom* room = rooms[rid];
    
    if (pkt.slot_index != -1) {
        room->change_slot(fd, pkt.slot_index);
    } else {
        room->set_ready(fd, pkt.extra_data == 1);
    }
    
    // 广播房间最新状态
    RoomStatePacket state = room->get_room_state_packet();
    std::vector<int> mems = room->get_player_fds();
    for(int mfd : mems) write(mfd, &state, sizeof(state));
}

void RoomManager::handle_game_packet(int fd, const GamePacket& pkt) {
    if (player_room_map.count(fd) == 0) return;
    int rid = player_room_map[fd];
    if (rooms.count(rid)) {
        rooms[rid]->handle_game_packet(fd, pkt);
    }
}

// === 内部逻辑实现 ===

void RoomManager::create_room(int fd) {
    // 修正：创建前先离开可能存在的旧房间
    if (player_room_map.count(fd)) {
        leave_room(fd);
    }

    std::string name = user_mgr->get_username(fd);
    int new_id = room_id_counter++;
    
    GameRoom* room = new GameRoom(new_id, name);
    rooms[new_id] = room;
    
    room->add_player(fd, name);
    player_room_map[fd] = new_id;
    
    std::cout << "[RoomMgr] Player " << name << " created Room " << new_id << std::endl;
    
    RoomStatePacket state = room->get_room_state_packet();
    write(fd, &state, sizeof(state));
}

void RoomManager::join_room(int fd, int room_id) {
    // 修正：加入前先清理旧房间状态，防止逻辑阻塞
    if (player_room_map.count(fd)) {
        std::cout << "[RoomMgr] Player already in room " << player_room_map[fd] << ", leaving first." << std::endl;
        leave_room(fd); 
    }

    if (rooms.count(room_id) == 0) {
        std::cout << "[RoomMgr] Join failed: Room " << room_id << " not found." << std::endl;
        return; 
    }
    
    GameRoom* room = rooms[room_id];
    std::string name = user_mgr->get_username(fd);
    
    if (room->add_player(fd, name)) {
        player_room_map[fd] = room_id;
        std::cout << "[RoomMgr] Player " << name << " joined Room " << room_id << std::endl;
        
        RoomStatePacket state = room->get_room_state_packet();
        std::vector<int> mems = room->get_player_fds();
        for(int mfd : mems) write(mfd, &state, sizeof(state));
    }
}

void RoomManager::leave_room(int fd) {
    if (player_room_map.count(fd) == 0) return;
    int rid = player_room_map[fd];
    
    if (rooms.count(rid)) {
        GameRoom* room = rooms[rid];
        room->remove_player(fd);
        
        if (!room->is_empty()) {
            RoomStatePacket state = room->get_room_state_packet();
            std::vector<int> mems = room->get_player_fds();
            for(int mfd : mems) write(mfd, &state, sizeof(state));
        }
    }
    player_room_map.erase(fd);
    send_room_list(fd);
}

void RoomManager::send_room_list(int fd) {
    RoomListPacket pkt;
    pkt.type = TYPE_ROOM_LIST_RESP;
    pkt.count = 0;
    
    int i = 0;
    for (auto& pair : rooms) {
        if (i >= 10) break;
        pkt.rooms[i] = pair.second->get_room_info();
        pkt.count++;
        i++;
    }
    write(fd, &pkt, sizeof(pkt));
}

// === 匹配逻辑 ===

void RoomManager::add_to_match(int fd) {
    for(auto& mp : match_queue) if(mp.fd == fd) return;
    if (player_room_map.count(fd)) return;

    MatchPlayer mp;
    mp.fd = fd;
    mp.join_time = get_ms();
    match_queue.push_back(mp);
    
    std::cout << "[Match] Player " << fd << " added to queue. Size: " << match_queue.size() << std::endl;
}

void RoomManager::process_matching() {
    if (match_queue.empty()) return;

    // 1. 检查是否满10人
    if (match_queue.size() >= 10) {
        int new_id = room_id_counter++;
        int owner_fd = match_queue[0].fd;
        std::string owner_name = user_mgr->get_username(owner_fd);
        
        GameRoom* room = new GameRoom(new_id, owner_name);
        rooms[new_id] = room;
        
        std::cout << "[Match] Full 10 players! Auto creating Room " << new_id << std::endl;
        for (int i = 0; i < 10; i++) {
            int fd = match_queue[i].fd;
            std::string name = user_mgr->get_username(fd);
            room->add_player(fd, name);
            player_room_map[fd] = new_id;
            room->set_ready(fd, true);
        }
        
        match_queue.erase(match_queue.begin(), match_queue.begin() + 10);
        
        // 自动进入选人阶段
        room->start_game(owner_fd); 
        return;
    }

    // 2. 检查超时 (10秒强制开)
    long long now = get_ms();
    if (now - match_queue[0].join_time > 10000) { 
        std::cout << "[Match] Timeout. Grouping " << match_queue.size() << " players." << std::endl;
        
        int new_id = room_id_counter++;
        int owner_fd = match_queue[0].fd;
        std::string owner_name = user_mgr->get_username(owner_fd);
        
        GameRoom* room = new GameRoom(new_id, owner_name);
        rooms[new_id] = room;
        
        for (auto& mp : match_queue) {
            int fd = mp.fd;
            std::string name = user_mgr->get_username(fd);
            room->add_player(fd, name);
            player_room_map[fd] = new_id;
        }
        match_queue.clear();
        
        // 广播进入房间
        RoomStatePacket state = room->get_room_state_packet();
        std::vector<int> mems = room->get_player_fds();
        for(int mfd : mems) write(mfd, &state, sizeof(state));
    }
}