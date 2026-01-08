#ifndef USER_MANAGER_H
#define USER_MANAGER_H

#include <string>
#include <map>
#include <fstream>
#include <iostream>
#include <vector>

// 简单的用户结构
struct UserData {
    std::string username;
    std::string password;
    // 可以在这里扩展战绩：int wins; int loses;
};

class UserManager {
private:
    std::string db_file;
    // 数据库: username -> UserData
    std::map<std::string, UserData> all_users;
    
    // 在线列表: fd -> username
    std::map<int, std::string> online_users;

    void load_db();
    void save_db();

public:
    UserManager(const std::string& filename = "users.txt");
    ~UserManager();

    // 注册: 返回 RET_SUCCESS 或 RET_FAIL_DUP
    int register_user(const std::string& username, const std::string& password);

    // 登录: 返回 RET_SUCCESS, RET_FAIL_PWD, RET_FAIL_NONAME
    int login_user(int fd, const std::string& username, const std::string& password);

    // 处理断线
    void logout_user(int fd);

    // 检查是否在线
    bool is_online(const std::string& username);

    // 通过FD获取用户名
    std::string get_username(int fd);
    
    // 获取当前在线人数
    int get_online_count();
};

#endif