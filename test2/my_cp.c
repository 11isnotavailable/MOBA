#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>  // 包含 open 的定义
#include <sys/stat.h> // 包含文件权限宏

#define BUFFER_SIZE 1024 // 定义缓冲区大小

int main(int argc, char *argv[]) {
    // 1. 参数检查：必须是 ./my_cp 源文件 目标文件
    // 对应指导书要求：程序要有一定的健壮性 
    if (argc != 3) {
        printf("Usage: %s <source_file> <destination_file>\n", argv[0]);
        exit(1);
    }

    char *src_path = argv[1];
    char *dest_path = argv[2];

    // 2. 打开源文件 (只读方式)
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd == -1) {
        perror("Open source file failed"); // perror会自动输出具体错误原因（如文件不存在）
        exit(1);
    }

    // 3. 打开/创建目标文件
    // O_WRONLY: 只写
    // O_CREAT: 如果文件不存在则创建
    // O_TRUNC: 如果文件存在，则清空内容（覆盖）
    // 0644: 文件权限 (rw-r--r--)
    int dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dest_fd == -1) {
        perror("Open/Create destination file failed");
        close(src_fd); // 失败时记得关闭已打开的文件
        exit(1);
    }

    // 4. 循环读写 (核心逻辑)
    char buffer[BUFFER_SIZE];
    int bytes_read;
    
    // read 返回读到的字节数，如果返回 0 表示读到了文件末尾 (EOF)，返回 -1 表示出错
    while ((bytes_read = read(src_fd, buffer, BUFFER_SIZE)) > 0) {
        // write 返回实际写入的字节数
        int bytes_written = write(dest_fd, buffer, bytes_read);
        
        // 健壮性检查：写入字节数不等于读取字节数，说明磁盘满了或出错
        if (bytes_written != bytes_read) {
            perror("Write error");
            close(src_fd);
            close(dest_fd);
            exit(1);
        }
    }

    // 检查 read 是否因为错误而退出
    if (bytes_read == -1) {
        perror("Read error");
    }

    // 5. 关闭文件描述符
    close(src_fd);
    close(dest_fd);

    printf("Copy successful: %s -> %s\n", src_path, dest_path);
    return 0;
}