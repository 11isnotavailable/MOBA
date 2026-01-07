#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <string.h>

int main() {
    int p1, p2;
    int fd[2]; // fd[0]是读端，fd[1]是写端
    char outpipe[100];
    char inpipe[100];

    // 1. 创建管道
    if (pipe(fd) == -1) {
        perror("Pipe creation failed");
        exit(1);
    }

    while((p1 = fork()) == -1);

    if (p1 == 0) {
        // --- 子进程 1 ---
        // 锁定管道写端，防止争抢
        lockf(fd[1], 1, 0); 
        
        sprintf(outpipe, "Child 1 is sending message!");
        write(fd[1], outpipe, 50); // 写入50字节
        sleep(1); // 故意延迟，测试锁的效果
        
        lockf(fd[1], 0, 0); // 解锁
        exit(0);
    } else {
        while((p2 = fork()) == -1);

        if (p2 == 0) {
            // --- 子进程 2 ---
            // 互斥锁：等待子进程1解锁后才能锁定
            lockf(fd[1], 1, 0); 
            
            sprintf(outpipe, "Child 2 is sending message!");
            write(fd[1], outpipe, 50);
            
            lockf(fd[1], 0, 0);
            exit(0);
        } else {
            // --- 父进程 ---
            // 父进程只负责读，等待两个子进程写完
            wait(NULL); // 等待子进程1
            read(fd[0], inpipe, 50);
            printf("%s\n", inpipe);

            wait(NULL); // 等待子进程2
            read(fd[0], inpipe, 50);
            printf("%s\n", inpipe);
            
            exit(0);
        }
    }
    return 0;
}