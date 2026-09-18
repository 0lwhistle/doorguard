/*
 * web_pages.h — 内嵌页面资源(HTML / CSS / JS)
 *
 * 内容由 gen_pages.sh 从 pages/ 生成到 web_pages.c;三个常量分别由
 * web_server 的 /、/app.css、/app.js 三个路由提供(拆开是为了让浏览器
 * 缓存样式与脚本,也便于前端单独改)。
 */
#ifndef DG_WEB_PAGES_H
#define DG_WEB_PAGES_H

extern const char *const DG_WEB_INDEX_HTML;
extern const char *const DG_WEB_APP_CSS;
extern const char *const DG_WEB_APP_JS;

#endif /* DG_WEB_PAGES_H */
