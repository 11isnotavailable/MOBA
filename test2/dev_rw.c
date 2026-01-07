#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

int main(int argc, char *argv[]) {
    // 1. 检查是否传入了设备路径
    if (argc != 2) {
        printf("Usage: %s <tty_device_path>\n", argv[0]);
        printf("Example: %s /dev/pts/0\n", argv[0]);
        return 1;
    }

    char *dev_path = argv[1];

    // 2. 打开设备文件 (读写模式)
    // 这一步体现了“一切皆文件”，我们像打开普通txt一样打开了硬件设备
    int fd = open(dev_path, O_RDWR);
    if (fd == -1) {
        perror("Open device failed");
        return 1;
    }

    printf("Device %s opened successfully (fd=%d).\n", dev_path, fd);
    printf("Trying to write to this device...\n");

    // 3. 写操作：向设备写入数据
    // 效果：字符串会直接出现在该终端的屏幕上
    char *msg = "\n[Message from code] Hello! This is written directly to the device file!\n";
    write(fd, msg, strlen(msg));

    // 4. (选做) 读操作：从设备读取数据
    // 效果：程序会阻塞在这里，等待你在那个终端敲键盘
    printf("Now reading from device (Please type something and press Enter):\n");
    char buffer[100];
    int bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        printf("Read from device: %s", buffer);
    }

    // 5. 关闭文件
    close(fd);
    return 0;
}