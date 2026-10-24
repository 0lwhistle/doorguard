/*
 * i18n.h — 多语言(spec-ui §2):键 = 原文(中文),值 = 译文
 */
#ifndef DG_I18N_H
#define DG_I18N_H

#ifdef __cplusplus
extern "C" {
#endif

/** 加载语言表(路径为 lang 目录,文件名 = <language>.json);
 *  语言取 cfg_get()->language。表缺失时 _() 回退原文(设备仍可用)。 */
int i18n_init(const char *lang_dir);

/** 翻译:查当前语言表;缺键回退原文(静默无日志,C6-4:实现无 WARN 能力;
 *  漏翻由 tests/test_i18n 的键一致性扫描兜住) */
const char *_(const char *key);

/** 切换语言并立即生效:换表 + 异步重建当前页(navigator_reload,
 *  见 i18n.c:弹窗回调里不能同步销毁页面);其余页面导航经过时自然重建 */
int i18n_set_language(const char *lang);

const char *i18n_current_language(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_I18N_H */
