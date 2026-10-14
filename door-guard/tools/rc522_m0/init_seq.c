/*
 * init_seq.c v6 — SPI 速度 A/B:同一读法(单笔 2B 全双工),先 1MHz 后 4MHz
 * 各连读 6 次(300ms 间隔)。定位「驱动 1M 失败 / spidev 4M 成功」的速度变量。
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
static uint32_t cur_speed;

static int rd(uint8_t reg, uint8_t *val)
{
	uint8_t tx[2] = { (uint8_t)((reg << 1) | 0x80), 0x00 };
	uint8_t rx[2] = { 0, 0 };
	struct spi_ioc_transfer tr;
	int ret;

	memset(&tr, 0, sizeof(tr));
	tr.tx_buf = (unsigned long)tx;
	tr.rx_buf = (unsigned long)rx;
	tr.len = 2;
	tr.speed_hz = cur_speed;
	tr.bits_per_word = 8;
	ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr) < 0 ? -1 : 0;
	*val = rx[1];
	return ret;
}

static void burst(const char *tag, uint32_t speed)
{
	uint8_t v;
	int i;

	cur_speed = speed;
	printf("%s:", tag);
	for (i = 0; i < 6; i++) {
		v = 0;
		rd(0x37, &v);
		printf(" %02x", v);
		usleep(300 * 1000);
	}
	printf("\n");
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/spidev7.0";
	uint8_t mode = SPI_MODE_0, bits = 8;


	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	ioctl(fd, SPI_IOC_WR_MODE, &mode);
	ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);

	burst("1MHz 连读 6 次", 1000000);
	burst("4MHz 连读 6 次", 4000000);
	burst("1MHz 复测      ", 1000000);
	return 0;
}
