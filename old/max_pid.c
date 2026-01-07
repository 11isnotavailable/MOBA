#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    printf("=== System Process Limit Detection ===\n");

    // 1. 读取内核允许的最大 PID 值
    // 这个值决定了系统范围内所有进程 ID 的上限
    FILE *fp = fopen("/proc/sys/kernel/pid_max", "r");
    if (fp != NULL) {
        char buffer[128];
        if (fgets(buffer, sizeof(buffer), fp) != NULL) {
            printf("[Kernel] Max PID value (pid_max): %s", buffer);
        }
        fclose(fp);
    } else {
        perror("Failed to read pid_max");
    }

    // 2. 读取当前用户允许创建的最大进程数
    // 这是更严格的限制，防止单个用户耗尽系统资源
    printf("[User] Max user processes (ulimit -u): ");
    fflush(stdout); // 确保前面的打印先输出
    system("ulimit -u");

    // 3. 读取整个系统允许的最大线程/进程总数
    FILE *fp_threads = fopen("/proc/sys/kernel/threads-max", "r");
    if (fp_threads != NULL) {
        char buffer[128];
        if (fgets(buffer, sizeof(buffer), fp_threads) != NULL) {
            printf("[Kernel] Max system threads (threads-max): %s", buffer);
        }
        fclose(fp_threads);
    }

    return 0;
}