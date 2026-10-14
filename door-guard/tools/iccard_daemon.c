/*
 * iccard_daemon.c — IC 读卡用户态守护进程(A 路方案,2026-10-14)
 *
 * 背景:k7_rc522 内核驱动在 spi-gpio 总线上 probe 必败(2026-10-14 全天
 * 排障:速度/缓冲/API/结构/复位全部排除,夹心实验证明芯片活着应答 spidev
 * 而不应答内核传输),且 rmmod 段错误两次卡死板子。故内核侧退出 IC 业务,
 * 改由本进程经 /dev/spidev 直接驱动 RC522。
 *
 * 代码血统(两份已验证来源拼合):
 *   - SPI 访问 = rc522_m0 单笔 2B 全双工 @4MHz(2026-10-14 全天零失败)
 *   - 协议    = rc522_esp32_test v4.1 裁判台(REQA→级联防冲突→选卡→UID,
 *               选卡 CRC 用芯片 CalcCRC,ESP32 满血验证)
 *   - 帧契约  = iccard_hal.h 的 dg_iccard_frame_t(24B packed,逐字节同构),
 *               经 FIFO 交付应用;iccard_hal 的 pipe 回退(ENOTTY 排水)
 *               天然兼容 FIFO 后端,应用零改动,仅配置 dev_path 指向 FIFO。
 *
 * 与内核驱动的行为差异(有意为之):
 *   - 无软复位/硬复位:RST 直连 3V3,芯片 POR 自醒;实测软复位写会打哑
 *     应答中的芯片(2026-10-14),配置序列不含任何复位;
 *   - 无环形缓冲/stats ioctl:帧直接进 FIFO,应用侧 iccard_hal_flush 的
 *     ENOTTY 排水回退已覆盖;若应用未起,FIFO 无读者时帧被丢弃(事件性
 *     数据,丢弃等价于"驱动没扫到",应用重开 FIFO 后从新帧开始)。
 *
 * 编译(WSL):见同目录 build_iccard_daemon.sh;板上部署:
 *   scp iccard_daemon root@板:/userdata/doorguard/
 *   /etc/init.d/S60doorguard 里的 spidev 绑定钩子会在开机时把 spidev
 *   绑到 spi-gpio 总线上的 rc522 设备(产出发 /dev/spidevX.Y)。
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- 帧契约(与 iccard_hal.h 逐字节同构;二进制契约,勿单边改) ---- */
#define DG_ICCARD_MAGIC    0x30434349UL
#define DG_ICCARD_UID_MAX  16
#define DG_ICCARD_FRAME_SZ 24
#define DG_ICCARD_TYPE_MF_CLASSIC 1

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint8_t  uid_len;
	uint8_t  card_type;
	uint16_t seq;
	uint8_t  uid[DG_ICCARD_UID_MAX];
} dg_frame_t;

/* ---- MFRC522 寄存器/命令(与裁判台/驱动同源) ---- */
#define R_COMMAND 0x01
#define R_COM_IRQ 0x04
#define R_DIV_IRQ 0x05
#define R_ERROR   0x06
#define R_STATUS2 0x08
#define R_FIFO    0x09
#define R_FIFOLVL 0x0A
#define R_CONTROL 0x0C
#define R_BITFRM  0x0D
#define R_COLL    0x0E
#define R_MODE    0x11
#define R_TXASK   0x15
#define R_RXSEL   0x17
#define R_RFCFG   0x26
#define R_CRCH    0x21
#define R_CRCL    0x22
#define R_TMODE   0x2A
#define R_TPRESC  0x2B
#define R_TRELO_H 0x2C
#define R_TRELO_L 0x2D
#define R_TXCTL   0x14
#define R_VERSION 0x37

#define CMD_IDLE    0x00
#define CMD_CALCCRC 0x03
#define CMD_TRANSCEIVE 0x0C
#define CMD_RESETPHASE 0x0F

#define IRQ_RX    0x20
#define IRQ_TIMER 0x01
#define IRQ_ALL   0x7F
#define FLUSH_BUF 0x80
#define STARTSEND 0x80
#define ANT_ON    0x03
#define ERR_HARD  0x1B

#define PICC_REQIDL 0x26
#define CL1 0x93
#define CL2 0x95
#define SAK_CASCADE 0x04
#define CT 0x88

#define SPI_SPEED  100000
#define SCAN_MS    50
#define STAT_MS    20000
#define SAME_MS    500
#define FIFO_PATH  "/tmp/dg_iccard.fifo"

static volatile sig_atomic_t g_stop;
static int g_spi = -1;
static int g_fifo = -1;
static int g_cs = -1;	/* 手工 CS 的 sysfs value fd;>=0 时 SPI_NO_CS+自控沿 */
static uint16_t g_seq;
/* 命中率观测(ESP32 裁判台时代 RF 命中率 ~2/20 未结案,K7 侧首轮必须量化) */
static uint32_t g_n_scan, g_n_atqa, g_n_uid;

/* 手工 CS:K7 实测「读通/写全死」精确分离——写锁存依赖 CS 上升沿,读在
 * CS 低期间即可取数;若 spi-gpio 内建 CS 的锁存沿缺失,症状正是如此。
 * SPI_NO_CS + sysfs GPIO 自控沿可判别:手工 CS 写生效=内建 CS 时序问题 */
static void cs_set(int hi)
{
	if (g_cs < 0)
		return;
	lseek(g_cs, 0, SEEK_SET);
	if (write(g_cs, hi ? "1" : "0", 1) < 0) {
		/* sysfs 偶发 EBUSY 不致命:下一笔事务再试 */
	}
}

/* ---- SPI:单笔 2B 全双工(rc522_m0 同款,今日零失败) ---- */
static int spi_xfer(const uint8_t *tx, uint8_t *rx, uint8_t len)
{
	struct spi_ioc_transfer tr;
	int ret;

	memset(&tr, 0, sizeof(tr));
	tr.tx_buf = (unsigned long)tx;
	tr.rx_buf = (unsigned long)rx;
	tr.len = len;
	tr.speed_hz = SPI_SPEED;
	tr.bits_per_word = 8;
	cs_set(0);
	ret = ioctl(g_spi, SPI_IOC_MESSAGE(1), &tr);
	cs_set(1);
	return (ret < 0) ? -1 : 0;
}

static int wr(uint8_t reg, uint8_t val)
{
	uint8_t tx[2] = { (uint8_t)(reg << 1), val };
	return spi_xfer(tx, NULL, 2);
}

/* 读寄存器,带「地址回波」检测重读:杜邦线串扰下 MISO 会间歇输出 MOSI
 * 延迟 8 拍的副本(10-14 定性,版本寄存器读回 0xEE 正是 0x37 的地址字节),
 * 读回值恰等于本笔地址字节即判回波,重读(合法寄存器值撞上该字节的概率
 * 极低,重读成本低);3 次全回波 = 链路性故障,返回 -1 让调用方可见 */
static int rd(uint8_t reg, uint8_t *val)
{
	uint8_t tx[2] = { (uint8_t)((reg << 1) | 0x80), 0x00 };
	uint8_t rx[2];
	int i;

	for (i = 0; i < 3; i++) {
		if (spi_xfer(tx, rx, 2))
			return -1;
		if (rx[1] != tx[0]) {
			*val = rx[1];
			return 0;
		}
		usleep(200);
	}
	return -1;
}

static void clear_bits(uint8_t reg, uint8_t mask)
{
	uint8_t v;
	if (!rd(reg, &v))
		wr(reg, (uint8_t)(v & ~mask));
}

/* ---- 芯片 CRC(CalcCRC 命令,裁判台 v3 验证过 MATCH) ---- */
static int chip_crc(const uint8_t *data, uint8_t len, uint8_t out[2])
{
	uint8_t v, hi, lo;
	int i, ret = 0;

	ret |= wr(R_COMMAND, CMD_IDLE);
	ret |= wr(R_FIFOLVL, FLUSH_BUF);
	for (i = 0; i < len; i++)
		ret |= wr(R_FIFO, data[i]);
	ret |= wr(R_COMMAND, CMD_CALCCRC);
	if (ret)
		return -1;
	for (i = 0; i < 20; i++) {
		usleep(500);
		if (rd(R_DIV_IRQ, &v))
			return -1;
		if (v & 0x04)
			break;
	}
	if (i >= 20)
		return -1;
	if (rd(R_CRCH, &hi) || rd(R_CRCL, &lo))
		return -1;
	wr(R_COMMAND, CMD_IDLE);
	out[0] = lo;
	out[1] = hi;
	return 0;
}

/* ---- 一发收(bench 同款);ret 0=有数据 -1=超时 -2=硬错 -3=SPI 错 ---- */
static int transceive(const uint8_t *tx, uint8_t tx_len, uint8_t *rx,
		      uint8_t rx_max, uint8_t last_bits, uint8_t *rx_len)
{
	uint8_t irq = 0, err = 0, n;
	int i, ret = -1;

	if (wr(R_COMMAND, CMD_IDLE) || wr(R_FIFOLVL, FLUSH_BUF) ||
	    wr(R_COM_IRQ, IRQ_ALL) || wr(R_BITFRM, last_bits & 0x07))
		return -3;
	for (i = 0; i < tx_len; i++)
		if (wr(R_FIFO, tx[i]))
			return -3;
	if (wr(R_COMMAND, CMD_TRANSCEIVE) ||
	    wr(R_BITFRM, (uint8_t)((last_bits & 0x07) | STARTSEND)))
		return -3;
	for (i = 0; i < 80; i++) {
		usleep(500);
		if (rd(R_COM_IRQ, &irq))
			return -3;
		if (irq & (IRQ_RX | IRQ_TIMER))
			break;
	}
	if (!(irq & IRQ_RX))
		goto out;
	if (rd(R_ERROR, &err))
		return -3;
	if (err & ERR_HARD)
		ret = -2;
	else {
		if (rd(R_FIFOLVL, &n))
			return -3;
		*rx_len = (n > rx_max) ? rx_max : n;
		for (i = 0; i < *rx_len; i++)
			if (rd(R_FIFO, &rx[i]))
				return -3;
		ret = 0;
	}
out:
	wr(R_COMMAND, CMD_IDLE);
	wr(R_COM_IRQ, IRQ_ALL);
	wr(R_BITFRM, 0);
	return ret;
}

/* ---- 协议:一层防冲突+选卡(bench 同序:清加密位/清 Coll/防冲突/选卡) ---- */
static int anticoll_select(uint8_t clv, uint8_t *sak, uint8_t *uid4)
{
	uint8_t tx[9], rx[5], crc[2], n;
	int ret;

	clear_bits(R_STATUS2, 0x08);	/* MFCrypto1On=0 */
	clear_bits(R_COLL, 0x80);	/* ValuesAfterColl=0 */

	tx[0] = clv;
	tx[1] = 0x20;
	n = 0;
	ret = transceive(tx, 2, rx, 5, 0, &n);
	if (ret)
		return ret;
	if (n != 5)
		return -2;
	if ((uint8_t)(rx[0] ^ rx[1] ^ rx[2] ^ rx[3]) != rx[4])
		return -2;
	memcpy(uid4, rx, 4);

	tx[0] = clv;
	tx[1] = 0x70;
	memcpy(&tx[2], rx, 4);
	tx[6] = rx[4];
	if (chip_crc(tx, 7, crc))
		return -3;
	tx[7] = crc[0];
	tx[8] = crc[1];
	n = 0;
	ret = transceive(tx, 9, sak, 1, 0, &n);
	if (ret)
		return ret;
	return (n == 1) ? 0 : -2;
}

/* 写后校验重写:写通道间歇性丢写(K7 线缆边际,读侧同源回波),每笔配置
 * 写循环「写→读→不匹配重写」直到读回一致即锁存成功;一次性 init 用,
 * 运行期 transceive 的写丢失由 50ms 扫描自然重试覆盖 */
static int wr_verify(uint8_t reg, uint8_t val, int tries)
{
	uint8_t rb;
	int i;

	for (i = 0; i < tries; i++) {
		wr(reg, val);
		usleep(3000);
		if (rd(reg, &rb) == 0 && rb == val)
			return 0;
		if ((i & 7) == 7)
			usleep(20000);	/* 好窗口稀疏:周期拉长等待跨过坏段 */
	}
	return -1;
}

/* 0=无卡;1=UID 到手(级联结构与裁判台 get_uid 逐字同构);atqa_hit 出参:
 * REQA 是否拿到 ATQA——uid 失败但 atqa 命中=防冲突/选卡段问题,区分用 */
static int read_card(uint8_t *uid, uint8_t *uid_len, uint8_t *sak,
		     int *atqa_hit)
{
	uint8_t seg[4];
	int ret, cl, n = 0;

	uint8_t atqa[2] = { 0 };
	uint8_t cmd = PICC_REQIDL;
	uint8_t n8 = 0;

	*atqa_hit = 0;
	ret = transceive(&cmd, 1, atqa, 2, 7, &n8);
	if (ret != 0 || n8 < 2)
		return 0;
	*atqa_hit = 1;

	for (cl = 0; cl < 3; cl++) {
		cmd = (cl == 0) ? CL1 : (cl == 1) ? CL2 : 0x97;
		ret = anticoll_select(cmd, sak, seg);
		if (ret)
			return 0;
		if (cl == 0) {
			memcpy(uid, seg, 4);
			n = 4;
		} else {
			if (seg[0] != CT)
				return 0;
			memcpy(uid + n, seg + 1, 3);
			n += 3;
		}
		if (!(*sak & SAK_CASCADE))
			break;
	}
	*uid_len = (uint8_t)n;
	return 1;
}

/* ---- FIFO:应用在则写帧,不在则丢(应用重开后从新帧开始) ---- */
static void fifo_emit(const dg_frame_t *f)
{
	ssize_t n = write(g_fifo, f, DG_ICCARD_FRAME_SZ);

	if (n == (ssize_t)sizeof(*f))
		return;
	if (errno == EPIPE || errno == EBADF || errno == ENXIO) {
		close(g_fifo);
		g_fifo = -1;
	} else if (errno == EAGAIN) {
		/* 应用读得慢:FIFO 满,丢帧(250ms 扫描,下一帧马上来) */
	}
}

static void on_sig(int sig)
{
	(void)sig;
	g_stop = 1;
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	const char *spi_dev = argc > 1 ? argv[1] : "/dev/spidev7.0";
	const char *fifo = argc > 2 ? argv[2] : FIFO_PATH;
	int cs_gpio = argc > 3 ? atoi(argv[3]) : 0;
	uint8_t mode = SPI_MODE_0, bits = 8;
	uint32_t speed = SPI_SPEED;
	uint8_t v = 0;
	int i;

	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	/* 手工 CS:第三个参数给 GPIO 编号即启用(实验判别内建 CS 锁存沿) */
	if (cs_gpio > 0) {
		char p[64];
		int fd;
		snprintf(p, sizeof(p), "/sys/class/gpio/export");
		fd = open(p, O_WRONLY);
		if (fd >= 0) {
			dprintf(fd, "%d", cs_gpio);
			close(fd);	/* 已导出时 EBUSY 忽略 */
		}
		snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/direction", cs_gpio);
		fd = open(p, O_WRONLY);
		if (fd < 0) {
			fprintf(stderr, "gpio%d direction: %s\n", cs_gpio, strerror(errno));
			return 1;
		}
		if (write(fd, "out", 3) < 0) {
			fprintf(stderr, "gpio%d set out: %s\n", cs_gpio, strerror(errno));
			return 1;
		}
		close(fd);
		snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/value", cs_gpio);
		g_cs = open(p, O_WRONLY);
		if (g_cs < 0) {
			fprintf(stderr, "gpio%d value: %s\n", cs_gpio, strerror(errno));
			return 1;
		}
		cs_set(1);	/* 空闲高 */
		mode |= SPI_NO_CS;
		fprintf(stderr, "manual CS on gpio%d (SPI_NO_CS)\n", cs_gpio);
	}

	/* SPI 打开 + 芯片配置(无任何复位:RST 直连 3V3,POR 自醒) */
	g_spi = open(spi_dev, O_RDWR);
	if (g_spi < 0) {
		fprintf(stderr, "open %s: %s\n", spi_dev, strerror(errno));
		return 1;
	}
	ioctl(g_spi, SPI_IOC_WR_MODE, &mode);
	ioctl(g_spi, SPI_IOC_WR_BITS_PER_WORD, &bits);
	ioctl(g_spi, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

	/* 版本探针:最多等 ~3s(芯片上电首笔可能哑,重试到活) */
	for (i = 0; i < 10; i++) {
		if (!rd(R_VERSION, &v) && (v == 0x91 || v == 0x92))
			break;
		fprintf(stderr, "probe %d: version=0x%02x, retry...\n", i, v);
		usleep(300 * 1000);
	}
	if (v != 0x91 && v != 0x92) {
		fprintf(stderr, "MFRC522 silent (0x%02x), exit\n", v);
		return 1;
	}
	fprintf(stderr, "MFRC522 version 0x%02x — OK\n", v);

	/* 唤醒:POR 后 CommandReg=0x20(PowerDown),写 IDLE 清位并等起振 */
	wr(R_COMMAND, CMD_IDLE);
	usleep(50 * 1000);

	/* 就绪+配置:写通道间歇丢写,每笔配置写带校验重写(见 wr_verify);
	 * RFCfg=0xA5 首验兼作「写通道活了」的门禁 */
	if (wr_verify(R_RFCFG, 0xA5, 100)) {
		fprintf(stderr, "MFRC522 write path dead, exit\n");
		return 1;
	}
	if (wr_verify(R_TMODE, 0x8D, 30) ||
	    wr_verify(R_TPRESC, 0x3E, 30) ||
	    wr_verify(R_TRELO_H, 0, 30) ||
	    wr_verify(R_TRELO_L, 30, 30) ||
	    wr_verify(R_TXASK, 0x40, 30) ||
	    wr_verify(R_MODE, 0x3D, 30) ||
	    wr_verify(R_RXSEL, 0x86, 30) ||
	    wr_verify(R_RFCFG, 0x7F, 30) ||
	    wr_verify(R_COM_IRQ, IRQ_ALL, 30)) {
		fprintf(stderr, "config write failed, exit\n");
		return 1;
	}
	if (!rd(R_TXCTL, &v))
		wr_verify(R_TXCTL, (uint8_t)(v | ANT_ON), 30);
	wr(R_COMMAND, CMD_IDLE);

	/* RF 关键寄存器回读:txctl 低 2 位=天线使能(0x03,静默未开=恒零命中),
	 * rxsel/rfcfg=接收链补配(0x86/0x7F)——恒零命中时先看这行 */
	{
		uint8_t t1 = 0, t2 = 0, t3 = 0;
		if (!rd(R_TXCTL, &t1) && !rd(R_RXSEL, &t2) && !rd(R_RFCFG, &t3))
			fprintf(stderr, "cfg: txctl=0x%02x rxsel=0x%02x rfcfg=0x%02x\n",
				t1, t2, t3);
	}

	/* FIFO:应用未起时在此重试 open(读者出现才成功) */
	mkfifo(fifo, 0666);

	fprintf(stderr, "iccard_daemon ready: spi=%s fifo=%s scan=%dms\n",
		spi_dev, fifo, SCAN_MS);

	uint8_t uid[16], uid_len = 0, sak = 0;
	int atqa_hit;
	uint8_t last_uid[16];
	uint8_t last_len = 0;
	uint64_t last_ok_ms = 0, next_stat;
	uint64_t next_scan = now_ms();

	next_stat = now_ms() + STAT_MS;

	while (!g_stop) {
		uint64_t t = now_ms();
		if (t < next_scan) {
			usleep((useconds_t)(next_scan - t) * 1000);
			continue;
		}
		next_scan = t + SCAN_MS;

		if (t >= next_stat) {
			fprintf(stderr,
				"stats: scan=%u atqa=%u uid=%u(20s 窗口)\n",
				g_n_scan, g_n_atqa, g_n_uid);
			g_n_scan = g_n_atqa = g_n_uid = 0;
			next_stat = t + STAT_MS;
		}
		g_n_scan++;

		uid_len = 0;
		sak = 0;
		if (!read_card(uid, &uid_len, &sak, &atqa_hit))
			continue;
		if (atqa_hit)
			g_n_atqa++;
		if (uid_len < 4)
			continue;
		/* 同卡冷却:50ms 扫描下同卡在场会 20 帧/s 洪水,daemon 侧压制 */
		if (uid_len == last_len && !memcmp(uid, last_uid, uid_len) &&
		    t - last_ok_ms < SAME_MS)
			continue;
		g_n_uid++;
		memcpy(last_uid, uid, uid_len);
		last_len = uid_len;
		last_ok_ms = t;

		if (g_fifo < 0) {
			g_fifo = open(fifo, O_WRONLY | O_NONBLOCK);
			if (g_fifo < 0)
				continue;	/* 应用未起,帧丢弃 */
		}

		dg_frame_t f;
		memset(&f, 0, sizeof(f));
		f.magic = DG_ICCARD_MAGIC;
		f.uid_len = uid_len;
		f.card_type = ((sak & 0x18) == 0x08) ?
			      DG_ICCARD_TYPE_MF_CLASSIC :
			      0xFF;
		f.seq = ++g_seq;
		memcpy(f.uid, uid, uid_len);
		fifo_emit(&f);
	}

	if (g_fifo >= 0)
		close(g_fifo);
	close(g_spi);
	return 0;
}
