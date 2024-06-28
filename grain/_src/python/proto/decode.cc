/* Copyright 2023 Google LLC. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "third_party/absl/strings/string_view.h"
#include "third_party/protobuf/map.h"
#include "third_party/protobuf/repeated_field.h"
#include "third_party/protobuf/repeated_ptr_field.h"
#include "third_party/pybind11/include/pybind11/cast.h"
#include "third_party/pybind11/include/pybind11/detail/common.h"
#include "third_party/pybind11/include/pybind11/gil.h"
#include "third_party/pybind11/include/pybind11/numpy.h"
#include "third_party/pybind11/include/pybind11/pybind11.h"
#include "third_party/pybind11/include/pybind11/pytypes.h"
#include "third_party/pybind11/include/pybind11/stl.h"  // IWYU pragma: keep (necessary for py::cast from std::vector)
#include "third_party/tensorflow/core/example/example.proto.h"
#include "third_party/tensorflow/core/example/feature.proto.h"

namespace py = ::pybind11;

namespace {

// Converts a repeated proto field into a np.ndarray.
template <typename T,
          std::enable_if_t<
              std::is_same_v<T, int64_t> || std::is_same_v<T, float>, int> = 0>
py::array_t<T> ConvertToNpArray(proto2::RepeatedField<T> input) {
  auto values = std::make_unique<proto2::RepeatedField<T>>(std::move(input));
  int size = values->size();
  typename proto2::RepeatedField<T>::const_pointer data_ptr = values->data();
  py::capsule values_capsule(
      /*value=*/values.get(), /*name=*/nullptr,
      /*destructor=*/[](void* p) {
        delete reinterpret_cast<proto2::RepeatedField<T>*>(p);
      });
  // The data will be freed once `values_capsule` is destroyed.
  values.release();
  py::array_t<T> np_array(size, data_ptr, values_capsule);
  return np_array;
}

// Create a NumPy array directly in C++ and move it up to python land. Only
// copy the data from the repeated field once, and directly into the NumPy
// array memory; avoid copying twice. Since NumPy stores bytes and strings with
// constant and equal size, and padds the ones that are smaller than max, we do
// that here manually instead of letting NumPy do it on-copy.
py::array ConvertToNpArray(absl::Span<const std::string* const> input) {
  int max_len =
      (*absl::c_max_element(input, [](const auto& lhs, const auto& rhs) {
        return lhs->size() < rhs->size();
      }))->size();
  int array_len = input.size() * max_len + 1;
  auto data = std::shared_ptr<char[]>(new char[array_len]);
  for (int i = 0; i < input.size(); ++i) {
    std::strncpy(data.get() + max_len * i, input[i]->data(), max_len);
  }
  return py::array(py::dtype(absl::StrCat("S", max_len)), {input.size()},
                   {max_len}, data.get());
}

}  // namespace

PYBIND11_MODULE(decode, m) {
  m.doc() =
      "Decode a `tensorflow.Example` protobuf into a dict of numpy arrays.";
  m.def(
      "parse_tf_example",
      [](absl::string_view serialized_proto) -> py::dict {
        tensorflow::Example example;
        {
          py::gil_scoped_release release;
          example.ParseFromString(serialized_proto);
        }
        proto2::Map<std::string, tensorflow::Feature>& features =
            *(example.mutable_features()->mutable_feature());
        py::dict result;
        for (auto& [feature_name, feature] : features) {
          py::array np_array;
          switch (feature.kind_case()) {
            case tensorflow::Feature::kInt64List:
              np_array = ConvertToNpArray<int64_t>(
                  std::move(*feature.mutable_int64_list()->mutable_value()));
              break;
            case tensorflow::Feature::kFloatList:
              np_array = ConvertToNpArray<float>(
                  std::move(*feature.mutable_float_list()->mutable_value()));
              break;
            case tensorflow::Feature::kBytesList:
              np_array = ConvertToNpArray(
                  *feature.mutable_bytes_list()->mutable_value());
              break;
            case tensorflow::Feature::KIND_NOT_SET:
              continue;
            default:
              throw py::value_error("Unexpected feature type");
          }
          result[py::str(feature_name)] = np_array;
        }
        return result;
      },
      py::return_value_policy::take_ownership);
}
