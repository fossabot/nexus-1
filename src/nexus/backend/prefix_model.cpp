#include "nexus/backend/prefix_model.h"
#include "nexus/common/model_db.h"

namespace nexus {
namespace backend {

PrefixModel::PrefixModel(int gpu_id, const ModelInstanceConfig& config) : 
    ModelInstance(gpu_id, config) {
  prefix_length_ = -1;
  std::string model_id = ModelSessionToModelID(model_session_);
  for (int i = 1; i < config.model_session_size(); ++i) {
    std::string other_model_id = ModelSessionToModelID(config.model_session(i));
    int pref_len = ModelDatabase::Singleton().GetSharePrefixLength(
        model_id, other_model_id);
    if (prefix_length_ < 0 || pref_len < prefix_length_) {
      prefix_length_ = pref_len;
    }
  }
  CHECK_GT(prefix_length_, 0) << "No prefix layers shared among models";
  
  ModelInstanceConfig prefix_cfg;
  prefix_cfg.add_model_session()->CopyFrom(model_session_);
  prefix_cfg.set_batch(batch_);
  prefix_cfg.set_max_batch(max_batch_);
  prefix_cfg.set_end_index(prefix_length_);
  prefix_model_ = CreateModelInstance(gpu_id, prefix_cfg);
  for (auto iter : prefix_model_->OutputShapes()) {
    prefix_output_name_ = iter.first;
    prefix_output_shape_ = iter.second;
  }
  prefix_output_arr_ = prefix_model_->GetOutputGpuArrays();

  max_suffix_output_size_ = 0;
  for (int i = 0; i < config.model_session_size(); ++i) {
    ModelInstanceConfig suffix_cfg;
    suffix_cfg.add_model_session()->CopyFrom(config.model_session(i));
    suffix_cfg.set_batch(batch_);
    suffix_cfg.set_max_batch(max_batch_);
    suffix_cfg.set_start_index(prefix_length_);
    suffix_cfg.set_input_name(prefix_output_name_);
    // Don't include batch dim in the shape, so start from 1
    for (uint i = 1; i < prefix_output_shape_.ndims(); ++i) {
      suffix_cfg.add_input_shape(prefix_output_shape_.dim(i));
    }
    //LOG(INFO) << suffix_cfg.DebugString();

    auto suffix_model = CreateModelInstance(gpu_id, suffix_cfg);
    auto model_sess_id = suffix_model->model_session_id();
    suffix_models_.emplace(model_sess_id, suffix_model);
    suffix_input_arrays_.emplace(model_sess_id,
                                 suffix_model->CreateInputGpuArray());
    auto suffix_outputs = suffix_model->OutputShapes();
    CHECK_EQ(suffix_outputs.size(), 1) << "All models must have only one output"
        " in the prefix batching";
    for (auto iter : suffix_outputs) {
      size_t size = iter.second.NumElements(1);
      suffix_output_sizes_.emplace(model_sess_id, size);
      suffix_output_names_.emplace(model_sess_id, iter.first);
      if (size > max_suffix_output_size_) {
        max_suffix_output_size_ = size;
      }
    }
  }

  LOG(INFO) << "Prefix output shape: " << prefix_output_shape_ <<
      ", max suffix output size: " << max_suffix_output_size_;
}

Shape PrefixModel::InputShape() const {
  return prefix_model_->InputShape();
}

std::unordered_map<std::string, Shape> PrefixModel::OutputShapes() const {
  return {{"output", Shape({max_batch_, max_suffix_output_size_})}};
}

ArrayPtr PrefixModel::CreateInputGpuArray() {
  return prefix_model_->CreateInputGpuArray();
}

std::unordered_map<std::string, ArrayPtr> PrefixModel::GetOutputGpuArrays() {
  // Doesn't support in-place output in GPU memory
  return {};
}

void PrefixModel::Preprocess(std::shared_ptr<Task> task) {
  prefix_model_->Preprocess(task);
}

void PrefixModel::Forward(std::shared_ptr<BatchTask> batch_task) {
  uint64_t batch_id = batch_task->batch_id();
  auto suffix_output_arr = batch_task->GetOutputArray("output");

  // Replace the origin output arrays by prefix output GPU array and
  // Forward prefix model
  batch_task->SetOutputArrays(prefix_output_arr_);
  VLOG(1) << "Forward prefix model " << prefix_model_->model_session_id() <<
      " with batch size " << batch_task->batch_size();
  prefix_model_->Forward(batch_task);

  // Append the outputs of prefix model to the input queue of corresponding
  // suffix model
  std::unordered_map<std::string, std::shared_ptr<BatchTask> > suffix_tasks;
  for (auto prefix_output : batch_task->outputs()) {
    auto task = prefix_output->task;
    auto model_sess_id = task->query.model_session_id();
    auto suffix_input = std::make_shared<Input>(
        prefix_output->arrays.at(prefix_output_name_), task,
        prefix_output->index_in_task);
    if (suffix_tasks.find(model_sess_id) == suffix_tasks.end()) {
      auto suffix_task = std::make_shared<BatchTask>(
          batch_id, suffix_models_[model_sess_id]->max_batch());
      suffix_task->SetInputArray(suffix_input_arrays_[model_sess_id]);
      suffix_tasks.emplace(model_sess_id, suffix_task);
    }
    suffix_tasks.at(model_sess_id)->AppendInput(suffix_input);
  }

  // Slice the output array for each suffix model and forward suffix model
  size_t offset = 0;
  std::vector<std::shared_ptr<Output> > suffix_outputs;
  suffix_outputs.reserve(batch_task->batch_size());
  for (auto iter : suffix_tasks) {
    auto model_sess_id = iter.first;
    auto suffix_task = iter.second;
    uint32_t batch = suffix_task->batch_size();
    size_t nfloats = batch * suffix_output_sizes_.at(model_sess_id);
    auto out_arr = suffix_output_arr->Slice(offset, nfloats);
    offset += nfloats;
    suffix_task->SetOutputArrays({{
          suffix_output_names_.at(model_sess_id), out_arr }});
    VLOG(1) << "Forward suffix model " << model_sess_id <<
        " with batch size " << suffix_task->batch_size();
    suffix_models_.at(model_sess_id)->Forward(suffix_task);
    auto outputs = suffix_task->outputs();
    suffix_outputs.insert(suffix_outputs.end(), outputs.begin(), outputs.end());
  }
  // Set suffix outputs into the batch_task outputs
  batch_task->set_outputs(suffix_outputs);
}

void PrefixModel::Postprocess(std::shared_ptr<Task> task) {
  auto suffix_model = suffix_models_.at(task->query.model_session_id());
  suffix_model->Postprocess(task);
}

} // namespace backend
} // namespace nexus
