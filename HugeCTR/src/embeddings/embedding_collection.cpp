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

#include <embeddings/embedding_collection.hpp>

#include "embedding/hier_model_parallel_embedding.hpp"
#include "embedding/model_parallel_embedding.hpp"

namespace embedding {

EmbeddingCollection::EmbeddingCollection(
    std::shared_ptr<HugeCTR::ResourceManager> resource_manager,
    std::vector<std::shared_ptr<CoreResourceManager>> core,
    const EmbeddingCollectionParam &ebc_param, const EmbeddingCollectionParam &eval_ebc_param,
    const std::vector<EmbeddingTableParam> &emb_table_param_list,
    std::shared_ptr<HugeCTR::ExchangeWgrad> exchange_wgrad)
    : resource_manager_(resource_manager),
      ebc_param_(ebc_param),
      eval_ebc_param_(eval_ebc_param),
      emb_table_param_list_(emb_table_param_list) {
  for (size_t i = 0; i < emb_table_param_list.size(); ++i) {
    embedding_optimizers_.push_back(emb_table_param_list[i].opt_param);
  }

  int num_gpus = resource_manager->get_local_gpu_count();

  for (int gpu_id = 0; gpu_id < num_gpus; ++gpu_id) {
    HugeCTR::CudaDeviceContext context(core[gpu_id]->get_device_id());

    embedding_tables_.push_back(create_grouped_embedding_tables(resource_manager, core[gpu_id],
                                                                ebc_param_, emb_table_param_list));
    embeddings_.push_back(create_grouped_embeddings(core[gpu_id], ebc_param_));
    eval_embeddings_.push_back(create_grouped_embeddings(core[gpu_id], eval_ebc_param_));
  }

  init_embedding_output_attrs(core);
  init_wgrad(core, exchange_wgrad);
  init_peer_buffer(core);
}

void EmbeddingCollection::init_embedding_output_attrs(
    std::vector<std::shared_ptr<CoreResourceManager>> core) {
  int num_gpus = resource_manager_->get_local_gpu_count();
  embedding_output_attrs.resize(num_gpus);

  for (int gpu_id = 0; gpu_id < num_gpus; ++gpu_id) {
    int num_grouped = static_cast<int>(ebc_param_.grouped_lookup_params.size());

    embedding_output_attrs[gpu_id].resize(num_grouped);
    for (size_t grouped_id = 0; grouped_id < num_grouped; ++grouped_id) {
      embedding_output_attrs[gpu_id][grouped_id].init(core[gpu_id], ebc_param_);
      embedding_output_attrs[gpu_id][grouped_id].update_mutable_data(core[gpu_id], ebc_param_);
    }
  }
}

void EmbeddingCollection::init_wgrad(std::vector<std::shared_ptr<CoreResourceManager>> core,
                                     std::shared_ptr<HugeCTR::ExchangeWgrad> exchange_wgrad) {
  int num_gpus = resource_manager_->get_local_gpu_count();

  wgrad_list_.resize(num_gpus);
  wgrad_tensor2_float_list_.resize(num_gpus);
  wgrad_tensor2_half_list_.resize(num_gpus);
  for (int gpu_id = 0; gpu_id < num_gpus; ++gpu_id) {
    HugeCTR::CudaDeviceContext context(core[gpu_id]->get_device_id());
    int num_grouped = static_cast<int>(ebc_param_.grouped_lookup_params.size());
    wgrad_list_[gpu_id].resize(num_grouped);
    for (size_t grouped_id = 0; grouped_id < num_grouped; ++grouped_id) {
      Wgrad &wgrad = wgrad_list_[gpu_id][grouped_id];
      auto current_tps = ebc_param_.grouped_lookup_params[grouped_id].table_placement_strategy;
      // 1. mp or sparse allreduce
      if (ebc_param_.allreduce_strategy_ == AllreduceStrategy::Sparse ||
          current_tps == TablePlacementStrategy::ModelParallel) {
        WgradInitializer{core[gpu_id], ebc_param_, grouped_id,
                         embeddings_[gpu_id][grouped_id]->get_wgrad_attr()}
            .init(wgrad)
            .init_indices()
            .init_data();
        continue;
      }
      // 2. init table_id_to_vocabulary_size and check if there is dynamic table
      std::vector<int> table_id_to_vocabulary_size;
      std::transform(emb_table_param_list_.begin(), emb_table_param_list_.end(),
                     std::back_inserter(table_id_to_vocabulary_size),
                     [](const embedding::EmbeddingTableParam &table_param) {
                       return table_param.max_vocabulary_size;
                     });

      std::for_each(table_id_to_vocabulary_size.begin(), table_id_to_vocabulary_size.end(),
                    [](int vocabulary_size) {
                      HCTR_CHECK_HINT(vocabulary_size > 0, "vocabuary_size should > 0.");
                    });

      int use_core23 = 1;
      if (getenv("HUGECTR_CORE23_NETWORK")) {
        use_core23 = atoi(getenv("HUGECTR_CORE23_NETWORK"));
      }
      if (use_core23) {
        // 2. dense allreduce can be group or not grouped
        bool grouped = (ebc_param_.allreduce_strategy_ == AllreduceStrategy::GroupDense);
        if (ebc_param_.wgrad_type_.type() == core23::ScalarType::Float) {
          AllreduceWgradInitializer{core[gpu_id], ebc_param_, table_id_to_vocabulary_size,
                                    grouped_id, embeddings_[gpu_id][grouped_id]->get_wgrad_attr()}
              .init(wgrad)
              .init_indices()
              .init_data(grouped, HugeCTR::GetWgradBufferChannel());

        } else if (ebc_param_.wgrad_type_.type() == core23::ScalarType::Half) {
          AllreduceWgradInitializer{core[gpu_id], ebc_param_, table_id_to_vocabulary_size,
                                    grouped_id, embeddings_[gpu_id][grouped_id]->get_wgrad_attr()}
              .init(wgrad)
              .init_indices()
              .init_data(grouped, HugeCTR::GetWgradHalfBufferChannel());

        } else {
          HCTR_OWN_THROW(HugeCTR::Error_t::WrongInput,
                         "Embedding wgrad type set wrong can't support!");
        }
      } else {
        // 2. not grouped dense allreduce
        bool not_grouped = (ebc_param_.allreduce_strategy_ == AllreduceStrategy::Dense);
        AllreduceWgradInitializer{core[gpu_id], ebc_param_, table_id_to_vocabulary_size, grouped_id,
                                  embeddings_[gpu_id][grouped_id]->get_wgrad_attr()}
            .init(wgrad)
            .init_indices()
            .init_data(not_grouped);
        if (not_grouped) continue;

        // 3. grouped dense allreduce
        HCTR_CHECK_HINT(exchange_wgrad != nullptr,
                        "if you want to group allreduce , you need send a exchange_wgrad into "
                        "embedding_collection");

        if (wgrad.attr.type.match<float>()) {
          auto grouped_wgrad_buff =
              std::dynamic_pointer_cast<HugeCTR::GroupedExchangeWgrad<float>>(exchange_wgrad)
                  ->get_embed_wgrad_buffs()[gpu_id];
          grouped_wgrad_buff->reserve({static_cast<size_t>(wgrad.max_buffer_size)},
                                      &(wgrad_tensor2_float_list_[gpu_id]));
          if (gpu_id == 0) grouped_allreduce_length_ = wgrad.max_buffer_size * sizeof(float);
        } else if (wgrad.attr.type.match<__half>()) {
          auto grouped_wgrad_buff =
              std::dynamic_pointer_cast<HugeCTR::GroupedExchangeWgrad<__half>>(exchange_wgrad)
                  ->get_embed_wgrad_buffs()[gpu_id];
          grouped_wgrad_buff->reserve({static_cast<size_t>(wgrad.max_buffer_size)},
                                      &(wgrad_tensor2_half_list_[gpu_id]));
          if (gpu_id == 0) grouped_allreduce_length_ = wgrad.max_buffer_size * sizeof(__half);
        } else {
          HCTR_OWN_THROW(HugeCTR::Error_t::WrongInput,
                         "have a wrong wgrad type, wgrad type need be float or __half");
        }
      }
    }
  }
}

void EmbeddingCollection::init_peer_buffer(std::vector<std::shared_ptr<CoreResourceManager>> core) {
  // collective init peer buffer
  if (ebc_param_.comm_strategy_ != CommunicationStrategy::Hierarchical) return;
  HCTR_CHECK(resource_manager_->all_p2p_enabled());
  int num_gpus = resource_manager_->get_local_gpu_count();

  gpu_barrier_ = std::make_unique<HugeCTR::GPUBarrier>(
      resource_manager_->get_local_gpu_count(), resource_manager_->get_local_gpu_device_id_list());

  auto init_hierarchical_embedding =
      [&](std::vector<std::vector<std::unique_ptr<IGroupedEmbeddingOp>>> &embeddings,
          size_t grouped_id) {
        std::vector<ModelCommBuffer *> model_comm_buffers;
        std::vector<IntraModelReductionBuffer *> intra_reduction_buffers;
        std::vector<IntraModelCommBuffer *> intra_model_comm_buffers;

        for (int gpu_id = 0; gpu_id < num_gpus; ++gpu_id) {
          HugeCTR::CudaDeviceContext context(core[gpu_id]->get_device_id());
          auto embedding =
              dynamic_cast<HierModelParallelEmbedding *>(embeddings[gpu_id][grouped_id].get());
          intra_reduction_buffers.push_back(embedding->get_intra_reduction_buffer());
          intra_model_comm_buffers.push_back(embedding->get_intra_model_comm_buffer());

          embedding->set_gpu_barrier(gpu_barrier_.get());
        }
        collective_init_peer_buffer(core, intra_reduction_buffers);
        collective_init_peer_buffer(core, intra_model_comm_buffers);
      };

  for (size_t grouped_id = 0; grouped_id < ebc_param_.grouped_lookup_params.size(); ++grouped_id) {
    if (ebc_param_.grouped_lookup_params[grouped_id].table_placement_strategy ==
        TablePlacementStrategy::ModelParallel) {
      init_hierarchical_embedding(embeddings_, grouped_id);
      init_hierarchical_embedding(eval_embeddings_, grouped_id);
    }
  }
}

void EmbeddingCollection::cache_ddl_output(int gpu_id,
                                           const HugeCTR::DataDistributor::Result &input,
                                           HugeCTR::DataDistributor::Result &output,
                                           int batch_size) {
  HugeCTR::CudaDeviceContext context(resource_manager_->get_local_gpu(gpu_id)->get_device_id());
  auto stream = resource_manager_->get_local_gpu(gpu_id)->get_stream();
  HCTR_CHECK(output.size() == input.size());

  for (size_t grouped_id = 0; grouped_id < input.size(); ++grouped_id) {
    auto &dst_result = output[grouped_id];
    auto &src_result = input[grouped_id];

    HCTR_LIB_THROW(cudaMemcpyAsync(dst_result.keys.data(), src_result.keys.data(),
                                   src_result.keys.data_type().size() * src_result.h_num_keys,
                                   cudaMemcpyDeviceToDevice, stream));
    core23::copy_sync(dst_result.num_keys, src_result.num_keys);
    core23::copy_async(dst_result.bucket_range, src_result.bucket_range, stream);
    core23::copy_async(dst_result.num_keys_per_bucket, src_result.num_keys_per_bucket, stream);
    dst_result.h_num_keys = src_result.h_num_keys;
  }
}

void EmbeddingCollection::forward_per_gpu(Stage stage, bool is_train, int gpu_id,
                                          const HugeCTR::DataDistributor::Result &input,
                                          core23::Tensor &output_buffer, int batch_size) {
  auto &embeddings = is_train ? embeddings_[gpu_id] : eval_embeddings_[gpu_id];

  for (size_t grouped_id = 0; grouped_id < embeddings.size(); ++grouped_id) {
    if (!embeddings[grouped_id]->is_valid_stage(stage)) continue;

    ILookup *lookup = dynamic_cast<ILookup *>(get_table(gpu_id, grouped_id));
    EmbeddingOutput embedding_output{output_buffer, embedding_output_attrs[gpu_id][grouped_id]};

    embeddings[grouped_id]->forward_per_gpu(stage, input[grouped_id], lookup, embedding_output,
                                            batch_size);
  }
}

void EmbeddingCollection::forward_per_gpu(bool is_train, int gpu_id,
                                          const HugeCTR::DataDistributor::Result &input,
                                          core23::Tensor &output_buffer, int batch_size) {
  std::vector<Stage> stages{Stage::DPForward};
  if (ebc_param_.comm_strategy_ == CommunicationStrategy::Uniform) {
    stages.insert(stages.end(), {Stage::MPModelForward, Stage::MPNetworkdForward});
  } else if (ebc_param_.comm_strategy_ == CommunicationStrategy::Hierarchical) {
    stages.insert(stages.end(), {Stage::HierMPModelForward, Stage::HierMPNetworkForward});
  } else {
    HCTR_OWN_THROW(HugeCTR::Error_t::IllegalCall, "comm strategy not supported in forward_per_gpu");
  }

  for (auto stage : stages) {
    forward_per_gpu(stage, is_train, gpu_id, input, output_buffer, batch_size);
  }
}

void EmbeddingCollection::backward_per_gpu(Stage stage, int gpu_id,
                                           const HugeCTR::DataDistributor::Result &input,
                                           const core23::Tensor &top_grad, int batch_size) {
  for (size_t grouped_id = 0; grouped_id < embeddings_[gpu_id].size(); ++grouped_id) {
    if (!embeddings_[gpu_id][grouped_id]->is_valid_stage(stage)) continue;

    EmbeddingOutput top_grad_buffer{top_grad, embedding_output_attrs[gpu_id][grouped_id]};
    embeddings_[gpu_id][grouped_id]->backward_per_gpu(stage, input[grouped_id], top_grad_buffer,
                                                      wgrad_list_[gpu_id][grouped_id], batch_size);
  }
}

void EmbeddingCollection::backward_per_gpu(int gpu_id,
                                           const HugeCTR::DataDistributor::Result &input,
                                           const core23::Tensor &top_grad, int batch_size) {
  std::vector<Stage> stages{Stage::DPBackwardIndexCalculation, Stage::DPLocalReduce,
                            Stage::DPAllreduce};
  if (ebc_param_.comm_strategy_ == CommunicationStrategy::Uniform) {
    stages.insert(stages.end(), {Stage::MPBackwardIndexCalculation, Stage::MPNetworkBackward,
                                 Stage::MPLocalReduce});
  } else if (ebc_param_.comm_strategy_ == CommunicationStrategy::Hierarchical) {
    stages.insert(stages.end(), {Stage::HierMPBackwardIndexCalculation,
                                 Stage::HierMPNetworkBackward, Stage::HierMPLocalReduce});
  } else {
    HCTR_OWN_THROW(HugeCTR::Error_t::IllegalCall,
                   "comm strategy not supported in backward_per_gpu");
  }

  for (auto stage : stages) {
    backward_per_gpu(stage, gpu_id, input, top_grad, batch_size);
  }
}

void EmbeddingCollection::update_per_gpu(int gpu_id, embedding::TablePlacementStrategy tps) {
  for (size_t grouped_id = 0; grouped_id < embeddings_[gpu_id].size(); ++grouped_id) {
    if (ebc_param_.grouped_lookup_params[grouped_id].table_placement_strategy != tps) continue;
    auto &wgrad = wgrad_list_[gpu_id][grouped_id];

    auto table = get_table(gpu_id, grouped_id);
    table->update(wgrad.unique_keys, wgrad.num_unique_keys, wgrad.table_ids, wgrad.ev_start_indices,
                  wgrad.data);
  }
}

void EmbeddingCollection::update_per_gpu(int gpu_id) {
  for (auto tps : {embedding::TablePlacementStrategy::DataParallel,
                   embedding::TablePlacementStrategy::ModelParallel}) {
    update_per_gpu(gpu_id, tps);
  }
}

void EmbeddingCollection::set_learning_rate(float lr) {
  for (auto &table_list : embedding_tables_) {
    for (auto &t : table_list) {
      t->set_learning_rate(lr);
    }
  }
  for (auto &cache : frequent_embedding_tables_) {
    cache->set_learning_rate(lr);
  }
}

void EmbeddingCollection::bind_grouped_wgrad_ptr() {
  if (ebc_param_.allreduce_strategy_ != AllreduceStrategy::GroupDense) return;
  int num_gpus = static_cast<int>(wgrad_list_.size());
  for (size_t grouped_id = 0; grouped_id < ebc_param_.grouped_lookup_params.size(); ++grouped_id) {
    if (ebc_param_.grouped_lookup_params[grouped_id].table_placement_strategy !=
        TablePlacementStrategy::DataParallel) {
      continue;
    }
    for (int gpu_id = 0; gpu_id < num_gpus; ++gpu_id) {
      auto &wgrad = wgrad_list_[gpu_id][grouped_id];
      if (wgrad.attr.type.match<float>()) {
        wgrad.bind_data_ptr(wgrad_tensor2_float_list_[gpu_id].get_ptr());
      } else if (wgrad.attr.type.match<__half>()) {
        wgrad.bind_data_ptr(wgrad_tensor2_half_list_[gpu_id].get_ptr());
      } else {
        HCTR_OWN_THROW(HugeCTR::Error_t::WrongInput,
                       "have a wrong wgrad type, wgrad type need be float or __half");
      }
    }
  }
}

}  // namespace embedding
