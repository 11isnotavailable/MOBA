#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#define MAX_PATH 1024

// 检查是否为目录
int is_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == -1) return 0;
    return S_ISDIR(st.st_mode);
}

void list_dir_recursive(const char *base_path) {
    DIR *dir;
    struct dirent *entry;
    char path[MAX_PATH];

    // 1. 打印当前目录头，例如 ".:"
    printf("%s:\n", base_path);

    if (!(dir = opendir(base_path))) {
        perror("opendir");
        return;
    }

    // 用于暂存子目录路径的数组（简单起见，定长数组）
    char *subdirs[256];
    int subdir_count = 0;

    // 2. 第一遍遍历：打印所有文件，并记录下子目录
    while ((entry = readdir(dir)) != NULL) {
        // 跳过 . 和 ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // 打印文件名
        printf("%s  ", entry->d_name);

        // 拼接完整路径
        snprintf(path, sizeof(path), "%s/%s", base_path, entry->d_name);

        // 如果是目录，加入待处理列表
        if (is_dir(path) && subdir_count < 256) {
            subdirs[subdir_count] = strdup(path); // 复制路径字符串
            subdir_count++;
        }
    }
    printf("\n\n"); // 当前目录打印完毕，换行
    closedir(dir);

    // 3. 第二遍：递归处理所有子目录
    for (int i = 0; i < subdir_count; i++) {
        list_dir_recursive(subdirs[i]);
        free(subdirs[i]); // 释放 strdup 分配的内存
    }
}

int main(int argc, char *argv[]) {
    const char *start_path = ".";
    if (argc > 1) {
        start_path = argv[1];
    }
    list_dir_recursive(start_path);
    return 0;
}