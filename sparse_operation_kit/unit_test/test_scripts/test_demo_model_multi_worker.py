"""
 Copyright (c) 2021, NVIDIA CORPORATION.
 
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
"""

import argparse

import sys
sys.path.append("../../") # where to find plugin
import sparse_operation_kit as sok
import tensorflow as tf

import numpy as np
import os, json
import pickle
import utils

from test_demo_model_single_worker import SOKDemo, test_tf_demo

def test_sok_demo(args, init_tensors, *random_samples):
    port = 12345
    os.environ["TF_CONFIG"] = json.dumps({
        'cluster': {"worker": [args.ips[i] + ":" + str(port + i) for i in range(args.worker_num)] },
        'task': {"type": 'worker', "index": args.task_id}
    })
    strategy = tf.distribute.MultiWorkerMirroredStrategy()
    with strategy.scope():
        result = sok.Init(global_batch_size=args.global_batch_size)

        plugin_demo = SOKDemo(combiner=args.combiner, 
                            max_vocabulary_size_per_gpu=args.max_vocabulary_size_per_gpu,
                            slot_num=args.slot_num, max_nnz=args.max_nnz,
                            embedding_vec_size=args.embedding_vec_size)

        emb_opt = utils.get_embedding_optimizer(args.optimizer)(learning_rate=0.1)
        dense_opt = utils.get_dense_optimizer(args.optimizer)(learning_rate=0.1)

    plugin_saver = sok.Saver()
    status = plugin_saver.load_tensors_to_variable(plugin_demo.embedding_layer.embedding_variable, init_tensors)

    loss_fn = tf.keras.losses.BinaryCrossentropy(from_logits=True, reduction=tf.keras.losses.Reduction.NONE)
    def _replica_loss(labels, logits):
        loss = loss_fn(labels, logits)
        return tf.nn.compute_average_loss(loss, global_batch_size=args.global_batch_size)

    @tf.function
    def _train_step(inputs, labels):
        with tf.GradientTape() as tape:
            logit, embedding_vector = plugin_demo(inputs, training=True)
            loss = _replica_loss(labels, logit)
        embedding_variables, other_variable = sok.split_embedding_variable_from_others(plugin_demo.trainable_variables)
        grads, emb_grads = tape.gradient(loss, [other_variable, embedding_variables])
        if "plugin" not in args.optimizer:
            with sok.OptimizerScope(embedding_variable):
                emb_opt.apply_gradients(zip(emb_grads, embedding_variables),
                                        experimental_aggregate_gradients=False)
        else:
            emb_opt.apply_gradients(zip(emb_grads, embedding_variables),
                                    experimental_aggregate_gradients=False)
        dense_opt.apply_gradients(zip(grads, other_variable))
        return logit, embedding_vector

    sok_results = list()

    def _dataset_fn(input_context):
        replica_batch_size = input_context.get_per_replica_batch_size(args.global_batch_size)
        dataset = utils.tf_dataset(*random_samples, batchsize=replica_batch_size, to_sparse_tensor=True, repeat=1)
        # because each worker has its own data source, so that no need to shard the dataset.
        return dataset

    dataset = strategy.distribute_datasets_from_function(_dataset_fn)

    for i, (sparse_tensors, replica_labels) in enumerate(dataset):
        print("-" * 30, "step ", str(i), "-" * 30)
        logit, embedding_vector = strategy.run(_train_step, args=(sparse_tensors, replica_labels))
        print("[INFO]: embedding_vector\n", embedding_vector)
        sok_results.append(embedding_vector)
        # FIXME: when the forward computation is too fast, there
        # may exist some conficts with datareader, which cause the program hang.
        import time
        time.sleep(0.2) # seconds

    return sok_results

def compare_sok_with_tf(args):
    if (args.global_batch_size % args.local_gpu_num != 0):
        raise ValueError("global_batch_size: %d is not divisible by local_gpu_num: %d"
                            %(args.global_batch_size, args.local_gpu_num))
    if (args.global_batch_size % args.worker_num != 0):
        raise ValueError("global_batch_size: %d is not divisible by worker_num: %d"
                            %(args.global_batch_size, args.worker_num))

    # each worker generate different dataset
    if args.generate_new_datas:
        worker_batch_size = args.global_batch_size // args.worker_num
        random_samples_local = utils.generate_random_samples(num_of_samples=worker_batch_size * args.iter_num,
                                                             vocabulary_size=args.local_gpu_num * args.max_vocabulary_size_per_gpu * args.worker_num,
                                                             slot_num=args.slot_num,
                                                             max_nnz=args.max_nnz)
        utils.save_to_file(r"./random_samples_" + str(args.task_id) + r".file", *random_samples_local)
    else:
        random_samples_local = utils.restore_from_file(r"./random_samples_" + str(args.task_id) + r".file")

    # each worker generate same init tensors, because each worker will do the filtering by itself.
    init_tensors = utils.get_ones_tensor(max_vocab_size_per_gpu=args.max_vocabulary_size_per_gpu,
                                        embedding_vec_size=args.embedding_vec_size,
                                        num=args.local_gpu_num * args.worker_num)

    sok_results_local = test_sok_demo(args, init_tensors, *random_samples_local)
    # save the forward embedding vector from different worker to file
    utils.save_to_file(r"./sok_embedding_vectors_" + str(args.task_id) + r".file", *sok_results_local)

    # aggregate dataset from different worker
    dataset_filenames = [r"./random_samples_" + str(task_id) + r".file"
                         for task_id in range(args.worker_num)]
    random_samples_total = [list() for _ in range(args.iter_num)]
    random_labels_total = [list() for _ in range(args.iter_num)]
    local_batch_size = args.global_batch_size // args.worker_num
    for work_id in range(args.worker_num):
        samples, labels = utils.restore_from_file(dataset_filenames[work_id])
        for i in range(args.iter_num):
            random_samples_total[i].extend(samples[i * local_batch_size : (i + 1) * local_batch_size])
            random_labels_total[i].extend(labels[i * local_batch_size : (i + 1) * local_batch_size])
    random_samples_total = np.concatenate(random_samples_total, axis=0)
    random_labels_total = np.concatenate(random_labels_total, axis=0)

    tf_results = test_tf_demo(args, init_tensors, random_samples_total, random_labels_total)

    # aggregate forward embedding vector from different worker
    sok_results_filenames = [r"./sok_embedding_vectors_" + str(task_id) + r".file"
                             for task_id in range(args.worker_num)]
    sok_results_total = list()
    for file_name in sok_results_filenames:
        sok_results_local = utils.restore_from_file(file_name)
        sok_results_total.append(sok_results_local)

    if (len(sok_results_total[0]) != len(tf_results)):
        raise ValueError("The length of results obtained from sok: %d is not equal to that of tensorflow: %d."
                        %(len(sok_results_total[0]), len(tf_results)))
    if (len(tf_results) != args.iter_num):
        raise ValueError("The length of embedding vectors: %d is not equal to iteration number: %d."
                         %(len(tf_results), args.iter_num))

    # for i, sok_vector in enumerate(sok_results_total):
    for i in range(args.iter_num):
        if args.local_gpu_num != 1:
            sok_vector = tf.concat([tf.concat(sok_results_total[task_id][i].values, axis=0)
                                    for task_id in range(args.worker_num)], axis=0)
        else:
            sok_vector = tf.concat([sok_results_total[task_id][i]
                                    for task_id in range(args.worker_num)], axis=0)
        tf.debugging.assert_near(tf.reshape(sok_vector, 
                                            shape=[-1, tf.shape(sok_vector)[-1]]),
                                 tf_results[i],
                                 atol=1e-4,
                                 rtol=1e-4)

    print("\n[INFO]: With MultiWorkerMirroredStrategy, the embedding vector obtained from " +\
          "sparse operation kit and tensorflow are consistent for %d iterations."
          %args.iter_num)

def get_task_id(ips):
    local_ip = utils.get_local_ip()
    for i in range(len(ips)):
        if ips[i] == local_ip:
            return i
    raise ValueError("Cannot find local_ip: %s in ips list: [%s]"
                     %(local_ip, ", ".join(ips)))

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='test demo model with single worker.')
    parser.add_argument('--local_gpu_num', type=int,
                        help='the number of GPUs used to do paralell training.',
                        required=False, default=8)
    parser.add_argument('--iter_num', type=int,
                        help='the number of testing iterations.',
                        required=False, default=100)
    parser.add_argument('--max_vocabulary_size_per_gpu', type=int,
                        required=False, default=128)
    parser.add_argument('--slot_num', type=int,
                        help='the number of feature fields',
                        required=False, default=1)
    parser.add_argument('--max_nnz', type=int,
                        help='the maximum number of keys in one slot',
                        required=False, default=1)
    parser.add_argument('--embedding_vec_size', type=int,
                        help='the dimention of embedding vector',
                        required=False, default=1)
    parser.add_argument('--combiner', type=str,
                        help='the combiner used to do reduction for sparse embedding layer. ' +\
                             'It is only respected in sparse embedding layer.',
                        required=False, default='mean', choices=['mean', 'sum'])
    parser.add_argument('--global_batch_size', type=int, required=False, default=16)
    parser.add_argument('--optimizer', type=str,
                        help="use what optimizer",
                        required=False, default='plugin_adam',
                        choices=['plugin_adam', 'adam', 'sgd'])
    parser.add_argument('--ips', type=str, nargs="+",
                        help="the ip address of each worker.",
                        required=False, default="0.0.0.0")
    parser.add_argument('--generate_new_datas', type=int, choices=[0, 1],
                        help='whether to generate new random samples',
                        required=False, default=1)
    args = parser.parse_args()

    if not isinstance(args.ips, list):
        args.ips = [args.ips]

    args.worker_num = len(args.ips)
    if utils.all_ips_in_local(args.ips):
        print("[INFO]: local_gpu_num will be ignored. The number of available GPUs will be automatically detected.")
        local_gpu_num = utils.get_local_gpu_count()
        if (local_gpu_num % args.worker_num != 0):
            raise ValueError("local_gpu_num: %d is not divisible by worker_num: %d"
                             %(local_gpu_num, args.worker_num))
        per_worker_gpu_num = local_gpu_num // args.worker_num
        args.local_gpu_num = per_worker_gpu_num

        processes = list()
        for task_id in range(args.worker_num):
            available_gpus = ",".join([str(per_worker_gpu_num * task_id + i)
                                       for i in range(per_worker_gpu_num)])
            print("[INFO]: on task: %d, its available GPUs are: %s" %(task_id, available_gpus))
            os.environ["CUDA_VISIBLE_DEVICES"] = available_gpus
            process = utils.TestProcess(func=compare_sok_with_tf, task_id=task_id, arguments=args)
            process.start()
            processes.append(process)

        for process in processes:
            process.join()
    else:
        args.task_id = get_task_id(args.ips)

        os.environ['CUDA_VISIBLE_DEVICES'] = ",".join([str(i) for i in range(args.local_gpu_num)])

        compare_sok_with_tf(args)
    
                        
    