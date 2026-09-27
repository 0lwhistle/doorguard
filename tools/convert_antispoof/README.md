# convert_antispoof — 反欺骗(MiniFASNet)模型转换与对拍

把 Silent-Face-Anti-Spoofing(小视 MiniFASNet,`door-guard/third_party/` 内 vendored)
的两个 80×80 单帧反欺骗模型转到 RK3576,并逐环节对拍防"转换对了但喂法错了"。

## 模型与规格(权威 = vendored 官方仓库)

| 文件 | 网络 | 取景倍率 scale |
|---|---|---|
| `2.7_80x80_MiniFASNetV2` | MiniFASNetV2 | 2.7 |
| `4_0_0_80x80_MiniFASNetV1SE` | MiniFASNetSE | 4.0 |

- **取景**:检测框(我们的 RetinaFace 框即可)按 scale 扩大 → 中心不变 →
  整体平移夹回图内 → resize 80×80(`generate_patches.CropImage`,C 复刻在
  `services/vision/face_antispoof.c` 的 `antispoof_scale_box`)。不是关键点变换。
- **输入域(头号坑)**:官方 `data_io/functional.py` 的 `to_tensor` 把
  `div(255)` **注释掉了**——输入是 BGR **原域 [0,255]**,没有任何归一化。
  按常规视觉模型喂 [0,1] 会得到看似合理却完全错误的分数(方向性全无)。
- **通道序**:cv2.imread 是 BGR;板上 RGA 产 RGB,须转 BGR 再喂
  (`antispoof_rgb_to_bgr`)。
- **输出**:3 类 softmax([fake, real, other],导出时已烤进模型),
  **label==1 才是真脸**,其余(含 2)一律按假体计(官方 test.py 只认 1)。
- 双模型概率**相加**取 argmax,real 分 = 相加后 [1]/2(展示口径)。

## 流程(WSL,python3.10 + rknn-toolkit2 2.3.2 + torch + opencv + onnxruntime)

```bash
cd tools/convert_antispoof
python3 export_onnx.py   /tmp/spoof_onnx          # pth → onnx(softmax 进图)
python3 check_onnx.py    /tmp/spoof_onnx /tmp/spoof_raw   # torch↔onnx 对拍+导 raw
python3 convert_rknn.py  /tmp/spoof_onnx /tmp/spoof_rknn  # onnx → rk3576 F16
```

产物 `.rknn` 放 `door-guard/models/`(按仓库惯例 rknn 不入 git,
sha256 记在 `door-guard/models/sha256sums.txt`)。板上路径
`/userdata/doorguard/models/`,文件名可用 env
`DG_RKNN_SPOOF_MODEL_A/B` 覆盖。

## 三方对拍基准(板上 `rknn_spoof_test`)

check_onnx.py 会把官方样例图(T1 真 / F1 假)按各模型 scale crop 成
80×80 BGR raw。板上:

```bash
rknn_spoof_test /userdata/doorguard/models/2.7_80x80_MiniFASNetV2.rknn \
    image_T1_2.7_80x80_MiniFASNetV2.raw \
    /userdata/doorguard/models/4_0_0_80x80_MiniFASNetV1SE.rknn \
    image_T1_4_0_0_80x80_MiniFASNetV1SE.raw
```

2026-09-27 实测(板上 NPU F16 ↔ WSL onnxruntime F32):

| 样例 | real 均分(板上) | real 均分(WSL) | 判定 |
|---|---|---|---|
| image_T1(真) | 0.9937 | 0.9936 | REAL ✓ |
| image_F1(假) | 0.0729 | 0.0723 | FAKE ✓ |

偏差 <0.001(F16 量化噪声)。再跑官方原版 `test.py` 交叉验证:
T1=Real 0.99、F1=Fake 0.73(= label2 概率 1.4633/2,与我们的复刻同源)。

## 运行时接入

- `face.antispoof_enable`(默认关)/ `face.antispoof_threshold`(默认 0.50,
  假体标定后调);模型加载失败只降级一次 ERROR,不影响识别主链路。
- 平滑:5 帧中位数(`antispoof_smooth_push`),`FACE_LOST` 清窗。
- 命中发布带 `spoof_challenge` → access FSM 发起多模态二次验证
  (见 spec-auth-business 与 auth_fsm.c 的 challenge_start)。
