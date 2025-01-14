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

#include <cub/cub.cuh>
#include <embedding/operators/generic_lookup.cuh>
#include <embedding/operators/mp_index_calculation.hpp>
#include <utils.cuh>
#include <utils.hpp>

namespace embedding {
void MPLocalReduceIndexCalculation::init(
    std::shared_ptr<CoreResourceManager> core,
    const LocalReduceIndexCalculation& local_reduce_index_calculation,
    const SortKeyAndSrcIdOp& sort_op, const CalDstIds& cal_dst_ids,
    const SegmentdUnique& segmented_unique, const CalDstOffsetMP& cal_dst_offset_mp) {
  core_ = core;
  local_reduce_index_calculation_ = local_reduce_index_calculation;
  sort_op_ = sort_op;
  cal_dst_ids_ = cal_dst_ids;
  segmented_unique_ = segmented_unique;
  cal_dst_offset_mp_ = cal_dst_offset_mp;
}

void MPLocalReduceIndexCalculation::cal_for_sparse_input(const EmbeddingInput& embedding_input,
                                                         ReductionIndices& reduction_indices,
                                                         Wgrad& wgrad, int batch_size) {
  local_reduce_index_calculation_.cal_for_sparse_input(embedding_input, sort_op_, segmented_unique_,
                                                       reduction_indices, wgrad, batch_size);

  auto stream = core_->get_local_gpu()->get_stream();
  if (!wgrad.attr.is_same_ev_size) {
    cal_dst_offset_mp_(wgrad.table_ids, wgrad.attr.table_id_to_ev_size, wgrad.num_unique_keys,
                       wgrad.ev_start_indices, core_, stream);
  }
}

}  // namespace embedding
