/*
 * init_seq.c v3 — CS 边沿 vs 会话中途:死亡定位实验
 *
 * 每个案例前空闲 1.2s(已知空闲后首笔必活):
 *   a. 1 条消息 1 笔 2B 读          —— 基线,应 92
 *   b. 1 条消息 1 笔 4B 读          —— CS 连续不抖:若 rx=xx 92 92 92,
 *                                        芯片在长会话中无恙,问题在 CS 边沿
 *   c. 1 条消息 2 笔 2B 读(cs_change=0)—— CS 同样连续,应同 b
 *   d. 2 条消息各 2B 读背靠背       —— 多一次 CS 边沿,已知死
 * 编译:$TC -O2 -Wall -static init_seq.c -o init_seq
 */
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int fd;

static int xfer_msg(struct spi_ioc_transfer *tr, int n)
{
	return ioctl(fd, SPI_IOC_MESSAGE(n), tr) < 0 ? -1 : 0;
}

static int rd_msg(uint8_t reg, uint8_t *out, uint8_t len, int msgs)
{
	/* msgs=1:单笔 len 字节全双工;msgs=len:每字节一笔(等价连发) */
	struct spi_ioc_transfer tr[8];
	uint8_t tx[8], rx[8];
	int i;

	memset(tr, 0, sizeof(tr));
	memset(tx, 0, sizeof(tx));
	memset(rx, 0, sizeof(rx));
	tx[0] = (uint8_t)((reg << 1) | 0x80);
	if (msgs == 1) {
		tr[0].tx_buf = (unsigned long)tx;
		tr[0].rx_buf = (unsigned long)rx;
		tr[0].len = len;
		tr[0].speed_hz = 1000000;
		tr[0].bits_per_word = 8;
		if (xfer_msg(tr, 1))
			return -1;
		memcpy(out, rx, len);
		return 0;
	}
	for (i = 0; i < len; i++) {
		tr[i].tx_buf = (unsigned long)&tx[i];
		tr[i].rx_buf = (unsigned long)&rx[i];
		tr[i].len = 1;
		tr[i].speed_hz = 1000000;
		tr[i].bits_per_word = 8;
		tr[i].cs_change = 0;
	}
	if (xfer_msg(tr, len))
		return -1;
	memcpy(out, rx, len);
	return 0;
}

static void idle(void)
{
	usleep(1200 * 1000);
}

static void dump(const char *tag, const uint8_t *rx, uint8_t len)
{
	int i;
	printf("%s rx:", tag);
	for (i = 0; i < len; i++)
		printf(" %02x", rx[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/spidev7.0";
	uint32_t speed = 1000000;
	uint8_t mode = SPI_MODE_0, bits = 8;
	uint8_t rx[8];
	int fd;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	ioctl(fd, SPI_IOC_WR_MODE, &mode);
	ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
	ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

	idle();
	rd_msg(0x37, rx, 2, 1);
	dump("a 单笔2B读     ", rx, 2);

	/* 寄存器扫描:全 18/00/FF=总线或芯片死;有真实默认值=芯片活 */
	{
		static const uint8_t regs[] = { 0x01, 0x04, 0x06, 0x07, 0x08,
			0x0A, 0x11, 0x14, 0x15, 0x26 };
		uint8_t v;
		int i;
		printf("reg扫描:");
		for (i = 0; i < (int)(sizeof(regs) / sizeof(regs[0])); i++) {
			rd_msg(regs[i], &v, 2, 1);
			printf(" %02x=%02x", regs[i], v);
		}
		printf("\n");
	}

	idle();
	rd_msg(0x37, rx, 4, 1);
	dump("b 单笔4B读     ", rx, 4);

	idle();
	rd_msg(0x37, rx, 4, 4);
	dump("c 单消息4笔1B  ", rx, 4);

	idle();
	rd_msg(0x37, rx, 2, 1);
	dump("d-1 两消息第1笔", rx, 2);
	rd_msg(0x37, rx, 2, 1);
	dump("d-2 两消息第2笔", rx, 2);

	idle();
	rd_msg(0x37, rx, 2, 1);
	dump("e-1 复核       ", rx, 2);
	return 0;
}
