#!/usr/bin/env python3
"""MiniFASNet(.pth)→ ONNX 导出(反欺骗双模型)。

规格依据 third_party/Silent-Face-Anti-Spoofing-master(权威):
  - 输入 80×80,crop = 检测框 ×scale(2.7 / 4.0)中心裁剪缩放,see check_onnx.py;
  - 官方推理 cv2.imread(BGR)→ 自带 to_tensor(**原域 [0,255],不除 255——
    functional.py 中 div(255) 被官方注释掉,这是与常规视觉模型最大的差异**)
    → softmax(3 类),label 1 = 真脸(test.py: label==1 → RealFace);
  - 本导出只把 softmax(dim=1) 烤进 ONNX:板上 C 侧直接喂 uint8 BGR 原域
    (rknn config mean=0/std=1),输出即 3 类概率。

用法: python3 export_onnx.py <输出目录>
"""
import collections
import os
import sys

import torch

_REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "..", "..", "door-guard", "third_party",
                     "Silent-Face-Anti-Spoofing-master")
sys.path.insert(0, _REPO)

from src.model_lib.MiniFASNet import MiniFASNetV2, MiniFASNetV1SE  # noqa: E402

# (权重文件名, 网络类):文件名同时是官方 scale/尺寸的编码(utility.parse_model_name)
MODELS = [
    ("2.7_80x80_MiniFASNetV2.pth", MiniFASNetV2),
    ("4_0_0_80x80_MiniFASNetV1SE.pth", MiniFASNetV1SE),
]


def get_kernel(height, width):
    """官方 utility.get_kernel:conv6 的核尺寸随输入边长变化"""
    return ((height + 15) // 16, (width + 15) // 16)


class Wrap(torch.nn.Module):
    """见模块注释:只加 softmax(dim=1),输入保持 BGR [0,255] 原域"""

    def __init__(self, core):
        super().__init__()
        self.core = core

    def forward(self, x):                       # x: NCHW float BGR [0,255]
        return torch.softmax(self.core(x), dim=1)


def load_core(cls, path):
    m = cls(conv6_kernel=get_kernel(80, 80))
    sd = torch.load(path, map_location="cpu")
    # 官方 _load_model:DataParallel 训练权重带 'module.' 前缀,剥掉
    if any(k.startswith("module.") for k in sd.keys()):
        sd = collections.OrderedDict((k[7:], v) for k, v in sd.items())
    m.load_state_dict(sd)
    m.eval()
    return m


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out_dir, exist_ok=True)
    dummy = torch.randn(1, 3, 80, 80)
    for name, cls in MODELS:
        core = load_core(cls, os.path.join(_REPO, "resources", "anti_spoof_models", name))
        onnx_path = os.path.join(out_dir, name.replace(".pth", ".onnx"))
        torch.onnx.export(Wrap(core), dummy, onnx_path, opset_version=11,
                          input_names=["input"], output_names=["prob"],
                          do_constant_folding=True)
        print("exported:", onnx_path)


if __name__ == "__main__":
    main()
