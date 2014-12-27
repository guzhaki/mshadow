/*!
 *  Copyright (c) 2014 by Contributors
 * \file tensor_cpu-inl.h
 * \brief implementation of CPU host code
 * \author Bing Xu, Tianqi Chen
 */
#ifndef MSHADOW_TENSOR_GPU_INL_H_
#define MSHADOW_TENSOR_GPU_INL_H_
#include "./base.h"
#include "./tensor.h"

namespace mshadow {
#if !(MSHADOW_USE_CUDA)
// do nothing if no GPU operation is involved
inline void InitTensorEngine(int dev_id) {
}
inline void ShutdownTensorEngine(void) {
}
#else
#if (MSHADOW_USE_NVML)
inline int AutoSelectDevice(int device_count) {
  // TODO(bing): nvml device id and cuda device id are not consistent
  return 0;
}
#endif
inline void InitTensorEngine(int dev_id) {
  cudaDeviceProp prop;
  int device_id = 0;
  int device_count = 0;
  cudaGetDeviceCount(&device_count);
  utils::Check(device_count > 0,
               "Cannot find CUDA device. Please check CUDA-Configuration");
  if (dev_id < 0) {
#if (MSHADOW_USE_NVML)
    device_id = AutoSelectDevice(device_count);
#endif
  } else {
    device_id = dev_id;
  }
  utils::Check(device_id < device_count, "Incorrect Device ID");
  utils::Check(cudaSetDevice(device_id) == cudaSuccess, "cannot set device");
  cudaGetDeviceProperties(&prop, device_id);
  printf("Use CUDA Device %d: %s\n", device_id, prop.name);
  cublasInit();
}
inline void ShutdownTensorEngine(void) {
  cublasShutdown();
}
template<int dim, typename DType>
inline void AllocSpace(Tensor<gpu, dim, DType> *obj, bool pad) {
  size_t pitch;
  // common choice for cuda mem align unit is 32
  if (pad && obj.size(dim - 1) >= MSHADOW_MIN_PAD_RATIO * 32) {
    cudaError_t err =
        cudaMallocPitch(reinterpret_cast<void**>(&obj.dptr_), &pitch,
                        obj->size(dim - 1) * sizeof(DType),
                        obj->shape_.FlatTo2D()[0]);
    utils::Check(err == cudaSuccess, cudaGetErrorString(err));
    obj->stride_ = static_cast<index_t>(pitch / sizeof(DType));
  } else {
    obj->stride_ = obj->size(dim - 1);
    cudaError_t err =
        cudaMallocPitch(reinterpret_cast<void**>(&obj.dptr_), &pitch,
                        obj->shape_.Size() * sizeof(DType), 1);
    utils::Check(err == cudaSuccess, cudaGetErrorString(err));
  }
}
template<int dim, typename DType>
inline void FreeSpace(Tensor<gpu, dim, DType> *obj) {
  cudaFree(obj->dptr_); obj->dptr = NULL;
}
template<typename A, typename B, int dim, typename DType>
inline void Copy(Tensor<A, dim, DType> _dst,
                 Tensor<B, dim, DType> _src,
                 cudaMemcpyKind kind) {
  utils::Check(_dst.shape_ == _src.shape_, "Copy:shape mismatch");
  Tensor<A, DType, 2> dst = _dst.FlatTo2D();
  Tensor<B, DType, 2> src = _src.FlatTo2D();
  cudaError_t err = cudaMemcpy2D(dst.dptr_, dst.stride_ * sizeof(DType),
                                 src.dptr_, src.stride_ * sizeof(DType),
                                 dst.size(1) * sizeof(DType),
                                 dst.size(0), kind);
  utils::Check(err == cudaSuccess, cudaGetErrorString(err));
}
template<int dim, typename DType>
inline void Copy(Tensor<cpu, dim, DType> dst,
                 const Tensor<gpu, dim, DType> &src) {
  Copy(dst, src, cudaMemcpyDeviceToHost);
}
template<int dim, typename DType>
inline void Copy(Tensor<gpu, dim, DType> dst,
                 const Tensor<gpu, dim, DType> &src) {
  Copy(dst, src, cudaMemcpyDeviceToDevice);
}
template<int dim, typename DType>
inline void Copy(Tensor<gpu, dim, DType> dst,
                 const Tensor<cpu, dim, DType> &src) {
  Copy(dst, src, cudaMemcpyHostToDevice);
}
// the following part is included only if compiler is nvcc
#ifdef __CUDACC__
#include "./cuda/tensor_gpu-inl.cuh"

template<typename Saver, typename R, int dim,
         typename DType, typename E, int etype>
inline void MapExp(TRValue<R, gpu, dim, DType> *dst,
                   const expr::Exp<E, DType, etype> &exp) {
  expr::TypeCheckPass<expr::TypeCheck<gpu, dim, DType, E>::kMapPass>
      ::Error_All_Tensor_in_Exp_Must_Have_Same_Type();
  Shape<dim> eshape = expr::ShapeCheck<dim, E>::Check(exp.self());
  utils::Check(eshape[0] == 0 || eshape == dst->self().shape_,
               "Assignment: Shape of Tensors are not consistent with target");
  cuda::MapPlan<Saver>(MakePlan(dst->self()),
                       MakePlan(exp.self()),
                       dst->shape_.FlatTo2D());
}

template<typename Saver, typename Reducer,
         typename R, typename DType, typename E, int etype>
inline void MapReduceKeepLowest(TRValue<R, cpu, 1, DType> *dst,
                                const expr::Exp<E, DType, etype> &exp,
                                DType scale) {
  expr::TypeCheckPass<expr::TypeCheck<gpu, 1, DType, E>::kRedPass>
      ::Error_TypeCheck_Not_Pass_For_Reduce_Exp();
  Shape<2> eshape = expr::ShapeCheck<expr::ExpInfo<E>::kDim, E>
      ::Check(exp.self()).FlatTo2D();
  utils::Check(eshape[1] == dst->self().size(0),
               "MapReduceKeepLowest::reduction dimension do not match");
  utils::Check(eshape[0] != 0, "can not reduce over empty tensor");
  cuda::MapReduceKeepLowest<Saver, Reducer>
      (MakePlan(dst->self()), MakePlan(exp.self()), scale, eshape);
}

template<typename Saver, typename Reducer, int dimkeep,
         typename R, typename DType, typename E, int etype>
inline void MapReduceKeepHighDim(TRValue<R, cpu, 1, DType> *dst,
                                 const expr::Exp<E, DType, etype> &exp,
                                 DType scale) {
  expr::TypeCheckPass<expr::TypeCheck<gpu, dimkeep, DType, E>::kRedPass>
      ::Error_TypeCheck_Not_Pass_For_Reduce_Exp();
  typedef Shape<expr::ExpInfo<E>::kDim> EShape;
  EShape eshape = expr::ShapeCheck<expr::ExpInfo<E>::kDim, E>
      ::Check(exp.self());
  utils::Check(eshape[dimkeep] == dst->self().size(0),
               "MapReduceKeepHighDim::reduction dimension do not match");
  // use equvalent form
  Shape<4> pshape = Shape4(eshape.ProdShape(0, dimkeep),
                           eshape[dimkeep],
                           eshape.ProdShape(dimkeep, EShape::kSubdim),
                           eshape[EShape::kSubdim]);
  // call equavalent map red dim 2
  cuda::MapReduceKeepDim2<Saver, Reducer>
      (MakePlan(dst->self()), MakePlan(exp.self()), scale, pshape);
}
template<typename DType>
inline void Softmax(Tensor<gpu, 2, DType> dst,
                    const Tensor<gpu, 2, DType>& src) {
  cuda::Softmax(dst, src);
}
#endif  // __CUDACC__
#endif  // MSHADOW_USE_CUDA
}  // namespace mshadow
#endif  // MSHADOW_TENSOR_GPU_INL_H_
