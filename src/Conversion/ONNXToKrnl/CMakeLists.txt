# SPDX-License-Identifier: Apache-2.0

add_onnx_mlir_library(OMONNXToKrnl
  ONNXToKrnlCommon.cpp
  ControlFlow/Loop.cpp
  ControlFlow/Scan.cpp
  Math/Clip.cpp
  Math/Elementwise.cpp
  Math/Gemm.cpp
  Math/LRN.cpp
  Math/MatMul.cpp
  Math/Reduction.cpp
  Math/Softmax.cpp
  NN/Conv.cpp
  NN/Normalization.cpp
  NN/Pooling.cpp
  RNN/RNNBase.cpp
  RNN/GRU.cpp
  RNN/LSTM.cpp
  RNN/RNN.cpp
  Tensor/ArgMax.cpp
  Tensor/Identity.cpp
  Tensor/Reshape.cpp
  Tensor/Pad.cpp
  Tensor/Transpose.cpp
  Tensor/Squeeze.cpp
  Tensor/Unsqueeze.cpp
  Tensor/Constant.cpp
  Tensor/ConstantOfShape.cpp
  Tensor/Concat.cpp
  Tensor/Split.cpp
  Tensor/Gather.cpp
  Tensor/Shape.cpp
  Tensor/Slice.cpp
  Tensor/Size.cpp
  Tensor/Flatten.cpp
  Tensor/Tile.cpp
  ConvertONNXToKrnl.cpp

  LINK_LIBS PUBLIC
  OMConstPropHelper
  OMSupport
  MLIRMathTransforms
  MLIRStandardOpsTransforms
  )
