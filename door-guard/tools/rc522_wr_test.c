/*
 * rc522_wr_test.c — K7 写路径判别实验(2026-10-14)
 *
 * 背景:daemon 实测「读通/写死」——版本寄存器可读 0x92,配置寄存器写后
 * 回读恒复位值。写路径是否彻底死,FIFO 回环一次定性:FIFOReg 是纯数据口,
 * 写入即计数,不涉任何配置语义。
 *
 * 判读:FIFOLVL 写后=3 → 写路径健康(问题在别处);
 *       =0 → 写死(CS 锁存/MOSI 数据/电气),转硬件排障。
 * 编译(WSL): aarch64-none-linux-gnu-gcc -O2 -o rc522_wr_test rc522_wr_test.c
 */
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define R_COMMAND 0x01
#define R_FIFO    0x09
#define R_FIFOLVL 0x0A
#define CMD_IDLE  0x00

static int g_fd;
static int xfer(const uint8_t *tx, uint8_t *rx, uint8_t len)
{
	struct spi_ioc_transfer tr;

	memset(&tr, 0, sizeof(tr));
	tr.tx_buf = (unsigned long)tx;
	tr.rx_buf = (unsigned long)rx;
	tr.len = len;
	tr.speed_hz = 1000000;
	tr.bits_per_word = 8;
	return ioctl(g_fd, SPI_IOC_MESSAGE(1), &tr) < 0 ? -1 : 0;
}

static int wr(uint8_t r, uint8_t v)
{
	uint8_t t[2] = { (uint8_t)(r << 1), v };

	return xfer(t, NULL, 2);
}

static int rd(uint8_t r, uint8_t *v)
{
	uint8_t t[2] = { (uint8_t)((r << 1) | 0x80), 0 };
	uint8_t x[2];
	int i, echo = 0;

	for (i = 0; i < 5; i++) {
		if (xfer(t, x, 2))
			return -1;
		if (x[1] != t[0]) {
			*v = x[1];
			printf("  [rd 0x%02x] = 0x%02x%s\n", r, x[1],
			       echo ? " (回波后重试)" : "");
			return 0;
		}
		echo++;
	}
	printf("  [rd 0x%02x] 连续 5 次地址回波 0x%02x —— MISO 虚接特征\n", r, t[0]);
	*v = x[1];
	return -1;
}

static int wr_chk(uint8_t r, uint8_t v)
{
	printf("  [wr 0x%02x] = 0x%02x\n", r, v);
	return wr(r, v);
}

/* 静默读(统计模式用,免刷屏) */
static int rd_q(uint8_t r, uint8_t *v)
{
	uint8_t t[2] = { (uint8_t)((r << 1) | 0x80), 0 };
	uint8_t x[2];
	int i;

	for (i = 0; i < 3; i++) {
		if (xfer(t, x, 2))
			return -1;
		if (x[1] != t[0]) {
			*v = x[1];
			return 0;
		}
		usleep(300);
	}
	return -1;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/spidev7.0";
	uint8_t mode = 0, bits = 8;
	uint32_t speeds[2] = { 1000000, 100000 };
	uint8_t v;
	int i, s;

	g_fd = open(dev, O_RDWR);
	if (g_fd < 0) {
		perror(dev);
		return 1;
	}
	ioctl(g_fd, SPI_IOC_WR_MODE, &mode);
	ioctl(g_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
	ioctl(g_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speeds[0]);

	/* 统计模式:对关键寄存器各 50 轮写→读,量化写通道间歇率 */
	if (argc > 2 && argv[2][0] == 'w') {
		struct { uint8_t reg, val; const char *name; } t[] = {
			{ 0x26, 0xA5, "RFCfg" }, { 0x11, 0x3D, "Mode" },
			{ 0x17, 0x86, "RxSel" }, { 0x14, 0x83, "TxCTL" },
			{ 0x02, 0x7F, "ComIEn" },
		};
		int r, k, ok;
		for (r = 0; r < 5; r++) {
			ok = 0;
			for (k = 0; k < 50; k++) {
				wr(t[r].reg, t[r].val);
				usleep(2000);
				if (rd_q(t[r].reg, &v) == 0 && v == t[r].val)
					ok++;
			}
			printf("%-7s(0x%02x<-0x%02x): %d/50 写读一致\n",
			       t[r].name, t[r].reg, t[r].val, ok);
		}
		close(g_fd);
		return 0;
	}

	/* 仲裁模式:c = CalccRC 硬件动作自证——写链路生死不依赖回读路径:
	 * FIFO 写 4B + CommandReg=CALCCRC,芯片硬件算 CRC,读只读结果寄存器
	 * 对照软件预期。结果非零且一致 = FIFO 写/命令写真实生效,「写死」是
	 * 回读(MISO)假象;零/超时 = 写链路真死,矛头指向 spi-gpio 时序 */
	if (argc > 2 && argv[2][0] == 'c') {
		uint8_t d[4] = { 0x12, 0x34, 0x56, 0x78 };
		uint16_t expect = 0x6363;
		uint8_t hi, lo, hi2, lo2;
		int k;

		for (i = 0; i < 4; i++) {
			expect ^= d[i];
			for (k = 0; k < 8; k++)
				expect = (expect & 1) ?
					 (uint16_t)((expect >> 1) ^ 0x8408) :
					 (uint16_t)(expect >> 1);
		}
		printf("expect CRC L=%02x H=%02x\n", expect & 0xff, expect >> 8);

		rd(0x01, &v);
		wr(R_FIFOLVL, 0x80);
		usleep(1000);
		for (i = 0; i < 4; i++)
			wr(R_FIFO, d[i]);
		wr(R_COMMAND, 0x03);		/* CALCCRC */
		for (k = 0; k < 50; k++) {
			usleep(500);
			rd_q(0x05, &v);		/* DivIrq bit2 = CRC ready */
			if ((v & 0x04))
				break;
		}
		rd(0x22, &lo);
		rd(0x21, &hi);
		rd(0x22, &lo2);
		rd(0x21, &hi2);
		wr(R_COMMAND, CMD_IDLE);
		printf("CRC result: L=%02x H=%02x (双读 L=%02x H=%02x)\n",
		       lo, hi, lo2, hi2);
		printf(((lo == (expect & 0xff) && hi == (expect >> 8)) ?
			"--> CRC 匹配:写链路在芯片侧真实生效!\n" :
			"--> CRC 不匹配/超时:写链路真死\n"));
		close(g_fd);
		return 0;
	}

	/* dump 模式:全 64 地址读指纹,对照标准 MFRC522 复位值
	 * (0x01=0x20 Command/0x14=0x80 TxCTL/0x26=0x48 RFCfg/0x37=0x91|0x92 Ver) */
	if (argc > 2 && argv[2][0] == 'd') {
		uint8_t a;
		rd(R_COMMAND, &v);		/* 热身 */
		for (a = 0; ; a++) {
			uint8_t t[2] = { (uint8_t)((a << 1) | 0x80), 0 };
			uint8_t x[2], r;
			int e;
			for (e = 0; e < 5; e++) {
				if (xfer(t, x, 2))
					return 1;
				if (x[1] != t[0])
					break;
				usleep(300);
			}
			r = x[1];
			printf("%02x=%02x%s  ", a, r, e >= 5 ? "!" : " ");
			if (a == 0x3f)
				break;
		}
		printf("\n(!=连续5次地址回波)\n");
		close(g_fd);
		return 0;
	}

	for (s = 0; s < 2; s++) {
		ioctl(g_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speeds[s]);
		printf("\n===== speed = %u Hz =====\n", speeds[s]);

		wr(R_COMMAND, CMD_IDLE);		/* 唤醒:写 CommandReg 清 PowerDown */
		usleep(100000);
		rd(0x01, &v);				/* 读回 CommandReg:0x00=唤醒写生效 / 0x20=写死 */
		printf("CommandReg after wake-write: 0x%02x  (0x00=写活 / 0x20=写死)\n", v);

		wr(R_FIFOLVL, 0x80);			/* flush FIFO */
		usleep(500);
		rd(R_FIFOLVL, &v);
		printf("FIFOLVL after flush: %u (expect 0)\n", v);
		wr(R_FIFO, 0x55);
		usleep(500);
		rd(R_FIFOLVL, &v);
		printf("FIFOLVL after 1 write: %u (expect 1)\n", v);

		wr(0x26, 0xA5);				/* RFCfgReg 非默认值 */
		usleep(500);
		rd(0x26, &v);
		printf("RFCfg wrote 0xA5, read 0x%02x (expect 0xA5, reset=0x48)\n", v);

		wr(0x14, 0x83);				/* TxControlReg 开天线 */
		usleep(500);
		rd(0x14, &v);
		printf("TxCTL wrote 0x83, read 0x%02x (expect 0x83, reset=0x80)\n", v);
	}
	(void)i;
	close(g_fd);
	return 0;
}
