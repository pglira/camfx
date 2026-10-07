#!/usr/bin/env python3
"""Append softmax and class reduction to the converted selfie_multiclass ONNX model.

The output becomes [1, 256, 256, 2] with channel 0 = p(person) = 1 - p(background)
and channel 1 = p(skin) = p(body-skin) + p(face-skin), so no per-pixel work is left
for the CPU.

Usage: add_postprocess.py <input.onnx> <output.onnx>
"""
import sys

import numpy as np
import onnx
from onnx import helper, numpy_helper, TensorProto


def main(src: str, dst: str) -> None:
    model = onnx.load(src)
    graph = model.graph
    logits = graph.output[0].name

    # Class weights: person = 1 - bg  -> weights [-1,0,0,0,0,0] plus bias 1
    #                skin   = body + face -> weights [0,0,1,1,0,0]
    w = np.array([[-1, 0], [0, 0], [0, 1], [0, 1], [0, 0], [0, 0]], dtype=np.float32)
    b = np.array([1, 0], dtype=np.float32)
    graph.initializer.extend([
        numpy_helper.from_array(w, "pp_weights"),
        numpy_helper.from_array(b, "pp_bias"),
    ])
    graph.node.extend([
        helper.make_node("Softmax", [logits], ["pp_probs"], axis=-1),
        helper.make_node("MatMul", ["pp_probs", "pp_weights"], ["pp_mixed"]),
        helper.make_node("Add", ["pp_mixed", "pp_bias"], ["masks"]),
    ])
    del graph.output[:]
    graph.output.append(helper.make_tensor_value_info("masks", TensorProto.FLOAT, [1, 256, 256, 2]))
    onnx.checker.check_model(model)
    onnx.save(model, dst)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
