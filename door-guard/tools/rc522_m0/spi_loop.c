/*
 * spi_loop.c — spi-gpio 回环验线工具(2026-10-14,RC522 软 SPI 排障)
 *
 * 用法:板端把 MISO 与 MOSI 两根线在板端(40pin 处)短接后运行:
 *   spi_loop /dev/spidev7.0
 * 全双工发送已知字节流。线回环(MISO≡MOSI)下 rx 与 tx 逐字节同相一致
 * (直连无延时;「滞后一字节」是从机应答式的行为,线回环不是)——
 * 回读一致 = SCK/MOSI/MISO 板侧路径全通,问题在模块端;不一致 =
 * SCK/MOSI 两脚有错(互换/错位/没接上)。
 *
 * 编译(WSL,静态省 glibc 顾虑):
 *   $TC -O2 -Wall -static spi_loop.c -o spi_loop
 */
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/spidev7.0";
	uint8_t tx[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x5A, 0xA5, 0x00, 0xFF };
	uint8_t rx[8] = { 0 };
	uint8_t mode = SPI_MODE_0;
	uint32_t speed = 1000000;
	struct spi_ioc_transfer tr;
	int fd, i, ok;

	memset(&tr, 0, sizeof(tr));
	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	ioctl(fd, SPI_IOC_WR_MODE, &mode);
	ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
	tr.tx_buf = (unsigned long)tx;
	tr.rx_buf = (unsigned long)rx;
	tr.len = sizeof(tx);
	tr.speed_hz = speed;
	tr.bits_per_word = 8;
	if (ioctl(fd, SPI_IOC_MESSAGE(1), &tr) < 0) {
		perror("transfer");
		return 1;
	}
	printf("tx:");
	for (i = 0; i < 8; i++)
		printf(" %02X", tx[i]);
	printf("\nrx:");
	for (i = 0; i < 8; i++)
		printf(" %02X", rx[i]);
	ok = memcmp(rx, tx, sizeof(tx)) == 0;
	printf("\n%s\n", ok ? "LOOPBACK OK — SCK/MOSI/MISO 板侧路径全通,查模块端" :
	       "LOOPBACK FAIL — SCK/MOSI 两脚有错(互换/错位/悬空)");
	return ok ? 0 : 1;
}
