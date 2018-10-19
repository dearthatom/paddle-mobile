/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include <vector>

#include "CL/cl.h"

#include "framework/cl/cl_half.h"
#include "framework/cl/cl_tool.h"
#include "framework/cl/cl_deleter.h"
#include "framework/ddim.h"
#include "framework/tensor.h"

namespace paddle_mobile {
namespace framework {

enum ImageType { Invalid = -1, Normal = 0, Folder = 1 };

class CLImage {
 public:
  CLImage() = default;

  /*
   * will not hold input tensor data, memcpy in this method
   * */
  void SetTensorData(float *tensorData, const DDim &dim) {
    int numel = product(dim);
    if (tensor_data_ != nullptr) {
      delete[](tensor_data_);
      tensor_data_ = nullptr;
    }
    tensor_data_ = new float[numel];
    memcpy(tensor_data_, tensorData, numel * sizeof(float));
    tensor_dims_ = dim;
  }

  /*
   * need call SetTensorData first
   *
   * folder when one dim or two dim
   * */
  void InitCLImage(cl_context context, cl_command_queue command_queue) {
    if (tensor_data_ == nullptr) {
      PADDLE_MOBILE_THROW_EXCEPTION(" need call SetTensorData first");
    }
    DLOG << tensor_dims_;
    if (tensor_dims_.size() <= 2) {
      DLOG << " dim <= 2 folder ~~~~~ ";
      InitCLImage2C(context, command_queue, tensor_data_, tensor_dims_);
    } else {
      DLOG << " dim >  2 norm ~~~~~ ";
      InitCLImage(context, command_queue, tensor_data_, tensor_dims_);
    }
    delete[](tensor_data_);
    tensor_data_ = nullptr;
    initialized_ = true;
  }

  /*
   * need call SetTensorData first
   * */
  void InitCLImageNormal(cl_context context, cl_command_queue command_queue) {
    if (tensor_data_ == nullptr) {
      PADDLE_MOBILE_THROW_EXCEPTION(" need call SetTensorData first");
    }
    InitCLImage(context, command_queue, tensor_data_, tensor_dims_);
    delete[](tensor_data_);
    tensor_data_ = nullptr;
    initialized_ = true;
  }

  void InitEmptyImage(cl_context context, cl_command_queue command_queue,
                      const DDim &dim) {
    if (tensor_data_ != nullptr) {
      PADDLE_MOBILE_THROW_EXCEPTION(
          " empty image tensor data shouldn't have value");
    }
    DLOG << " init empty image ";
    if (tensor_dims_.size() <= 2) {
      DLOG << " dim <= 2 folder ~~~~~ ";
      InitCLImage2C(context, command_queue, tensor_data_, tensor_dims_);
    } else {
      DLOG << " dim >  2 norm ~~~~~ ";
      InitCLImage(context, command_queue, tensor_data_, tensor_dims_);
    }


//    InitCLImage(context, command_queue, nullptr, dim);
    initialized_ = true;
  }

  cl_mem GetCLImage() const { return cl_image_.get(); }

  const DDim &ImageDims() const { return image_dims_; }

  inline size_t ImageWidth() const { return image_width_; }

  inline size_t ImageHeight() const { return image_height_; }

  /*
   * block of channels, 4 channel one block
   * */
  inline size_t CBlock() const { return c_block_; }

  /*
   *  width of original tensor
   * */
  inline size_t WidthOfOneBlock() const { return width_of_one_block_; }

  /*
   *  height of original tensor
   * */
  inline size_t HeightOfOneBlock() const { return height_of_one_block_; }

  inline cl_command_queue CommandQueue() const { return command_queue_; }

  /*
   *  resize original tensor dim
   * */
  inline CLImage &Resize(const DDim &dims) {
    tensor_dims_ = dims;
    return *this;
  }

  template <typename T>
  T *data() const {
    if (initialized_) {
      PADDLE_MOBILE_THROW_EXCEPTION(
          " cl image has initialized, tensor data has been deleted, can't use "
          "tensor data");
    }
    return reinterpret_cast<T *>(tensor_data_);
  }

  /*
   *  numel of tensor dim
   * */
  inline int64_t numel() const { return product(tensor_dims_); }

  /*
   *  original tensor dim
   * */
  const DDim &dims() const { return tensor_dims_; }

  const ImageType GetImageType() const { return image_type_; }

 private:
  ImageType image_type_ = Invalid;
  void InitCLImage2C(cl_context context, cl_command_queue command_queue,
                     float *tensor_data, const DDim &dim) {
    image_type_ = Folder;
    command_queue_ = command_queue;
    assert(dim.size() <= 2);
    int tdim[2] = {1, 1};
    if (dim.size() == 1) {
      tdim[1] = dim[0];
    } else {
      tdim[0] = dim[0];
      tdim[1] = dim[1];
    }
    int width = (tdim[1] + 3) / 4;
    int height = tdim[0];

    image_width_ = width;
    image_height_ = height;
    image_dims_ = make_ddim({width, height});
    width_of_one_block_ = width;
    height_of_one_block_ = height;
    c_block_ = 1;

    std::unique_ptr<half_t[]> imageData{};
    if (tensor_data) {
      imageData.reset(new half_t[width * height * 4]);
      for (int h = 0; h < tdim[0]; h++) {
        for (int w = 0; w < tdim[1]; w++) {
          imageData[(h * width + w / 4) * 4 + (w % 4)] =
              Float2Half(tensor_data[h * tdim[1] + w]);
        }
      }
    }
    InitCLImage(context, width, height, imageData.get());
  }

  void InitCLImage(cl_context context, int width, int height, void *data) {
    cl_image_format cf = {.image_channel_order = CL_RGBA,
                          .image_channel_data_type = CL_HALF_FLOAT};
    cl_image_desc cid = {
        .image_type = CL_MEM_OBJECT_IMAGE2D,
        .image_width = width,
        .image_height = height,
        .image_depth = 1,
        .image_array_size = 1,
        .image_row_pitch = 0,
        .image_slice_pitch = 0,
        .num_mip_levels = 0,
        .num_samples = 0,
        // .buffer = nullptr
    };
    cid.buffer = nullptr;
    cl_int err;
    cl_mem cl_image = clCreateImage(
        context, CL_MEM_READ_WRITE | (data ? CL_MEM_COPY_HOST_PTR : 0),
        &cf,   // const cl_image_format *image_format
        &cid,  // const cl_image_desc *image_desc
        data,  // void *host_ptr
        &err);
    cl_image_.reset(cl_image);
    if (err != CL_SUCCESS) {
      CL_CHECK_ERRORS(err);
      PADDLE_MOBILE_THROW_EXCEPTION(" create image 2d error ");
    }
  }
  void InitCLImage(cl_context context, cl_command_queue command_queue,
                   float *tensor_data, const DDim &dim) {
    image_type_ = Normal;
    DLOG << " tensor dim: " << dim;
    // NCHW -> [W * (C+3)/4, H * N]
    tensor_dims_ = dim;
    command_queue_ = command_queue;
    if (tensor_data) {
      tensor_data_ = tensor_data;
    }
    size_t new_dims[] = {1, 1, 1, 1};

    for (int j = 0; j < dim.size(); ++j) {
      new_dims[4 - dim.size() + j] = dim[j];
    }

    size_t N, C, H, W;

    N = new_dims[0];
    C = new_dims[1];
    H = new_dims[2];
    W = new_dims[3];

    width_of_one_block_ = W;
    height_of_one_block_ = H;

    size_t width = W * ((C + 3) / 4);
    size_t height = H * N;

    image_width_ = width;
    image_height_ = height;
    image_dims_ = make_ddim({image_width_, image_height_});
    c_block_ = width / W;

    DLOG << " tensor dim " << tensor_dims_;
    DLOG << " 赋值时: image width: " << image_width_;
    DLOG << " 赋值时: image height: " << image_height_;

    std::unique_ptr<half_t[]> imageData{};
    int count = 0;
    if (tensor_data != nullptr) {
      imageData.reset(new half_t[width * height * 4]);
      float *p = tensor_data;
      size_t i0 = 0;
      for (int n = 0; n < N; n++) {
        for (int c = 0; c < c_block_ * 4; c++) {
          size_t i1 = i0 + (c / 4) * W;
          for (int h = 0; h < H; h++) {
            size_t i2 = (i1 << 2) + c % 4;
            for (int w = 0; w < W; w++) {
              if (c < C) {
                // int x = (n * width * H + h * width + (c / 4) * W + w) * 4 +
                // (c % 4);
                imageData[i2] = Float2Half(*p);
                i2 += 4;
                p++;
              } else {
                imageData[i2] = 0.0;
                i2 += 4;
              }
            }
            i1 += width;
          }
        }
        i0 += width * H;
      }
    }
    InitCLImage(context, width, height, imageData.get());
  }

  bool initialized_ = false;
  std::unique_ptr<_cl_mem, CLMemDeleter> cl_image_;
  size_t image_width_;
  size_t width_of_one_block_;
  size_t height_of_one_block_;
  size_t image_height_;
  size_t c_block_;
  DDim tensor_dims_;
  DDim image_dims_;
  float *tensor_data_;
  cl_context context_;
  cl_command_queue command_queue_;
};

void TensorToCLImage(Tensor *tensor, CLImage *image,
                     cl_command_queue commandQueue);

void CLImageToTensor(CLImage *image, Tensor *tensor,
                     cl_command_queue commandQueue);

#ifdef PADDLE_MOBILE_DEBUG
Print &operator<<(Print &printer, const CLImage &image);
#endif

}  // namespace framework
}  // namespace paddle_mobile