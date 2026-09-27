#!/usr/bin/env python3
"""ONNX → RK3576 rknn(F16,不做 int8 量化)。

为什么 F16:模型仅 ~2MB,板上预计个位数 ms,量化收益小;而 int8 标定需要
真/假人脸 crop 标定集,当前没有——留作后续优化(届时在本目录补标定集生成)。
输入为 BGR 原域 [0,255](官方 to_tensor 不做 /255,见 export_onnx.py 文档),
板上喂 uint8 BGR 即可,config 不设归一化。

用法: python3 convert_rknn.py <onnx目录> <rknn输出目录>
"""
import os
import sys

from rknn.api import RKNN

MODELS = [
    "2.7_80x80_MiniFASNetV2.onnx",
    "4_0_0_80x80_MiniFASNetV1SE.onnx",
]


def main():
    onnx_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    for name in MODELS:
        rknn = RKNN(verbose=False)
        rknn.config(mean_values=[[0, 0, 0]], std_values=[[1, 1, 1]],
                    target_platform="rk3576")
        if rknn.load_onnx(os.path.join(onnx_dir, name)) != 0:
            sys.exit(f"load_onnx 失败: {name}")
        if rknn.build(do_quantization=False) != 0:
            sys.exit(f"build 失败: {name}")
        out = os.path.join(out_dir, name.replace(".onnx", ".rknn"))
        if rknn.export_rknn(out) != 0:
            sys.exit(f"export 失败: {name}")
        rknn.release()
        print("converted:", out)


if __name__ == "__main__":
    main()
