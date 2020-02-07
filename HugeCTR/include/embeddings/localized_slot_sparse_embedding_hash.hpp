/*
 * Copyright (c) 2019, NVIDIA CORPORATION.
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
#include "HugeCTR/include/common.hpp"
#include "HugeCTR/include/embedding.hpp"
#include "cub/cub/device/device_radix_sort.cuh"
#include "HugeCTR/include/embeddings/sparse_embedding_hash_functors.h"

#include <vector>

#ifdef ENABLE_MPI
#include <mpi.h>
#endif

namespace HugeCTR {
/**
 * The LocalizedSlotSparseEmbeddingHash class inherits from Embedding class, which is the base 
 * class for implementing all embedding layers. In this class, some of the slots in the embedding 
 * table are assigned to a single GPU, which are called localized slots. For example, slot-0 on 
 * GPU-0, slot-1 on GPU-1, slot-2 on GPU-0, slot-3 on GPU-1, etc. The embedding table is encapsulated 
 * in a hash table. The key in the hash table is called as hash_table_key, and the value in the hash 
 * table is called as hash_table_value_index that means it indicates the embedding feature's row 
 * number in the embedding table, and the embedding feature is called as hash_table_value. This class 
 * implements all the operations needed by the training process of embedding layer, including forward 
 * propagation and backward propagation. The forward propagation is corresponding to the API forward(). 
 * The backward propagation is divided into 2-stage APIs: backward() and update_params(). The class 
 * also provides the operations for uploading hash tables(including hash_table_key, hash_table_value_index 
 * and hash_table_value) from a host file to GPUs(which named upload_params_to_device()), and for 
 * downloading hash tables from GPUs to a host file(which named download_params_to_host()).
 */

template <typename TypeHashKey>
class LocalizedSlotSparseEmbeddingHash : public Embedding<TypeHashKey> {
  using Base = Embedding<TypeHashKey>;

  using TypeHashValueIndex = TypeHashKey;  // use the hash key type as the hash value_index type(it
                                           // will be uint32 or int64)
  using NvHashTable = nv::HashTable<TypeHashKey, TypeHashValueIndex, std::numeric_limits<TypeHashKey>::max()>;
  
  using comm_handler_traits = FasterGossipComm::FasterGossipCommAll2AllTraits<float>;
  using comm_handler = FasterGossipComm::FasterGossipComm<float, comm_handler_traits>;

 private:
  SparseEmbeddingHashParams embedding_params_; /**< Sparse embedding hash params. */

  std::vector<OptParams> opt_params_; /**< Optimizer params. */
  std::vector<std::unique_ptr<NvHashTable>> hash_tables_; /**< Hash table.  */

  // define tensors
  Tensors<float> hash_table_value_tensors_; /**< Hash table value. */
  Tensors<TypeHashValueIndex> hash_table_slot_id_tensors_; /**< the tensors for storing slot ids */
  Tensors<TypeHashValueIndex>
      hash_value_index_tensors_; /**< Hash value index. The index is corresponding to the line
                                    number of the value. */
  Tensors<float>
      embedding_feature_tensors_; /**< the output tensor of the forward(). */
  Tensors<float> wgrad_tensors_;  /**< the input tensor of the backward(). */
  Tensors<float>
      opt_m_tensors_; /**< The mi variable storage for adam optimizer in the update_params(). */
  Tensors<float>
      opt_v_tensors_; /**< The vi variable storage for adam optimizer in the update_params(). */
  Tensors<float> opt_momentum_tensors_; /**< The momentum variable storage for the momentum
                                           optimizer in the update_params(). */
  Tensors<float> opt_accm_tensors_;     /**< The accm variable storage for the nesterov
                                                         optimizer in the update_params(). */
  // Tensors<TypeHashKey>
  //     row_offset_allreduce_tensors_; /**< The temp memory to store the row_offset after all_reduce
  //                                       operation among multi-gpu in forward(). */
  Tensors<TypeHashValueIndex>
      hash_value_index_sort_tensors_; /**< The temp memory to store the sorted hash table value
                                         indexes in update_params(). */
  Tensors<uint32_t> hash_value_index_count_tensors_; /**< The temp memory to store the count of hash
                                                        table value indexes in update_params(). */
  Tensors<uint32_t>
      hash_value_index_count_offset_tensors_; /**< The temp memory to store the offset of each count
                                                 of hash table value indexes in update_params(). */
  Tensors<uint32_t> hash_value_index_count_counter_tensors_; /**< The temp memory to store the
                                                                counter of the count of hash table
                                                                value indexes in update_params(). */
  Tensors<TypeHashKey> sample_id_tensors_;      /**< The temp memory to store the sample ids of hash
                                                   table value in      update_params(). */
  Tensors<TypeHashKey> sample_id_sort_tensors_; /**< The temp memory to store the sorted sample ids
                                                   of hash table value in update_params(). */
  Tensors<TypeHashKey> temp_storage_sort_tensors_; /**< The temp memory for the CUB lib sorting API
                                                      in update_params(). */
  Tensors<TypeHashValueIndex>
      deltaw_hash_value_index_tensors_; /**< The temp memory to store the hash table indexes of
                                           deltaw in update_params(). */
  Tensors<float> deltaw_tensors_; /**< The temp memory to store the deltaw in update_params(). */

  // define GeneralBuffers
  GeneralBuffers<float> float_bufs_;     /**< float type general buffer. */
  GeneralBuffers<uint32_t> uint32_bufs_; /**< uint32 type general buffer. */
  GeneralBuffers<TypeHashKey> key_bufs_; /**< TypeHashKey type general buffer. */
  GeneralBuffers<TypeHashValueIndex>
      value_index_bufs_; /**< TypeHashValueIndex type general buffer. */

  std::vector<size_t> temp_storage_sort_bytes_;  /**< The temp variable for CUB lib sorting API. */
  int max_vocabulary_size_per_gpu_;               /**< Max vocabulary size for each GPU. */
  int slot_num_per_gpu_; /*< slot number per GPU */

  SparseEmbeddingHashFunctors functors_; /**< obj of SparseEmbeddingHashFunctors */

  std::string plan_file_; /*< plan file for all2all */
  std::unique_ptr<comm_handler> all2all_forward_; /**< obj of all2all for forward */
  std::unique_ptr<comm_handler> all2all_backward_; /**< obj of all2all for backward */
  Tensors<float> all2all_tensors_; /**< the temple buffer to store all2all results */

 public:
  /**
   * The constructor of LocalizedSlotSparseEmbeddingHash.
   * @param row_offsets_tensors row offsets of the input tensor(refer to row offset vector in sparse
   * matrix CSR format).
   * @param hash_key_tensors hash keys of the input tensor(refer to value vector in sparse matrix
   * CSR format).
   * @param embedding_params embedding params for initialization.
   * @param gpu_resource_group the GPU resource group
   */
  LocalizedSlotSparseEmbeddingHash(const Tensors<TypeHashKey> &row_offsets_tensors,
                      const Tensors<TypeHashKey> &hash_key_tensors,
                      SparseEmbeddingHashParams embedding_params,
                      const std::string plan_file,
                      const std::shared_ptr<GPUResourceGroup> &gpu_resource_group);
  /**
   * The forward propagation of embedding layer.
   */
  void forward() override;
  /**
   * The first stage of backward propagation of embedding layer,
   * which computes the wgrad by the dgrad from the top layer.
   */
  void backward() override;
  /**
   * This function is used for implementing CPU multi-threads when doing
   * update_params() on multi-GPUs. In this case, each CPU thread corresponding
   * to one GPU.
   * @param tid the CPU thread id.
   */
  void update_params_per_thread(int tid);
  /**
   * The second stage of backward propagation of embedding layer, which
   * updates the hash table by wgrad(from backward()) and optimizer.
   */
  void update_params() override;
  /**
   * Read the hash table from the weight_stream on the host, and
   * upload it onto multi-GPUs global memory.
   * @param weight_stream the host file stream for reading data from.
   */
  void upload_params_to_device(std::ifstream &weight_stream) override;
  /**
   * Download the hash table from multi-GPUs global memroy to CPU memory
   * and write it to the weight_stream on the host.
   * @param weight_stream the host file stream for writing data to.
   */
  void download_params_to_host(std::ofstream &weight_stream) override;
  /**
   * Get the total size of hash tables on all GPUs.
   */
  long long get_params_num() override;

  // only used for results check
  /**
   * Get the forward() results from GPUs and copy them to the host pointer
   * embedding_feature. This function is only used for unit test.
   * @param embedding_feature the host pointer for storing the forward()
   * results.
   */
  void get_forward_results(float *embedding_feature) override;
  /**
   * Get the backward() results from GPUs and copy them to the host pointer
   * wgrad. The wgrad on each GPU should be the same. This function is only
   * used for unit test.
   * @param wgrad the host pointer for stroing the backward() results.
   * @param devIndex the GPU device id.
   */
   void get_backward_results(float *wgrad, int devIndex) override;
  /**
   * Get the update_params() results(the hash table, including hash_table_keys
   * and hash_table_values) from GPUs and copy them to the host pointers.
   * This function is only used for unit test.
   * @param hash_table_key the host pointer for stroing the hash table keys.
   * @param hash_table_value the host pointer for stroing the hash table values.
   */
  void get_update_params_results(TypeHashKey *hash_table_key, float *hash_table_value) override;

};  // end of class LocalizedSlotSparseEmbeddingHash

template <typename TypeHashKey>
LocalizedSlotSparseEmbeddingHash<TypeHashKey>::LocalizedSlotSparseEmbeddingHash(
    const Tensors<TypeHashKey> &row_offsets_tensors, 
    const Tensors<TypeHashKey> &hash_key_tensors,
    SparseEmbeddingHashParams embedding_params,
    const std::string plan_file,
    const std::shared_ptr<GPUResourceGroup> &gpu_resource_group)
    : embedding_params_(embedding_params),
      plan_file_(plan_file),
      Base(row_offsets_tensors, hash_key_tensors, embedding_params.batch_size,
           embedding_params.slot_num, embedding_params.embedding_vec_size, gpu_resource_group) {
  try {
    int total_gpu_count = Base::device_resources_->get_total_gpu_count();
    int local_gpu_count = Base::device_resources_->size();
    CudaDeviceContext context((*Base::device_resources_)[0]->get_device_id());

    // CAUSION: can not decide how many <key,value> pairs in each GPU, because the GPU distribution
    // is computed by (key%local_gpu_count) In order to not allocate the total size of hash table on each
    // GPU, meanwhile get a better performance by a unfull hash table, the users need to set the
    // param "load_factor"(load_factor<1). The size of
    // max_vocabulary_size_per_gpu_=vocabulary_size/total_gpu_count/load_factor, which should be more than
    // vocabulary_size/total_gpu_count.
    max_vocabulary_size_per_gpu_ =
        (int)((float)embedding_params_.vocabulary_size /
              total_gpu_count / embedding_params_.load_factor);

    slot_num_per_gpu_ = (embedding_params_.slot_num + total_gpu_count - 1) / total_gpu_count;

#ifndef NDEBUG
    std::cout << "max_vocabulary_size_per_gpu_:" << max_vocabulary_size_per_gpu_ << std::endl;
    std::cout << "slot_num_per_gpu_:" << slot_num_per_gpu_ << std::endl;
#endif

    // for hash_table_value initialization
    HugeCTR::UnifiedDataSimulator<float> fdata_sim(-1.f / embedding_params_.embedding_vec_size,
                                                   1.f / embedding_params_.embedding_vec_size);
    float *h_hash_table_value;
    CK_CUDA_THROW_(cudaMallocHost(
        &h_hash_table_value,
        max_vocabulary_size_per_gpu_ * embedding_params_.embedding_vec_size * sizeof(float)));
    for (long long i = 0;
         i < (max_vocabulary_size_per_gpu_ * embedding_params_.embedding_vec_size); i++) {
      h_hash_table_value[i] = fdata_sim.get_num();
    }

    for (int id = 0; id < local_gpu_count; id++) {
      int cur_device = (*Base::device_resources_)[id]->get_device_id();
      context.set_device(cur_device);

      // construct HashTable object: used to store hash table <key, value_index>
      hash_tables_.emplace_back(
          new NvHashTable(max_vocabulary_size_per_gpu_));

      // new GeneralBuffer objects
      float_bufs_.emplace_back(new GeneralBuffer<float>());
      uint32_bufs_.emplace_back(new GeneralBuffer<uint32_t>());
      key_bufs_.emplace_back(new GeneralBuffer<TypeHashKey>());
      value_index_bufs_.emplace_back(new GeneralBuffer<TypeHashValueIndex>());

      // new hash table value vectors
      hash_table_value_tensors_.emplace_back(
          new Tensor<float>({max_vocabulary_size_per_gpu_, embedding_params_.embedding_vec_size},
                            float_bufs_.back(), TensorFormat_t::HW));

      // new hash table value_index that get() from HashTable
      hash_value_index_tensors_.emplace_back(new Tensor<TypeHashValueIndex>(
          {1, embedding_params_.batch_size * embedding_params_.max_feature_num},
          value_index_bufs_.back(), TensorFormat_t::HW));

      // new embedding features reduced by hash table values(results of forward)
      embedding_feature_tensors_.emplace_back(
          new Tensor<float>({embedding_params_.batch_size * slot_num_per_gpu_,
                             embedding_params_.embedding_vec_size},
                            float_bufs_.back(), TensorFormat_t::HW));

      // new wgrad used by backward
      wgrad_tensors_.emplace_back(
          new Tensor<float>({embedding_params_.batch_size * slot_num_per_gpu_,
                             embedding_params_.embedding_vec_size},
                            float_bufs_.back(), TensorFormat_t::HW));

      // new optimizer params used by update_params
      opt_params_.push_back(OptParams());
      opt_params_[id].optimizer = embedding_params_.opt_params.optimizer;
      opt_params_[id].lr = embedding_params_.opt_params.lr;
      switch (embedding_params_.opt_params.optimizer) {
        case 0:  // adam
          opt_m_tensors_.emplace_back(
              new Tensor<float>({max_vocabulary_size_per_gpu_, embedding_params_.embedding_vec_size},
                                float_bufs_.back(), TensorFormat_t::HW));
          opt_v_tensors_.emplace_back(
              new Tensor<float>({max_vocabulary_size_per_gpu_, embedding_params_.embedding_vec_size},
                                float_bufs_.back(), TensorFormat_t::HW));
          break;

        case 1:  // momentum_sgd
          opt_momentum_tensors_.emplace_back(
              new Tensor<float>({max_vocabulary_size_per_gpu_, embedding_params_.embedding_vec_size},
                                float_bufs_.back(), TensorFormat_t::HW));
          break;

        case 2:  // nesterov
          opt_accm_tensors_.emplace_back(
              new Tensor<float>({max_vocabulary_size_per_gpu_, embedding_params_.embedding_vec_size},
                                float_bufs_.back(), TensorFormat_t::HW));
          break;

        default:
          throw std::runtime_error(
              std::string("[HCDEBUG][ERROR] Runtime error: Invalid optimizer type: ") +
              std::to_string(embedding_params_.opt_params.optimizer) + "\n");
      }

      // new temp tensors used by update_params
      // row_offset_allreduce_tensors_.emplace_back(new Tensor<TypeHashKey>(
      //     {1, embedding_params_.batch_size * embedding_params_.slot_num + 1}, key_bufs_.back(),
      //     TensorFormat_t::HW));
      sample_id_tensors_.emplace_back(new Tensor<TypeHashKey>(
          {1, embedding_params_.batch_size * embedding_params_.max_feature_num}, key_bufs_.back(),
          TensorFormat_t::HW));
      sample_id_sort_tensors_.emplace_back(new Tensor<TypeHashKey>(
          {1, embedding_params_.batch_size * embedding_params_.max_feature_num}, key_bufs_.back(),
          TensorFormat_t::HW));
      hash_value_index_sort_tensors_.emplace_back(new Tensor<TypeHashValueIndex>(
          {1, embedding_params_.batch_size * embedding_params_.max_feature_num},
          value_index_bufs_.back(), TensorFormat_t::HW));
      hash_value_index_count_tensors_.emplace_back(new Tensor<uint32_t>(
          {1, embedding_params_.batch_size * embedding_params_.max_feature_num},
          uint32_bufs_.back(), TensorFormat_t::HW));
      hash_value_index_count_offset_tensors_.emplace_back(new Tensor<uint32_t>(
          {1, embedding_params_.batch_size * embedding_params_.max_feature_num},
          uint32_bufs_.back(), TensorFormat_t::HW));
      hash_value_index_count_counter_tensors_.emplace_back(
          new Tensor<uint32_t>({1, 1}, uint32_bufs_.back(), TensorFormat_t::HW));
      deltaw_hash_value_index_tensors_.emplace_back(new Tensor<TypeHashValueIndex>(
          {1, embedding_params_.batch_size * embedding_params_.max_feature_num},
          value_index_bufs_.back(), TensorFormat_t::HW));
      deltaw_tensors_.emplace_back(
          new Tensor<float>({embedding_params_.batch_size * embedding_params_.max_feature_num,
                             embedding_params_.embedding_vec_size},
                            float_bufs_.back(), TensorFormat_t::HW));

      // cal the temp storage bytes for CUB radix sort
      size_t temp = 0;
      cub::DeviceRadixSort::SortPairs(
          (void *)NULL, (size_t &)temp, (TypeHashKey *)NULL, (TypeHashKey *)NULL,
          (TypeHashKey *)NULL, (TypeHashKey *)NULL,
          embedding_params_.batch_size * embedding_params_.max_feature_num);
      temp_storage_sort_bytes_.push_back(temp);
      int size = (int)ceil((float)temp_storage_sort_bytes_[id] / sizeof(TypeHashKey));

      // new temp storage tensors for CUB radix sort
      temp_storage_sort_tensors_.emplace_back(
          new Tensor<TypeHashKey>({1, size}, key_bufs_.back(), TensorFormat_t::HW));

      // temp tensors for all2all 
      all2all_tensors_.emplace_back(
          new Tensor<float>({embedding_params_.batch_size * slot_num_per_gpu_,
          embedding_params_.embedding_vec_size}, float_bufs_.back(), TensorFormat_t::HW));

      // the tenosrs for storing slot ids
      // TODO: init to -1 ?
      hash_table_slot_id_tensors_.emplace_back(
          new Tensor<TypeHashValueIndex>({max_vocabulary_size_per_gpu_, 1},
          value_index_bufs_.back(), TensorFormat_t::HW));

      // init GenenralBuffers to do real allocation
#ifndef NDEBUG
      std::cout << " max_feature_num_:" << embedding_params_.max_feature_num;
      std::cout << " slot_num_per_gpu_:" << slot_num_per_gpu_;
      std::cout << " float_bufs_:" << float_bufs_.back()->get_size();
      std::cout << " uint32_bufs_:" << uint32_bufs_.back()->get_size();
      std::cout << " key_bufs_:" << key_bufs_.back()->get_size();
      std::cout << " value_index_bufs_:" << value_index_bufs_.back()->get_size() << std::endl;
#endif
      float_bufs_.back()->init(cur_device);
      uint32_bufs_.back()->init(cur_device);
      key_bufs_.back()->init(cur_device);
      value_index_bufs_.back()->init(cur_device);

      // do hash table value initialization
      CK_CUDA_THROW_(cudaMemcpy(
          hash_table_value_tensors_[id]->get_ptr(), h_hash_table_value,
          max_vocabulary_size_per_gpu_ * embedding_params_.embedding_vec_size * sizeof(float),
          cudaMemcpyHostToDevice));

      switch (embedding_params_.opt_params.optimizer) {
        case 0:  // adam
          CK_CUDA_THROW_(cudaMemsetAsync(
              opt_m_tensors_[id]->get_ptr(), 0,
              max_vocabulary_size_per_gpu_ * embedding_params_.embedding_vec_size * sizeof(float),
              (*Base::device_resources_)[id]->get_stream()));
          CK_CUDA_THROW_(cudaMemsetAsync(
              opt_v_tensors_[id]->get_ptr(), 0,
              max_vocabulary_size_per_gpu_ * embedding_params_.embedding_vec_size * sizeof(float),
              (*Base::device_resources_)[id]->get_stream()));
          opt_params_[id].hyperparams.adam.times = 0;
          opt_params_[id].hyperparams.adam.alpha_t =
              embedding_params_.opt_params.hyperparams.adam.alpha_t;
          opt_params_[id].hyperparams.adam.beta1 =
              embedding_params_.opt_params.hyperparams.adam.beta1;
          opt_params_[id].hyperparams.adam.beta2 =
              embedding_params_.opt_params.hyperparams.adam.beta2;
          opt_params_[id].hyperparams.adam.epsilon =
              embedding_params_.opt_params.hyperparams.adam.epsilon;
          opt_params_[id].hyperparams.adam.m_ptr = opt_m_tensors_[id]->get_ptr();
          opt_params_[id].hyperparams.adam.v_ptr = opt_v_tensors_[id]->get_ptr();
          break;

        case 1:  // momentum_sgd
          CK_CUDA_THROW_(cudaMemsetAsync(
              opt_momentum_tensors_[id]->get_ptr(), 0,
              max_vocabulary_size_per_gpu_ * embedding_params_.embedding_vec_size * sizeof(float),
              (*Base::device_resources_)[id]->get_stream()));
          opt_params_[id].hyperparams.momentum.factor =
              embedding_params_.opt_params.hyperparams.momentum.factor;
          opt_params_[id].hyperparams.momentum.momentum_ptr = opt_momentum_tensors_[id]->get_ptr();
          break;

        case 2:  // nesterov
          CK_CUDA_THROW_(cudaMemsetAsync(
              opt_accm_tensors_[id]->get_ptr(), 0,
              max_vocabulary_size_per_gpu_ * embedding_params_.embedding_vec_size * sizeof(float),
              (*Base::device_resources_)[id]->get_stream()));
          opt_params_[id].hyperparams.nesterov.mu =
              embedding_params_.opt_params.hyperparams.nesterov.mu;
          opt_params_[id].hyperparams.nesterov.accm_ptr = opt_accm_tensors_[id]->get_ptr();
          break;

        default:
          throw std::runtime_error(
              std::string("[HCDEBUG][ERROR] Runtime error: Invalid optimizer type: ") +
              std::to_string(embedding_params_.opt_params.optimizer) + "\n");
      }

    }  // end of for(int id = 0; id < local_gpu_count; id++)

    // all2all init
    // TODO: (just support intra-node currently)
    const size_t element_per_send = (embedding_params_.batch_size * slot_num_per_gpu_) * \
                                    embedding_params_.embedding_vec_size / total_gpu_count;
    // std::cout << "slot_num_per_gpu_=" << slot_num_per_gpu_ << std::endl;
    // std::cout << "element_per_send=" << element_per_send << std::endl;
    // std::cout << "embedding_feature_tensors_=" << embedding_feature_tensors_[0]->get_ptr() << std::endl;
    // std::cout << "all2all_tensors_" << all2all_tensors_[0]->get_ptr() << std::endl;
    functors_.all2all_init(all2all_forward_, plan_file_, element_per_send, embedding_feature_tensors_, \
                          all2all_tensors_, Base::device_resources_);
    functors_.all2all_init(all2all_backward_, plan_file_, element_per_send, Base::output_tensors_, \
                          all2all_tensors_, Base::device_resources_);
    // sync
    functors_.sync_all_gpus(Base::device_resources_, context);

    CK_CUDA_THROW_(cudaFreeHost(h_hash_table_value));
  } catch (const std::runtime_error &rt_err) {
    std::cerr << rt_err.what() << std::endl;
    throw;
  }

  return;
}  // end of LocalizedSlotSparseEmbeddingHash()

template <typename TypeHashKey>
long long LocalizedSlotSparseEmbeddingHash<TypeHashKey>::get_params_num() {
  //  return ((long long)embedding_params_.embedding_vec_size * embedding_params_.vocabulary_size);

  // Read data from input_buffers_ -> look up -> write to output_tensors

  long long total_size = 0;

  CudaDeviceContext context((*Base::device_resources_)[0]->get_device_id());

  // need to collect the <key, value> pair count from all GPUs and do sum reduction
  int local_gpu_count = Base::device_resources_->size();
  for (int id = 0; id < local_gpu_count; id++) {
    context.set_device((*Base::device_resources_)[id]->get_device_id());
    total_size += hash_tables_[id]->get_size((*Base::device_resources_)[id]->get_stream());
    CK_CUDA_THROW_(cudaStreamSynchronize((*Base::device_resources_)[id]->get_stream()));
  }

  total_size *= (long long)embedding_params_.embedding_vec_size;

  return total_size;
}

template <typename TypeHashKey>
void LocalizedSlotSparseEmbeddingHash<TypeHashKey>::forward() {
  // Read data from input_buffers_ -> look up -> write to output_tensors

  CudaDeviceContext context((*Base::device_resources_)[0]->get_device_id());

  int local_gpu_count = Base::device_resources_->size();
  int total_gpu_count = Base::device_resources_->get_total_gpu_count();

  // do forward propagation
  functors_.forward(embedding_params_.batch_size,
                  slot_num_per_gpu_, 
                  embedding_params_.embedding_vec_size,
                  embedding_params_.combiner,
                  Base::row_offsets_tensors_,
                  Base::value_tensors_,
                  hash_tables_,
                  hash_table_value_tensors_, 
                  hash_value_index_tensors_,
                  embedding_feature_tensors_,
                  Base::device_resources_,
                  context);

  // sync
  functors_.sync_all_gpus(Base::device_resources_, context);

  // do all-to-all (just support intra-node currently)
  // src=embedding_feature_tensors_; dst=Base::output_tensors_
  functors_.all2all_async(all2all_forward_);

  // sync
  functors_.sync_all_gpus(Base::device_resources_, context);

  // reorder 
  functors_.reorder(embedding_params_.batch_size,
                    embedding_params_.slot_num, 
                    embedding_params_.embedding_vec_size,
                    all2all_tensors_, 
                    Base::output_tensors_,
                    Base::device_resources_,
                    context);

  // sync
  functors_.sync_all_gpus(Base::device_resources_, context);

  // store slot ids
  functors_.store_slot_id(embedding_params_.batch_size,
                    embedding_params_.slot_num, 
                    Base::row_offsets_tensors_, 
                    hash_value_index_tensors_,
                    hash_table_slot_id_tensors_,
                    Base::device_resources_,
                    context);
  // sync
  functors_.sync_all_gpus(Base::device_resources_, context);

  return;
}  // end of forward()

template <typename TypeHashKey>
void LocalizedSlotSparseEmbeddingHash<TypeHashKey>::backward() {
  // Read dgrad from output_tensors -> compute wgrad

  CudaDeviceContext context((*Base::device_resources_)[0]->get_device_id());

  // reorder 
  functors_.reorder(embedding_params_.batch_size,
                    embedding_params_.slot_num, 
                    embedding_params_.embedding_vec_size,
                    Base::output_tensors_, 
                    all2all_tensors_,
                    Base::device_resources_,
                    context);
  
  // sync
  functors_.sync_all_gpus(Base::device_resources_, context);

  // do all2all to collect the top_grad
  // src=all2all_tensors_; dst=embedding_feature_tensors_
  functors_.all2all_async(all2all_backward_);

  // sync
  functors_.sync_all_gpus(Base::device_resources_, context);

  // do backward
  functors_.backward(embedding_params_.batch_size, 
                    slot_num_per_gpu_,
                    embedding_params_.embedding_vec_size, 
                    embedding_params_.combiner, 
                    Base::row_offsets_tensors_,
                    embedding_feature_tensors_,
                    wgrad_tensors_,
                    Base::device_resources_,
                    context);

  // sync
  functors_.sync_all_gpus(Base::device_resources_, context);

  return;
}  // end of backward()

template <typename TypeHashKey>
void LocalizedSlotSparseEmbeddingHash<TypeHashKey>::update_params_per_thread(int tid) {
#ifndef NDEBUG
  MESSAGE_("update_params_per_thread: this is thread: " + std::to_string(tid));
#endif

  CudaDeviceContext context((*Base::device_resources_)[tid]->get_device_id());

  // accumulate times for adam optimizer
  opt_params_[tid].hyperparams.adam.times++;

  // do update params operation
  functors_.update_params((*Base::device_resources_)[tid]->get_stream(), 
                    embedding_params_.batch_size,
                    slot_num_per_gpu_, 
                    embedding_params_.embedding_vec_size, 
                    max_vocabulary_size_per_gpu_,
                    opt_params_[tid], 
                    Base::row_offsets_tensors_[tid]->get_ptr(),
                    Base::value_tensors_[tid]->get_ptr(), 
                    hash_tables_[tid].get(),
                    hash_value_index_tensors_[tid]->get_ptr(), 
                    sample_id_tensors_[tid]->get_ptr(),
                    sample_id_sort_tensors_[tid]->get_ptr(), 
                    hash_value_index_sort_tensors_[tid]->get_ptr(),
                    hash_value_index_count_tensors_[tid]->get_ptr(),
                    hash_value_index_count_offset_tensors_[tid]->get_ptr(),
                    hash_value_index_count_counter_tensors_[tid]->get_ptr(),
                    temp_storage_sort_tensors_[tid]->get_ptr(), 
                    temp_storage_sort_bytes_[tid],
                    wgrad_tensors_[tid]->get_ptr(), 
                    deltaw_hash_value_index_tensors_[tid]->get_ptr(),
                    deltaw_tensors_[tid]->get_ptr(), 
                    hash_table_value_tensors_[tid]->get_ptr());
                    
  // stream sync on single GPU
  CK_CUDA_THROW_(cudaStreamSynchronize((*Base::device_resources_)[tid]->get_stream()));

  return;
}

template <typename TypeHashKey>
static void update_params_per_thread_wrapper_hash_localized(
    int tid, LocalizedSlotSparseEmbeddingHash<TypeHashKey> *sparse_embedding_hash) {
  sparse_embedding_hash->update_params_per_thread(tid);
}

template <typename TypeHashKey>
void LocalizedSlotSparseEmbeddingHash<TypeHashKey>::update_params() {
  int local_gpu_count = Base::device_resources_->size();
  int total_gpu_count = Base::device_resources_->get_total_gpu_count();

  if (total_gpu_count > 1) {  // use multiple CPU threads to launch tasks on multiple GPUs
    // launch threads
    for (int id = 0; id < local_gpu_count; id++) {
      Base::device_resources_->results[id] = Base::device_resources_->train_thread_pool.push(
          std::ref(update_params_per_thread_wrapper_hash_localized<TypeHashKey>), this);
    }

    // wait for threads completion
    for (int id = 0; id < local_gpu_count; id++) {
      Base::device_resources_->results[id].get();
    }
  } else if (total_gpu_count == 1) {  // use current main thread to launch task on one GPU
    update_params_per_thread(0);
  } else {
    throw std::runtime_error(
        std::string("[HCDEBUG][ERROR] Runtime error: total_gpu_count <= 0 \n"));
  }

  return;
}

// read hash_table_key and hash_table_value from host file, and write to GPU
template <typename TypeHashKey>
void LocalizedSlotSparseEmbeddingHash<TypeHashKey>::upload_params_to_device(std::ifstream &weight_stream) {
#ifndef NDEBUG
  MESSAGE_("upload_params_to_device");
#endif  
  
  // check if file is opened successfully
  if (!weight_stream.is_open()) {
    CK_THROW_(Error_t::WrongInput, "Error: file not open for reading");
  }

  CudaDeviceContext context((*Base::device_resources_)[0]->get_device_id());

  functors_.upload_params_to_device<TypeHashKey, TypeHashValueIndex>(weight_stream,
                            embedding_params_.vocabulary_size,
                            embedding_params_.embedding_vec_size,
                            max_vocabulary_size_per_gpu_,
                            hash_table_value_tensors_,
                            hash_table_slot_id_tensors_,
                            hash_tables_,
                            Base::device_resources_,
                            context);

  return;
}  // end of upload_params_to_device()

// read hash_table_key and hash_table_value from GPU, and write to the file on the host
template <typename TypeHashKey>
void LocalizedSlotSparseEmbeddingHash<TypeHashKey>::download_params_to_host(std::ofstream &weight_stream) {
  // check if the file is opened successfully
  if (!weight_stream.is_open()) {
    CK_THROW_(Error_t::WrongInput, "Error: file not open for writing");
    return;
  }

  CudaDeviceContext context((*Base::device_resources_)[0]->get_device_id());

  functors_.download_params_to_host(weight_stream,
                            embedding_params_.vocabulary_size,
                            embedding_params_.embedding_vec_size,
                            max_vocabulary_size_per_gpu_,
                            hash_table_value_tensors_,
                            hash_table_slot_id_tensors_,
                            hash_tables_,
                            Base::device_resources_,
                            context);

  return;
}  // end of download_params_to_host()

// only used for results check: copy forward results from output_tensors_ to embedding_feature(input
// CPU buffer)
template <typename TypeHashKey>
void LocalizedSlotSparseEmbeddingHash<TypeHashKey>::get_forward_results(float *embedding_feature) {

  CudaDeviceContext context((*Base::device_resources_)[0]->get_device_id());

  int total_gpu_count = Base::device_resources_->get_total_gpu_count();
  int batchsize_per_gpu =
      (int)(embedding_params_.batch_size / total_gpu_count);
  int memcpy_size =
      batchsize_per_gpu * embedding_params_.slot_num * embedding_params_.embedding_vec_size;

  functors_.get_forward_results(memcpy_size,
                              Base::output_tensors_,
                              embedding_feature,
                              Base::device_resources_,
                              context);

  // sync
  functors_.sync_all_gpus(Base::device_resources_, context);

  return;
}  // end of get_forward_results()

// only used for results check: copy backward() results from wgrad_tensors_ to wgrad(input CPU
// buffer)
template <typename TypeHashKey>
void LocalizedSlotSparseEmbeddingHash<TypeHashKey>::get_backward_results(float *wgrad, int devIndex) {
  CudaDeviceContext context((*Base::device_resources_)[0]->get_device_id());

  Tensors<float> all2all_tensors;
  Tensors<float> reorder_tenors;
  GeneralBuffers<float> float_bufs;
  int local_gpu_count = Base::device_resources_->size();
  int total_gpu_count = Base::device_resources_->get_total_gpu_count();

  for (int id = 0; id < local_gpu_count; id++) { 
    int cur_device = (*Base::device_resources_)[id]->get_device_id();
    context.set_device(cur_device);

    float_bufs.emplace_back(new GeneralBuffer<float>());

    all2all_tensors.emplace_back(
        new Tensor<float>({embedding_params_.batch_size * slot_num_per_gpu_,
                          embedding_params_.embedding_vec_size},
                          float_bufs.back(), TensorFormat_t::HW));

    reorder_tenors.emplace_back(
        new Tensor<float>({embedding_params_.batch_size * slot_num_per_gpu_,
                          embedding_params_.embedding_vec_size},
                          float_bufs.back(), TensorFormat_t::HW));    

    float_bufs.back()->init(cur_device); 
  }

  // all2all
  std::unique_ptr<comm_handler> all2all;
  const size_t element_per_send = (embedding_params_.batch_size * slot_num_per_gpu_) * \
                                  embedding_params_.embedding_vec_size / total_gpu_count;
  functors_.all2all_init(all2all, plan_file_, element_per_send, wgrad_tensors_, \
                        all2all_tensors, Base::device_resources_);
  functors_.all2all_async(all2all);

  // sync
  functors_.sync_all_gpus(Base::device_resources_, context);

  // reorder 
  functors_.reorder(embedding_params_.batch_size,
                    embedding_params_.slot_num, 
                    embedding_params_.embedding_vec_size,
                    all2all_tensors, 
                    reorder_tenors,
                    Base::device_resources_,
                    context);
  
  // sync
  functors_.sync_all_gpus(Base::device_resources_, context);

  // there are batch_size_per_gpu samples' wgard on each GPU 
  int batchsize_per_gpu =
      (int)(embedding_params_.batch_size / total_gpu_count);
  int memcpy_size = batchsize_per_gpu * embedding_params_.slot_num *
                    embedding_params_.embedding_vec_size;
  functors_.get_backward_results(memcpy_size,
                                reorder_tenors,
                                wgrad,
                                Base::device_resources_,
                                context);

  return;
}  // end of get_backward_results()

// only used for results check: copy hash_tabale <key, value> from gpu to cpu
template <typename TypeHashKey>
void LocalizedSlotSparseEmbeddingHash<TypeHashKey>::get_update_params_results(TypeHashKey *hash_table_key,
                                                                        float *hash_table_value) {
  CudaDeviceContext context((*Base::device_resources_)[0]->get_device_id());

  functors_.get_update_params_results(max_vocabulary_size_per_gpu_,
                                    embedding_params_.embedding_vec_size,
                                    embedding_params_.vocabulary_size,
                                    hash_table_value_tensors_,
                                    hash_tables_,
                                    hash_table_key,
                                    hash_table_value,
                                    Base::device_resources_,
                                    context);

  return;

}  // end of get_update_params_results()

}  // namespace HugeCTR
