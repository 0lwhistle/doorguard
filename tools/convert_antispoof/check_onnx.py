#!/usr/bin/env python3
"""官方 torch pipeline vs ONNX 导出 对拍 + 样例图真/假方向校验 + crop raw 导出。

完整复刻官方 test.py 链路(caffemodel bbox → _get_new_box×scale → resize 80×80
→ BGR[0,1] → 双模型 softmax 平均),与 ONNX 导出(除 255+softmax 已进图)对拍:
两路 3 类概率差应 <1e-4;真脸样例(T*)label=1、假脸样例(F*)label=0。

同时把两张样例图的 crop 结果导出为 BGR[0,255] uint8 raw(80×80×3),
供板上 rknn_spoof_test 三方对拍(torch/onnx ↔ 板上 rknn)。

用法: python3 check_onnx.py <onnx目录> <raw输出目录>
"""
import math
import os
import sys

import cv2
import numpy as np
import onnxruntime as ort
import torch
import torch.nn.functional as F

_REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "..", "..", "door-guard", "third_party",
                     "Silent-Face-Anti-Spoofing-master")
sys.path.insert(0, _REPO)

from src.model_lib.MiniFASNet import MiniFASNetV2, MiniFASNetV1SE  # noqa: E402
from export_onnx import MODELS, load_core  # noqa: E402

SAMPLES = [  # (文件名, 期望是否真脸):T=真 F=假(官方 images/sample 命名)。
    # 官方语义:label==1 → Real,**其余(含 2)一律 Fake**(test.py 只认 1)
    ("image_T1.jpg", True),
    ("image_F1.jpg", False),
]


def get_bbox(img):
    """官方 anti_spoof_predict.Detection.get_bbox:caffemodel 单目标框"""
    height, width = img.shape[0], img.shape[1]
    aspect_ratio = width / height
    if img.shape[1] * img.shape[0] >= 192 * 192:
        img = cv2.resize(img, (int(192 * math.sqrt(aspect_ratio)),
                               int(192 / math.sqrt(aspect_ratio))),
                         interpolation=cv2.INTER_LINEAR)
    blob = cv2.dnn.blobFromImage(img, 1, mean=(104, 117, 123))
    net = cv2.dnn.readNetFromCaffe(
        os.path.join(_REPO, "resources/detection_model/deploy.prototxt"),
        os.path.join(_REPO, "resources/detection_model/Widerface-RetinaFace.caffemodel"))
    net.setInput(blob, "data")
    out = net.forward("detection_out").squeeze()
    i = int(np.argmax(out[:, 2]))
    left, top = out[i, 3] * width, out[i, 4] * height
    right, bottom = out[i, 5] * width, out[i, 6] * height
    return [int(left), int(top), int(right - left + 1), int(bottom - top + 1)]


def get_new_box(src_w, src_h, bbox, scale):
    """官方 generate_patches.CropImage._get_new_box:框×scale 后整体平移夹回图内"""
    x, y, box_w, box_h = bbox
    scale = min((src_h - 1) / box_h, min((src_w - 1) / box_w, scale))
    new_w, new_h = box_w * scale, box_h * scale
    cx, cy = box_w / 2 + x, box_h / 2 + y
    ltx, lty = cx - new_w / 2, cy - new_h / 2
    rbx, rby = cx + new_w / 2, cy + new_h / 2
    if ltx < 0:
        rbx -= ltx
        ltx = 0
    if lty < 0:
        rby -= lty
        lty = 0
    if rbx > src_w - 1:
        ltx -= rbx - src_w + 1
        rbx = src_w - 1
    if rby > src_h - 1:
        lty -= rby - src_h + 1
        rby = src_h - 1
    return int(ltx), int(lty), int(rbx), int(rby)


def crop(org, bbox, scale, out=80):
    """官方 CropImage.crop(crop=True 分支):BGR 原域 uint8"""
    ltx, lty, rbx, rby = get_new_box(org.shape[1], org.shape[0], bbox, scale)
    patch = org[lty:rby + 1, ltx:rbx + 1]
    return cv2.resize(patch, (out, out))


def parse_scale(name):
    """官方 utility.parse_model_name 的 scale 位:'2.7_...' → 2.7,'4_0_0_...' → 4.0"""
    return float(name.split("_")[0:-1][0])


def main():
    onnx_dir, raw_dir = sys.argv[1], sys.argv[2]
    os.makedirs(raw_dir, exist_ok=True)
    sess = {name: ort.InferenceSession(os.path.join(onnx_dir, name.replace(".pth", ".onnx")))
            for name, _ in MODELS}
    torch_models = {name: load_core(cls, os.path.join(_REPO, "resources",
                                                      "anti_spoof_models", name))
                    for name, cls in MODELS}

    ok_all = True
    for img_name, want_label in SAMPLES:
        img = cv2.imread(os.path.join(_REPO, "images/sample", img_name))
        assert img is not None, img_name
        bbox = get_bbox(img)

        pred_t = np.zeros((3,), np.float32)
        pred_o = np.zeros((3,), np.float32)
        for name, _ in MODELS:
            patch = crop(img, bbox, parse_scale(name))
            x = patch.astype(np.float32)                  # BGR 原域 [0,255](不除 255!)
            t = torch.tensor(x.transpose(2, 0, 1))[None]
            with torch.no_grad():
                pred_t += F.softmax(torch_models[name](t), dim=1).numpy()[0]
            pred_o += sess[name].run(
                None, {"input": patch.astype(np.float32).transpose(2, 0, 1)[None]})[0][0]

            raw_path = os.path.join(raw_dir, f"{os.path.splitext(img_name)[0]}_"
                                            f"{name.replace('.pth', '')}.raw")
            patch.tofile(raw_path)                        # BGR uint8 80*80*3

        diff = float(np.abs(pred_t - pred_o).max())
        label = int(np.argmax(pred_o))
        real = pred_o[1] / 2.0                            # 官方展示口径:score/2
        is_real = (label == 1)
        tag = "OK" if is_real == want_label and diff < 1e-3 else "FAIL"
        ok_all &= (tag == "OK")
        print(f"{img_name}: bbox={bbox} label={label}(期望{'real' if want_label else 'fake'}) "
              f"real={real:.4f} torch|onnx 最大差={diff:.2e} [{tag}]")
        print(f"  onnx 3类 = {np.round(pred_o, 5).tolist()}")
    print("ALL", "PASS" if ok_all else "FAIL")
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
