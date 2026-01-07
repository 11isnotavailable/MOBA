#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdlib.h>

int main() {
    int p1, p2, i;

    // 创建第一个子进程
    while((p1 = fork()) == -1); 

    if (p1 == 0) {
        // --- 子进程 1 (打印 b) ---
        for (i = 0; i < 100; i++) {
            printf("daughter %d (b)\n", i);
            // usleep(1000); // 稍微停顿一下，让并发效果更明显（可选）
        }
        exit(0);
    } 
    else {
        // --- 父进程 ---
        // 创建第二个子进程
        while((p2 = fork()) == -1);

        if (p2 == 0) {
            // --- 子进程 2 (打印 c) ---
            for (i = 0; i < 100; i++) {
                printf("son %d (c)\n", i);
            }
            exit(0);
        } 
        else {
            // --- 父进程 (打印 a) ---
            for (i = 0; i < 100; i++) {
                printf("parent %d (a)\n", i);
            }
        }
    }
    return 0;
}