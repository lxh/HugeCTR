/*
 * Copyright (c) 2023, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <optimizer.hpp>

namespace HugeCTR {

/**
 * Adam optimizer
 */
template <typename T>
class SGDOptimizer : public Optimizer {
 public:
  /**
   * Constructor of SGDOptimizer.
   * names of hyper-parameters are the same as in Algorithm 1 of Adam paper (arXiv:1412.6980)
   * @param weight_main weights to be updated
   * @param wgrad gradient for weights
   * @param device_id the id of GPU where update kernel is launched
   * @param lr learning rate
   # @param scaler scaler factor for mixed precision
   */
  SGDOptimizer(const Tensor2<float>& weight_main, const Tensor2<__half>& weight_main_half,
               const Tensor2<T>& wgrad, const std::shared_ptr<GPUResource>& gpu_resource,
               float lr = 0.001f, float scaler = 1.f, bool use_mixed_precision = false);
  /**
   * Constructor of SGDOptimizer.
   * names of hyper-parameters are the same as in Algorithm 1 of Adam paper (arXiv:1412.6980)
   * @param weight_tensors  a list of dense layer weight tensors
   * @param wgrad gradient for weight tensors
   * @param gpu_resource the GPU where update kernel is launched
   * @param lr learning rate
   # @param scaler scaler factor for mixed precision
   */
  SGDOptimizer(std::optional<WeightTensors> weight_tensors,
               std::optional<WeightHalfTensors> weight_half_tensors,
               std::optional<WgradTensors<T>> wgrad_tensors,
               const std::shared_ptr<GPUResource>& gpu_resource, float lr = 0.001f,
               float scaler = 1.f, bool use_mixed_precision = false);

  /**
   * update the weights using gradient
   * @param stream cuda stream used by update kernel
   */
  void update() override;

 private:
  Tensor2<T> wgrad_;
  Tensor2<__half> weight_main_half_;

  std::optional<WgradTensors<T>> wgrad_tensors_;
  std::optional<WeightHalfTensors> weight_half_tensors_;
  bool use_mixed_precision_;
};

}  // namespace HugeCTR
