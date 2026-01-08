#include "user_manager.h"
#include "protocol.h"
#include <sstream>

UserManager::UserManager(const std::string& filename) : db_file(filename) {
    load_db();
}

UserManager::~UserManager() {
    save_db();
}

void UserManager::load_db() {
    std::ifstream in(db_file);
    if (!in.is_open()) {
        std::cout << "[UserManager] No user db found, creating new one." << std::endl;
        return;
    }
    
    std::string line, u, p;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        ss >> u >> p;
        if (!u.empty() && !p.empty()) {
            UserData d; d.username = u; d.password = p;
            all_users[u] = d;
        }
    }
    std::cout << "[UserManager] Loaded " << all_users.size() << " users." << std::endl;
    in.close();
}

void UserManager::save_db() {
    std::ofstream out(db_file);
    for (auto& pair : all_users) {
        out << pair.second.username << " " << pair.second.password << "\n";
    }
    out.close();
}

int UserManager::register_user(const std::string& username, const std::string& password) {
    if (all_users.count(username)) {
        return RET_FAIL_DUP; // 用户名已存在
    }
    
    UserData d; 
    d.username = username; 
    d.password = password;
    all_users[username] = d;
    
    save_db(); // 立即持久化
    std::cout << "[UserManager] New user registered: " << username << std::endl;
    return RET_SUCCESS;
}

int UserManager::login_user(int fd, const std::string& username, const std::string& password) {
    if (all_users.count(username) == 0) {
        return RET_FAIL_NONAME;
    }
    
    if (all_users[username].password != password) {
        return RET_FAIL_PWD;
    }

    if (is_online(username)) {
        // 简单处理：不允许重复登录，或者顶号（这里选择不允许）
        return RET_FAIL_DUP;
    }

    // 登录成功，记录在线状态
    online_users[fd] = username;
    std::cout << "[UserManager] User login: " << username << " (fd: " << fd << ")" << std::endl;
    return RET_SUCCESS;
}

void UserManager::logout_user(int fd) {
    if (online_users.count(fd)) {
        std::cout << "[UserManager] User logout: " << online_users[fd] << std::endl;
        online_users.erase(fd);
    }
}

bool UserManager::is_online(const std::string& username) {
    for (auto& pair : online_users) {
        if (pair.second == username) return true;
    }
    return false;
}

std::string UserManager::get_username(int fd) {
    if (online_users.count(fd)) return online_users[fd];
    return "";
}

int UserManager::get_online_count() {
    return online_users.size();
}