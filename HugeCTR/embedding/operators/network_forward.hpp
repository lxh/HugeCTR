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

#include <core23/registry.hpp>
#include <embedding/common.hpp>

namespace embedding {
using core::CoreResourceManager;

struct NetworkIndices {
  std::vector<int> h_network_ids;
  std::vector<int> h_network_gpu_ids;
  std::vector<int> h_network_offsets;
  std::vector<int> h_network_dst_lookup_ids;

  core23::Tensor network_ids;
  core23::Tensor network_gpu_ids;
  core23::Tensor network_offsets;
  core23::Tensor network_dst_lookup_ids;

  void init(std::shared_ptr<CoreResourceManager> core,
            const std::vector<std::vector<int>> &h_global_lookup_ids);
};

struct NetworkBufferAttr {
  std::vector<core23::Tensor> id_to_ev_size_list;
  core23::Tensor id_to_ev_size;

  std::vector<core23::Tensor> id_to_ev_start_indices_list;
  core23::Tensor id_to_ev_start_indices;

  int num_gpus;
  std::vector<int> gpu_id_to_max_ev_elements;

  EmbeddingLayout layout;
  int max_ev_size;
  bool is_ragged;
  bool is_aligned;
  core23::DataType type;
  void init(std::shared_ptr<CoreResourceManager> core, const EmbeddingCollectionParam &ebc_param,
            size_t grouped_id, const std::vector<std::vector<int>> &h_global_lookup_ids);
};

struct NetworkBuffer {
  std::vector<core23::Tensor> data_list;
  core23::Tensor data;

  NetworkBufferAttr attr;

  void init(std::shared_ptr<CoreResourceManager> core, const NetworkBufferAttr &attr,
            int batch_size);
};

class NetworkForward {
  std::shared_ptr<CoreResourceManager> core_;

 public:
  NetworkForward() = default;

  NetworkForward(std::shared_ptr<CoreResourceManager> core);

  void sparse_forward(const core23::Tensor &dp_num_keys_per_bucket,
                      const NetworkBuffer &network_buffer, const NetworkIndices &network_indices,
                      EmbeddingOutput &embedding_output, int batch_size);

  void compute(const core23::Tensor &row_lengths, const core23::Tensor &d_combiner_list,
               const core23::Tensor &network_comm_buffer, const core23::Tensor &network_ids,
               const core23::Tensor &network_gpu_ids, const core23::Tensor &network_offsets,
               const core23::Tensor &network_dst_lookup_ids, const core23::Tensor &network_ev_sizes,
               const core23::Tensor &network_ev_offsets, core23::Tensor &output_buffer,
               const core23::Tensor &d_ev_size_offset, int batch_size, int max_ev_size);
};

}  // namespace embedding
