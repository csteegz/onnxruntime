# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

if(NOT NUMPY_INCLUDE_DIR)
  include(onnxruntime_python.cmake)
endif(NOT NUMPY_INCLUDE_DIR)
include_directories(${PYTHON_INCLUDE_DIR})
include_directories(${NUMPY_INCLUDE_DIR})
file(GLOB onnxruntime_pyop_srcs "${ONNXRUNTIME_ROOT}/core/external_ops/pyop.cc")
add_library(onnxruntime_pyop SHARED ${onnxruntime_pyop_srcs})
set_target_properties(onnxruntime_pyop PROPERTIES LINK_FLAGS "/ignore:4199")
target_link_libraries(onnxruntime_pyop PUBLIC ${PYTHON_LIBRARIES})