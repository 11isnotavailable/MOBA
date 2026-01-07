#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#define MAX_PATH 4096

// 获取文件的inode编号
ino_t get_inode(char *fname) {
    struct stat info;
    if (stat(fname, &info) == -1) {
        perror("stat");
        exit(1);
    }
    return info.st_ino;
}

// 递归函数：反向寻找路径
void print_path_to_root(ino_t this_inode) {
    ino_t my_inode;
    char its_name[MAX_PATH];

    // 1. 获取当前目录(其实是上一层递归传下来的父目录)的 .. 的inode
    // 如果 . 和 .. 的inode一样，说明到达了根目录 /
    if (get_inode(".") == get_inode(".."))
        return;

    my_inode = get_inode("."); // 保存当前目录(子)的inode
    
    chdir(".."); // 2. 真正切换到父目录

    // 3. 在父目录中搜索：哪个文件的inode等于 my_inode？
    DIR *dir_ptr = opendir(".");
    struct dirent *direntp;
    while ((direntp = readdir(dir_ptr)) != NULL) {
        if (direntp->d_ino == my_inode) {
            // 找到了！记录名字
            strcpy(its_name, direntp->d_name);
            // 4. 递归继续往上找
            print_path_to_root(my_inode);
            // 5. 递归回来时打印名字 (实现倒序打印)
            printf("/%s", its_name);
            break;
        }
    }
    closedir(dir_ptr);
}

int main() {
    print_path_to_root(get_inode("."));
    printf("\n");
    return 0;
}