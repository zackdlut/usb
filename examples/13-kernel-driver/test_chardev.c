/*
 * 用户态测试：打开 /dev/miniusb_vendor，对应第 10 章 vendor_client 的几项功能。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "vendor_protocol.h"

#define IOCTL_GET_INFO 0xCAFE01
#define IOCTL_SET_LED  0xCAFE02

static void die(const char *what)
{
	perror(what);
}

static void print_info(const struct vendor_info *info)
{
	printf("uptime_ms=%u loopback_bytes=%u button=%u led=%u board=%u\n",
	       info->uptime_ms, info->loopback_bytes, info->button, info->led,
	       info->board);
}

int main(int argc, char **argv)
{
	int fd;
	struct vendor_info info;
	char buf[256];

	if (argc < 2) {
		fprintf(stderr,
			"用法: %s info | led on|off | echo <文本>\n"
			"需要驱动已加载且设备 cafe:4011 已连接。\n",
			argv[0]);
		return 1;
	}

	fd = open("/dev/miniusb_vendor", O_RDWR);
	if (fd < 0) {
		die("open /dev/miniusb_vendor");
		fprintf(stderr, "提示：sudo insmod miniusb_vendor.ko 了吗？\n");
		return 1;
	}

	if (strcmp(argv[1], "info") == 0) {
		if (ioctl(fd, IOCTL_GET_INFO, &info) < 0) {
			die("ioctl GET_INFO");
			close(fd);
			return 1;
		}
		print_info(&info);
	} else if (strcmp(argv[1], "led") == 0 && argc >= 3) {
		long on = (strcmp(argv[2], "on") == 0);
		if (ioctl(fd, IOCTL_SET_LED, on) < 0) {
			die("ioctl SET_LED");
			close(fd);
			return 1;
		}
		printf("LED %s\n", on ? "on" : "off");
	} else if (strcmp(argv[1], "echo") == 0 && argc >= 3) {
		size_t len = strlen(argv[2]);
		if (len > sizeof(buf))
			len = sizeof(buf);
		memcpy(buf, argv[2], len);
		ssize_t n = write(fd, buf, len);
		if (n < 0) {
			die("write");
			close(fd);
			return 1;
		}
		buf[n] = '\0';
		printf("回环: %s\n", buf);
	} else {
		fprintf(stderr, "未知子命令\n");
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
