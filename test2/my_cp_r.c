#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>

#define BUFFER_SIZE 1024

// --- 功能1: 复制单个文件 (复用 Task 1 的逻辑) ---
void copy_file(const char *src_path, const char *dest_path) {
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd == -1) {
        perror("Open source file failed");
        return;
    }

    // 创建目标文件，权限设为 0644 (rw-r--r--)
    int dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dest_fd == -1) {
        perror("Create dest file failed");
        close(src_fd);
        return;
    }

    char buffer[BUFFER_SIZE];
    int bytes_read;
    while ((bytes_read = read(src_fd, buffer, BUFFER_SIZE)) > 0) {
        write(dest_fd, buffer, bytes_read);
    }

    close(src_fd);
    close(dest_fd);
    // printf("Copied file: %s -> %s\n", src_path, dest_path);
}

// --- 功能2: 递归复制 (核心逻辑) ---
void copy_recursive(const char *src_path, const char *dest_path) {
    struct stat src_stat;
    
    // 1. 获取源路径的属性
    if (stat(src_path, &src_stat) == -1) {
        perror("stat failed");
        return;
    }

    // 2. 判断是文件还是目录
    if (S_ISREG(src_stat.st_mode)) {
        // --- 如果是普通文件，直接复制 ---
        copy_file(src_path, dest_path);
    } 
    else if (S_ISDIR(src_stat.st_mode)) {
        // --- 如果是目录，进行递归 ---
        
        // A. 创建目标目录 (权限 0755: rwxr-xr-x)
        mkdir(dest_path, 0755);
        // printf("Created dir: %s\n", dest_path);

        // B. 打开源目录
        DIR *dir_ptr = opendir(src_path);
        if (dir_ptr == NULL) {
            perror("opendir failed");
            return;
        }

        struct dirent *entry;
        char new_src[1024];
        char new_dest[1024];

        // C. 遍历目录项
        while ((entry = readdir(dir_ptr)) != NULL) {
            // 重要：跳过 "." 和 ".."，否则会无限递归死循环
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            // D. 拼接新的路径
            // sprintf 是格式化字符串，把 src/filename 拼接到 new_src 里
            sprintf(new_src, "%s/%s", src_path, entry->d_name);
            sprintf(new_dest, "%s/%s", dest_path, entry->d_name);

            // E. 递归调用自己！
            copy_recursive(new_src, new_dest);
        }
        closedir(dir_ptr);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <source_dir> <dest_dir>\n", argv[0]);
        return 1;
    }

    printf("Starting recursive copy from [%s] to [%s]...\n", argv[1], argv[2]);
    copy_recursive(argv[1], argv[2]);
    printf("Recursive copy finished.\n");

    return 0;
}