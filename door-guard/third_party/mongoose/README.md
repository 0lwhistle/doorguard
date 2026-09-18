# mongoose(vendored)

- **原出处**:https://github.com/cesanta/mongoose,官方 release 包 `mongoose-7.23.zip`
- **版本**:7.23(2026-08-12)
- **取用内容**:仅顶层 amalgamated 单文件 `mongoose.c` / `mongoose.h` + `LICENSE`,
  原包中的 test/tutorials/resources/CI 等目录不进库
- **改动点**:无(原样 vendored;后续如打补丁,在此处记录)
- **许可**:**GPLv2 / 商业双许可**(Cesanta)。以 GPL 路径使用需遵守 GPLv2 义务;
  如产品闭源商用,需向 Cesanta 购买商业授权——接入正式功能前先确认许可路线
- **当前用途**:备用。现有 web 服务用 civetweb(见 `modules/net/`);
  mongoose 作为候选替换/新增网络功能时再接入,接入时同步 CMake 与本文件
- **使用示例**(最小 HTTP 服务,详见官方 docs):

```c
#include "mongoose.h"

static void fn(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = ev_data;
        mg_http_reply(c, 200, "", "hello\n");   // 实际业务在此路由
    }
}

int main(void) {
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_http_listen(&mgr, "http://0.0.0.0:8080", fn, NULL);
    for (;;) mg_mgr_poll(&mgr, 100);            // 事件循环,吃自己线程
    mg_mgr_free(&mgr);
}
```
