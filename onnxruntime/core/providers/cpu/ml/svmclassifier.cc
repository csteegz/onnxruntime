// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/cpu/ml/svmclassifier.h"

namespace onnxruntime {
namespace ml {

#define ADD_IN_TYPE_SVM_CLASSIFIER_OP(in_type)                                                                                                                                                    \
  ONNX_CPU_OPERATOR_TYPED_ML_KERNEL(                                                                                                                                                              \
      SVMClassifier,                                                                                                                                                                              \
      1,                                                                                                                                                                                          \
      in_type,                                                                                                                                                                                    \
      KernelDefBuilder().TypeConstraint("T1", DataTypeImpl::GetTensorType<in_type>()).TypeConstraint("T2", {DataTypeImpl::GetTensorType<int64_t>(), DataTypeImpl::GetTensorType<std::string>()}), \
      SVMClassifier<in_type>);

ADD_IN_TYPE_SVM_CLASSIFIER_OP(float);
ADD_IN_TYPE_SVM_CLASSIFIER_OP(double);
ADD_IN_TYPE_SVM_CLASSIFIER_OP(int64_t);
ADD_IN_TYPE_SVM_CLASSIFIER_OP(int32_t);

template <typename T>
SVMClassifier<T>::SVMClassifier(const OpKernelInfo& info)
    : OpKernel(info),
      SVMCommon<T>(info),
      vectors_per_class_(info.GetAttrsOrDefault<int64_t>("vectors_per_class")),
      proba_(info.GetAttrsOrDefault<float>("prob_a")),
      probb_(info.GetAttrsOrDefault<float>("prob_b")),
      support_vectors_(info.GetAttrsOrDefault<float>("support_vectors")),
      post_transform_(MakeTransform(info.GetAttrOrDefault<std::string>("post_transform", "NONE"))) {
  ORT_ENFORCE(info.GetAttrs<float>("rho", rho_).IsOK());
  ORT_ENFORCE(info.GetAttrs<float>("coefficients", coefficients_).IsOK());

  // prob_a and prob_b are optional for Z output
  ORT_ENFORCE(proba_.size() == probb_.size());

  // one of these should be valid
  ORT_ENFORCE(info.GetAttrs<std::string>("classlabels_strings", classlabels_strings_).IsOK() ||
              info.GetAttrs<int64_t>("classlabels_ints", classlabels_ints_).IsOK());

  vector_count_ = 0;
  feature_count_ = 0;
  class_count_ = 0;
  for (int64_t i = 0; i < static_cast<int64_t>(vectors_per_class_.size()); i++) {
    starting_vector_.push_back(vector_count_);
    vector_count_ += vectors_per_class_[i];
  }

  using_strings_ = false;
  if (!classlabels_strings_.empty()) {
    using_strings_ = true;
    class_count_ = classlabels_strings_.size();
  } else if (!classlabels_ints_.empty()) {
    class_count_ = classlabels_ints_.size();
  } else {
    class_count_ = 1;
  }
  if (vector_count_ > 0) {
    feature_count_ = support_vectors_.size() / vector_count_;  //length of each support vector
    mode_ = SVM_TYPE::SVM_SVC;
  } else {
    feature_count_ = coefficients_.size() / class_count_;  //liblinear mode
    mode_ = SVM_TYPE::SVM_LINEAR;
    set_kernel_type(KERNEL::LINEAR);
  }
  ORT_ENFORCE(!classlabels_strings_.empty() || !classlabels_ints_.empty());
  ORT_ENFORCE(proba_.size() == probb_.size());
  ORT_ENFORCE(!coefficients_.empty());
  weights_are_all_positive_ = true;
  for (int64_t i = 0; i < static_cast<int64_t>(coefficients_.size()); i++) {
    if (coefficients_[i] < 0) {
      weights_are_all_positive_ = false;
      break;
    }
  }
}

template <typename T>
Status SVMClassifier<T>::Compute(OpKernelContext* ctx) const {
  const Tensor* X = ctx->Input<Tensor>(0);

  int64_t stride = X->Shape().NumDimensions() == 1 ? X->Shape()[0] : X->Shape()[1];
  int64_t N = X->Shape().NumDimensions() == 1 ? 1 : X->Shape()[0];

  Tensor* Y = ctx->Output(0, TensorShape({N}));
  Tensor* Z;

  std::vector<int64_t> dims;
  if (mode_ == SVM_TYPE::SVM_SVC && proba_.empty())
    dims = {static_cast<int64_t>(N), static_cast<int64_t>(class_count_ * (class_count_ - 1) / 2)};
  else
    dims = {static_cast<int64_t>(N), static_cast<int64_t>(class_count_)};
  Z = ctx->Output(1, TensorShape(dims));

  const auto* x_data = X->template Data<T>();
  int64_t zindex = 0;

  for (int64_t n = 0; n < N; n++)  //for each example
  {
    int64_t current_weight_0 = n * stride;
    int64_t maxclass = -1;
    double maxweight = 0.f;
    std::vector<float> decisions;
    std::vector<float> scores;
    std::vector<float> kernels;
    std::vector<int64_t> votes;

    if (mode_ == SVM_TYPE::SVM_SVC) {
      for (int64_t j = 0; j < vector_count_; j++) {
        float val = kernel_dot(x_data, current_weight_0, support_vectors_, feature_count_ * j, feature_count_, get_kernel_type());
        kernels.push_back(val);
      }
      for (int64_t j = 0; j < class_count_; j++) {
        votes.push_back(0);
      }
      int evals = 0;
      for (int64_t i = 0; i < class_count_; i++) {        //for each class
        for (int64_t j = i + 1; j < class_count_; j++) {  //for each class
          float sum = 0;
          int64_t start_index_i = starting_vector_[i];  // *feature_count_;
          int64_t start_index_j = starting_vector_[j];  // *feature_count_;

          int64_t class_i_support_count = vectors_per_class_[i];
          int64_t class_j_support_count = vectors_per_class_[j];

          int64_t pos1 = (vector_count_) * (j - 1);
          int64_t pos2 = (vector_count_) * (i);
          for (int64_t m = 0; m < class_i_support_count; m++) {
            float val1 = coefficients_[pos1 + start_index_i + m];
            float val2 = kernels[start_index_i + m];
            sum += val1 * val2;
          }
          for (int64_t m = 0; m < class_j_support_count; m++) {
            float val1 = coefficients_[pos2 + start_index_j + m];
            float val2 = kernels[start_index_j + m];
            sum += val1 * val2;
          }

          sum += rho_[evals];
          scores.push_back(sum);
          if (sum > 0) {
            votes[i]++;
          } else {
            votes[j]++;
          }
          evals++;  //index into rho
        }
      }
    } else if (mode_ == SVM_TYPE::SVM_LINEAR) {     //liblinear
      for (int64_t j = 0; j < class_count_; j++) {  //for each class
        float val = kernel_dot(x_data, current_weight_0, coefficients_, feature_count_ * j, feature_count_, get_kernel_type());
        val += rho_[0];
        scores.push_back(val);
      }
    }
    if (!proba_.empty() && mode_ == SVM_TYPE::SVM_SVC) {
      //compute probabilities from the scores
      std::vector<float> estimates;
      std::vector<float> probsp2;
      int64_t num = class_count_ * class_count_;
      for (int64_t m = 0; m < num; m++) {
        probsp2.push_back(0.f);  //min prob
      }
      for (int64_t m = 0; m < class_count_; m++) {
        estimates.push_back(0.f);  //min prob
      }
      int64_t index = 0;
      for (int64_t i = 0; i < class_count_; i++) {
        for (int64_t j = i + 1; j < class_count_; j++) {
          float val1 = sigmoid_probability(scores[index], proba_[index], probb_[index]);
          float val2 = std::max(val1, 1.0e-7f);
          probsp2[i * class_count_ + j] = std::min(val2, 1 - 1.0e-7f);
          probsp2[j * class_count_ + i] = 1 - probsp2[i * class_count_ + j];
          index++;
        }
      }
      multiclass_probability(class_count_, probsp2, estimates);
      //copy probabilities back into scores
      scores.resize(estimates.size());
      for (int64_t k = 0; k < static_cast<int64_t>(estimates.size()); k++) {
        scores[k] = estimates[k];
      }
    }
    int64_t maxvotes = 0;
    if (!votes.empty()) {
      for (int64_t k = 0; k < static_cast<int64_t>(votes.size()); k++) {
        if (votes[k] > maxvotes) {
          maxvotes = votes[k];
          maxclass = k;
        }
      }
    } else {
      for (int64_t k = 0; k < static_cast<int64_t>(scores.size()); k++) {
        if (scores[k] > maxweight) {
          maxclass = k;
          maxweight = scores[k];
        }
      }
    }
    //write top class
    int write_additional_scores = -1;
    if (rho_.size() == 1)  //binary
    {
      if (using_strings_) {
        if (classlabels_strings_.size() == 2 && weights_are_all_positive_ && maxweight >= 0.5 && proba_.empty()) {
          Y->template MutableData<std::string>()[n] = classlabels_strings_[1];  //positive label
          write_additional_scores = 0;
        } else if (classlabels_strings_.size() == 2 && maxweight > 0 && !weights_are_all_positive_ && proba_.empty()) {
          Y->template MutableData<std::string>()[n] = classlabels_strings_[1];  //positive label
          write_additional_scores = 0;
        } else if (classlabels_strings_.size() == 2 &&
                   !proba_.empty()) {  // this case all classes are in their rightful spot
          Y->template MutableData<std::string>()[n] = classlabels_strings_[maxclass];  //whichever label
          write_additional_scores = -1;
        } else if (classlabels_strings_.size() == 2) {
          Y->template MutableData<std::string>()[n] = classlabels_strings_[0];  //negative label
          write_additional_scores = 1;
        } else if (maxweight > 0) {
          Y->template MutableData<std::string>()[n] = "1";  //positive label
        } else {
          Y->template MutableData<std::string>()[n] = "0";  //negative label
        }
      } else  //no strings
      {
        if (classlabels_ints_.size() == 2 && weights_are_all_positive_ && maxweight >= 0.5 && proba_.empty()) {
          Y->template MutableData<int64_t>()[n] = classlabels_ints_[1];  //positive label
          write_additional_scores = 0;
        } else if (classlabels_ints_.size() == 2 && maxweight > 0 && !weights_are_all_positive_ && proba_.empty()) {
          Y->template MutableData<int64_t>()[n] = classlabels_ints_[0];  //pos  label
          write_additional_scores = 0;
        } else if (classlabels_ints_.size() == 2 &&
                   !proba_.empty())  // this case all classes are in their rightful spot
        {
          Y->template MutableData<int64_t>()[n] = classlabels_ints_[maxclass];  //whichever label
          write_additional_scores = -1;
        } else if (classlabels_ints_.size() == 2) {
          Y->template MutableData<int64_t>()[n] = classlabels_ints_[0];  //negative label
          write_additional_scores = 1;
        } else if (maxweight > 0) {
          Y->template MutableData<int64_t>()[n] = 1;  //positive label
        } else {
          Y->template MutableData<int64_t>()[n] = 0;  //negative label
        }
      }
    } else {  //multiclass
      if (using_strings_) {
        Y->template MutableData<std::string>()[n] = classlabels_strings_[maxclass];
      } else {
        Y->template MutableData<int64_t>()[n] = classlabels_ints_[maxclass];
      }
    }

    write_scores(scores, post_transform_, zindex, Z, write_additional_scores);
    zindex += scores.size();
  }

  return Status::OK();
}

}  // namespace ml
}  // namespace onnxruntime
