/*
 * Copyright (c) 2020, NVIDIA CORPORATION.
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

#include <HugeCTR/pybind/optimizer.hpp>

namespace HugeCTR {

OptParamsPy::OptParamsPy() : initialized(false) {}

OptParamsPy::OptParamsPy(Optimizer_t optimizer_type,
                        Update_t update_t,
                        OptHyperParamsPy opt_hyper_params)
  : optimizer(optimizer_type), update_type(update_t), hyperparams(opt_hyper_params), initialized(true) {}

} // namespace HugeCTR