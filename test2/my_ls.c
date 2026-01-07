#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <unistd.h>

// 辅助函数：将模式 mode_t 转换为字符串 (如 -rwxr-xr-x)
void mode_to_str(mode_t mode, char *str) {
    strcpy(str, "----------");
    
    // 1. 判断文件类型
    if (S_ISDIR(mode)) str[0] = 'd';
    else if (S_ISCHR(mode)) str[0] = 'c';
    else if (S_ISBLK(mode)) str[0] = 'b';
    
    // 2. 拥有者权限 (User)
    if (mode & S_IRUSR) str[1] = 'r';
    if (mode & S_IWUSR) str[2] = 'w';
    if (mode & S_IXUSR) str[3] = 'x';
    
    // 3. 组权限 (Group)
    if (mode & S_IRGRP) str[4] = 'r';
    if (mode & S_IWGRP) str[5] = 'w';
    if (mode & S_IXGRP) str[6] = 'x';
    
    // 4. 其他人权限 (Other)
    if (mode & S_IROTH) str[7] = 'r';
    if (mode & S_IWOTH) str[8] = 'w';
    if (mode & S_IXOTH) str[9] = 'x';
}

// 打印单个文件的详细信息
void print_file_info(const char *dir_path, const char *filename) {
    struct stat info;
    char full_path[1024];
    
    // 拼接完整路径：目录/文件名
    sprintf(full_path, "%s/%s", dir_path, filename);
    
    // 获取文件属性
    if (stat(full_path, &info) == -1) {
        perror("stat");
        return;
    }
    
    // 1. 权限字符串
    char mode_str[11];
    mode_to_str(info.st_mode, mode_str);
    
    // 2. 链接数
    int links = info.st_nlink;
    
    // 3. 用户名
    struct passwd *pw = getpwuid(info.st_uid);
    char *user = pw ? pw->pw_name : "unknown";
    
    // 4. 组名
    struct group *gr = getgrgid(info.st_gid);
    char *group = gr ? gr->gr_name : "unknown";
    
    // 5. 大小
    long size = info.st_size;
    
    // 6. 时间 (去掉末尾的换行符)
    char *time_str = ctime(&info.st_mtime);
    time_str[strlen(time_str) - 1] = '\0';
    
    // 格式化输出: 权限 链接数 用户 组 大小 时间 文件名
    printf("%s %d %s %s %5ld %s %s\n", 
           mode_str, links, user, group, size, time_str, filename);
}

int main(int argc, char *argv[]) {
    char *dir_name;
    
    // 处理参数：如果没有参数，默认当前目录 "."
    if (argc == 1) {
        dir_name = ".";
    } else {
        dir_name = argv[1];
    }
    
    // 打开目录
    DIR *dir_ptr = opendir(dir_name);
    if (dir_ptr == NULL) {
        perror("opendir");
        exit(1);
    }
    
    struct dirent *direntp;
    // 循环读取目录项
    while ((direntp = readdir(dir_ptr)) != NULL) {
        // 为了输出整洁，通常跳过隐藏文件 (以.开头的文件)
        // 如果你想完全模拟 ls -a -l，可以去掉下面这两行
        if (direntp->d_name[0] == '.') 
             continue;
            
        print_file_info(dir_name, direntp->d_name);
    }
    
    closedir(dir_ptr);
    return 0;
}