# enroll — 用户/特征录入编排服务

用户生命周期与生物特征录入的唯一编排层:两段式人脸草稿(设备拍摄)、
静态图直落(web 上传)、指纹/IC 编排转发、用户 CRUD 收口。

## 职责与链路

```
设备拍摄:UI page_capture → EV_ENROLL_REQUEST{FACE}
          → EV_VISION_CAPTURE_REQ → 后端(板上 rknn worker / sim mock)
          → EV_VISION_FEATURE{seq} → 草稿槽(不落库)→ EV_ENROLL_RESULT{OK=草稿就绪}
          UI「保存」= commit_draft(查重+DB+特征库+头像);「放弃」= discard_draft

web 上传:web_server /api/users/face_set(JPEG body)
          → enroll_service_face_upload(单飞闸+静态图槽+EV_VISION_STILL_REQ)
          → 后端 still_enroll 提取 → 成功 EV_VISION_FEATURE(直落库,不经草稿)
                                    → 失败 EV_VISION_STILL_FAIL
          两路都按 seq 回执 EV_ENROLL_RESULT → web WS enroll 消息

指纹/IC:  EV_ENROLL_REQUEST{FINGER/FINGER_DEL/IC/...} → EV_FINGER_SET_MODE /
          EV_ICCARD_CTRL;绑定判定与落库在 on_ic_card;模板生命周期归 fp_provider
```

## 落库公共尾 `face_commit()`

设备拍摄 commit 与 web 上传直落**共用同一落库尾**,保证两条路径语义一致:
查重(storage 比较器)→ DB 覆写 → 内存特征库 INSERT → 头像落库。
失败反向还原(library_add 失败按 existing 回写 DB,不留"DB 有特征内存没有"
的半状态)。**方式位**:落库成功自动置位 `DG_AUTH_FACE`、`clear_face` 自动
清位(与指纹"删光清位"/IC"解绑清位"对齐,不变式见 spec-database §1);
`ic_card` 必须回填(db_user_update 对它是"始终覆盖"语义,不回填会悄悄解绑卡)。

## 线程契约

- 草稿槽/单飞闸有锁;`draft_avatar` 返回内部缓冲指针,依赖「UI 单线程 +
  采集与提交不同时在飞」使用约定(见 enroll_service.h)
- 总线回调(总线分发线程)内不碰 LVGL、不跑长事务;静态图/特征大缓冲
  一律 static(总线线程栈仅 64KB)

## 测试

- `tests/test_enroll_flow.c`:两段式草稿全生命周期 + commit 失败还原 +
  web 上传端到端(受理→单飞→直落→回执)+ user_save/save_ex 语义
- `tests/test_storage.c` [S11]:方式位不变式(add/update 拒收/清位/迁移规范化)
- `tests/web/api_test.sh`:`/api/users/face_set` 受理制链路(板上或宿主 sim)
