/*
 * rc522_m0.c — K7 门禁 MFRC522 读头 SPI 验线工具(里程碑 M0)
 *
 * 为什么有它:内核驱动 .ko 是练习者手写的(HANDOFF §6.1 分工),驱动写错和
 * 接线接错会互相污染。本工具在写驱动之前用 spidev 用户态把「模块 + 接线 +
 * SPI4 链路」单独验干净:读 MFRC522 版本寄存器 0x37,应答 0x91/0x92 即通。
 *
 * 用法:rc522_m0 [-d /dev/spidev4.0] [-s 4000000] [-r 次数]
 *   -r N  连读 N 次比对一致(查虚焊/干扰/供电跌落);-r 0 = 常驻循环,Ctrl-C 退出
 *
 * 判读表、接线表、编译命令见同目录 README.md;板上需已存在 /dev/spidev4.0
 * (20261006 M0 镜像与此前 KickPi 出厂镜像均具备)。
 * 边界:本工具只覆盖 M0,不依赖 door-guard 任何代码;驱动契约见
 * docs/tech/ICCARD_PROTOCOL.md §2~§6(M1~M4 由驱动 .ko 实现)。
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/spi/spidev.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define REG_VERSION   0x37u
#define DFL_DEV       "/dev/spidev4.0"
#define DFL_SPEED_HZ  4000000u /* RC522 上限 10M,按 HANDOFF §6.2 保守起步 */

static volatile sig_atomic_t g_stop;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* 读一个寄存器:地址字节 (reg<<1)|0x80,后跟 1 个 dummy,数据在 dummy 期间移出 */
static int rc522_read_reg(int fd, uint32_t speed_hz, uint8_t reg, uint8_t *val)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) | 0x80u), 0x00u };
    uint8_t rx[2] = { 0u, 0u };
    struct spi_ioc_transfer tr;

    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    tr.len = 2;
    tr.speed_hz = speed_hz;
    tr.bits_per_word = 8;
    if (ioctl(fd, SPI_IOC_MESSAGE(1), &tr) < 0)
        return -errno;
    *val = rx[1];
    return 0;
}

static const char *version_str(uint8_t v)
{
    switch (v) {
    case 0x91:
        return "MFRC522 v1.0 —— 链路通";
    case 0x92:
        return "MFRC522 v2.0 —— 链路通";
    case 0x88:
    case 0x89:
        return "兼容芯片(FM17522 一类)—— 链路通";
    case 0x00:
        return "MISO 无数据 —— 查 3V3/GND、SCK/MOSI/MISO(8/10/12 脚)、CS(16 脚)";
    case 0xFF:
        return "MISO 恒高 —— 查 MISO(12 脚)是否接错位 / 模块供电 / RST 是否拉高";
    default:
        return "非常见版本号:链路疑似通,记下该值回报(可能为小众兼容芯片)";
    }
}

int main(int argc, char **argv)
{
    const char *dev = DFL_DEV;
    uint32_t speed = DFL_SPEED_HZ;
    long repeat = 1;
    int opt, fd, fail = 0;
    long i;

    while ((opt = getopt(argc, argv, "d:s:r:h")) != -1) {
        switch (opt) {
        case 'd':
            dev = optarg;
            break;
        case 's':
            speed = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'r':
            repeat = strtol(optarg, NULL, 0);
            break;
        default:
            fprintf(stderr, "用法: %s [-d 设备] [-s 速率Hz] [-r 次数(0=循环)]\n", argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }
    if (speed == 0)
        speed = DFL_SPEED_HZ;

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror(dev);
        fprintf(stderr, "板上没有该节点?确认镜像含 spidev(dtsi spi4/spidev@0 已使能)\n");
        return 1;
    }

    uint8_t mode = SPI_MODE_0; /* MFRC522: Mode 0、MSB first,写错 mode 表现常为全 0/全 FF */
    uint8_t bits = 8;
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) {
        perror("SPI_IOC_WR_MODE");
        close(fd);
        return 1;
    }
    if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
        perror("SPI_IOC_WR_BITS_PER_WORD");
        close(fd);
        return 1;
    }

    signal(SIGINT, on_sigint);

    for (i = 0; !g_stop && (repeat == 0 || i < repeat); i++) {
        uint8_t ver = 0;
        int rc = rc522_read_reg(fd, speed, REG_VERSION, &ver);
        if (rc < 0) {
            fprintf(stderr, "[%ld] SPI 传输失败: %s\n", i, strerror(-rc));
            fail = 1;
        } else {
            printf("[%ld] VersionReg(0x37) = 0x%02X  %s\n", i, ver, version_str(ver));
            if (ver != 0x91 && ver != 0x92 && ver != 0x88 && ver != 0x89)
                fail = 1;
        }
        if (repeat == 0 || repeat > 1)
            usleep(300 * 1000);
    }

    close(fd);
    return fail;
}
