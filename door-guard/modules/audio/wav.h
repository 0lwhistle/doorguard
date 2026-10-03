/*
 * wav.h — WAV(RIFF/PCM)文件头解析(modules/audio;纯函数,宿主可测)
 *
 * 只收 PCM16(压缩格式/非 16bit 拒收):语音提示素材由 TTS/录音产出,
 * 规格在 README 钉死 48k/16bit/(单|双)声道;宽解码不属于门禁职责。
 */
#ifndef DG_WAV_H
#define DG_WAV_H

#include <stddef.h>
#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned rate;                          /**< 采样率(本工程恒 48000) */
    unsigned channels;                      /**< 1 = 单声道 / 2 = 立体声 */
    unsigned bits;                          /**< 恒 16 */
    size_t   data_off;                      /**< PCM 数据起始偏移(字节) */
    size_t   data_len;                      /**< PCM 数据字节数 */
} wav_info_t;

/** 解析 WAV 头。
 *  @return DG_OK / DG_ERR_PARAM(空参)/ DG_ERR_UNSUPPORTED(非 PCM16 或
 *  声道数不支持)/ DG_ERR_IO(结构损坏:RIFF/WAVE 缺失、无 data 块)。
 *  data_len 超出 buf 实长 = 截断文件 → DG_ERR_IO。 */
int wav_probe(const uint8_t *buf, size_t len, wav_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DG_WAV_H */
