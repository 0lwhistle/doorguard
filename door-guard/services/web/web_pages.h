/*
 * web_pages.h — 内嵌前端资源(由 gen_pages.sh 生成到 web_pages.c)
 *
 * 资源是 frontend/(Vue 工程)的构建产物:web_server 在 "/" 上挂一个通用
 * 静态处理器,按**精确路径**在 DG_WEB_ASSETS 里查找并按其 MIME 返回。
 * 这样以后加资源(图片/字体)只要放进 frontend/public 重新构建,固件代码不用动。
 */
#ifndef DG_WEB_PAGES_H
#define DG_WEB_PAGES_H

#ifdef __cplusplus
extern "C" {
#endif

/** 一条内嵌资源 */
typedef struct {
    const char *path;   /**< 请求路径(以 '/' 开头,如 "/assets/app.js") */
    const char *mime;   /**< Content-Type */
    const char *data;   /**< 内容(文本资源无内嵌 0,二进制同样安全:长度显式给出) */
    unsigned    len;    /**< 字节长度 */
} dg_web_asset_t;

/** 资源表(含首页别名) */
extern const dg_web_asset_t DG_WEB_ASSETS[];
extern const int DG_WEB_ASSET_COUNT;

/** 单页应用入口(web_server 用作未知路径的回退) */
extern const char *const DG_WEB_INDEX_HTML;

#ifdef __cplusplus
}
#endif

#endif /* DG_WEB_PAGES_H */
