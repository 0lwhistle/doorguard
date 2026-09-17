/*
 * test_i18n.c — i18n 一致性测试(Phase 5;此测试不过 Phase 永不通过)
 *
 * 自动化三查:
 *   1. 扫描 ui/ 全部源码的 `_("...")` 键,断言 zh-CN/en-US 两份 json 全覆盖
 *   2. 字符串字面量里的裸中文(未经 _() 包裹)必须为 0
 *   3. zh-CN 全部键的字符在生成字体 dg_font_cn_16 中都有字形
 */
#include "cJSON.h"
#include "dg_test.h"
#include "lvgl.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DG_SOURCE_DIR
#define DG_SOURCE_DIR "."
#endif

#define MAX_KEYS 512
static char s_keys[MAX_KEYS][128];
static int s_key_cnt = 0;

/* ---------- 源码扫描 ---------- */

static bool utf8_cjk(const unsigned char *p, int32_t *cp)
{
    unsigned char c = p[0];
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        return *cp >= 0x80;
    }
    if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return *cp >= 0x2E80;               /* CJK 部首/汉字区起 */
    }
    return false;
}

/** 扫描单个 .c 文件(按行):
 *  - 提取 _("...") 键
 *  - 跳过 DG_LOG 行(约定:日志调用保持单行,是开发者向文案而非 UI label)
 *  - 其余行中字符串字面量含 CJK = 裸中文未包裹,计数失败 */
static void scan_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("  无法打开 %s\n", path);
        dg_fail++;
        return;
    }
    char line[4096];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;

        if (strstr(line, "DG_LOG"))
            continue;

        /* 提取 _("...") */
        const char *p = line;
        while ((p = strstr(p, "_(")) != NULL) {
            const char *q = p + 2;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q == '"') {
                q++;
                char key[128];
                size_t k = 0;
                while (*q && *q != '"' && k < sizeof(key) - 1)
                    key[k++] = *q++;
                key[k] = '\0';
                if (*q == '"' && k > 0) {
                    bool dup = false;
                    for (int i = 0; i < s_key_cnt; i++) {
                        if (!strcmp(s_keys[i], key))
                            dup = true;
                    }
                    if (!dup && s_key_cnt < MAX_KEYS)
                        snprintf(s_keys[s_key_cnt++], 128, "%s", key);
                }
            }
            p += 2;
        }

        /* 裸中文:抹掉 _("...") 片段后,行内剩余字符串字面量含 CJK 即违规 */
        char scrub[4096];
        snprintf(scrub, sizeof(scrub), "%s", line);
        p = scrub;
        while ((p = strstr(p, "_(")) != NULL) {
            char *q = strchr(p, '"');
            if (!q)
                break;
            char *e = strchr(q + 1, '"');
            if (!e)
                break;
            memset(q, ' ', (size_t)(e - q + 1));
            p = e + 1;
        }
        bool in_str = false;
        for (size_t i = 0; i < strlen(scrub); i++) {
            char c = scrub[i];
            if (c == '"' && (i == 0 || scrub[i - 1] != '\\'))
                in_str = !in_str;
            if (in_str && (unsigned char)c >= 0x80) {
                int32_t cp;
                if (utf8_cjk((unsigned char *)&scrub[i], &cp)) {
                    printf("  裸中文(未 _() 包裹): %s:%d\n", path, lineno);
                    dg_fail++;
                    break;
                }
            }
        }
    }
    fclose(f);
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (strstr(ent->d_name, ".c"))
            scan_file(full);
    }
    closedir(d);
}

/** 检查一段文本的全部 CJK 字符在生成字体中有字形 */
static void font_check_text(const char *text)
{
    if (!text)
        return;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        int32_t cp;
        if (!utf8_cjk(p, &cp))
            continue;
        lv_font_glyph_dsc_t dsc;
        if (!lv_font_get_glyph_dsc(&dg_font_cn_16, &dsc, (uint32_t)cp, 0)) {
            printf("  字体缺字形 U+%04X (文本: %s)\n", cp, text);
            dg_fail++;
        }
        p += 2;
    }
}

int main(void)
{
    /* 1. 扫描 ui 根与 widgets 子目录(新子目录在此登记) */
    scan_dir(DG_SOURCE_DIR "/ui");
    scan_dir(DG_SOURCE_DIR "/ui/widgets");
    printf("[I18N] 提取键 %d 个\n", s_key_cnt);
    DG_CHECK(s_key_cnt >= 10);              /* 源码标签规模底线,防扫描失效静默通过 */

    /* 2. 两份 json 全覆盖 */
    char p1[256], p2[256];
    snprintf(p1, sizeof(p1), "%s/ui/lang/zh-CN.json", DG_SOURCE_DIR);
    snprintf(p2, sizeof(p2), "%s/ui/lang/en-US.json", DG_SOURCE_DIR);
    FILE *f1 = fopen(p1, "rb"), *f2 = fopen(p2, "rb");
    DG_CHECK(f1 && f2);
    char b1[8192], b2[8192];
    size_t n1 = f1 ? fread(b1, 1, sizeof(b1) - 1, f1) : 0;
    size_t n2 = f2 ? fread(b2, 1, sizeof(b2) - 1, f2) : 0;
    if (f1) fclose(f1);
    if (f2) fclose(f2);
    b1[n1] = b2[n2] = '\0';
    cJSON *zh = cJSON_Parse(b1), *en = cJSON_Parse(b2);
    DG_CHECK(zh && en);

    for (int i = 0; i < s_key_cnt; i++) {
        const cJSON *vz = cJSON_GetObjectItemCaseSensitive(zh, s_keys[i]);
        const cJSON *ve = cJSON_GetObjectItemCaseSensitive(en, s_keys[i]);
        if (!cJSON_IsString(vz) || !vz->valuestring[0]) {
            printf("  缺 zh-CN 键: %s\n", s_keys[i]);
            dg_fail++;
        }
        if (!cJSON_IsString(ve) || !ve->valuestring[0]) {
            printf("  缺 en-US 键: %s\n", s_keys[i]);
            dg_fail++;
        }
    }

    /* 3. 字体字形覆盖:json 全部键值 + 源码提取键,CJK 字符逐一检查 */
    lv_init();
    for (const cJSON *node = zh->child; node; node = node->next) {
        if (!cJSON_IsString(node) || !node->string)
            continue;
        font_check_text(node->string);
        if (node->valuestring)
            font_check_text(node->valuestring);
    }
    for (int i = 0; i < s_key_cnt; i++)
        font_check_text(s_keys[i]);

    cJSON_Delete(zh);
    cJSON_Delete(en);
    DG_TEST_EXIT();
}
