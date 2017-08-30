/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                           License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2017, Intel Corporation, all rights reserved.
// Copyright (c) 2016-2017 Fabian David Tschopp, all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include "../../precomp.hpp"
#include "common.hpp"

#ifdef HAVE_OPENCL
#include "math_functions.hpp"
#include <vector>
#include "opencl_kernels_dnn.hpp"

using namespace cv;

struct gemm_callback_arg {
    std::vector<cl_event> evs;
    std::vector<cl_mem> imgs;
};

static void CL_CALLBACK gemmCallback(cl_event event,
                                     cl_int event_command_exec_status,
                                     void *user_data)
{
    struct gemm_callback_arg *arg = (struct gemm_callback_arg *) user_data;
    for(int i = 0; i < arg->evs.size(); i++)
    {
        clReleaseEvent(arg->evs[i]);
    }

    for(int i = 0; i < arg->imgs.size(); i++)
    {
        clReleaseMemObject(arg->imgs[i]);
    }
    delete arg;
}

// Create and copy buffer to image for GEMM's matrix A and B.
// Will return image to caller if the input image is NULL. Otherwise,
// will use the image directly. It's caller's responsibility to
// release the created image.
template<typename Dtype>
void ocl4dnnGEMMCopyBufferToImage(int32_t ctx_id,
                                  cl_mem *image, cl_mem buffer, int offset,
                                  bool is_matrix_a, bool transpose,
                                  bool padding, int padded_height,
                                  int padded_width, int height,
                                  int width, int ld, int wait_list_size,
                                  cl_event *wait_list,
                                  cl_event *event)
{
    ocl::Context ctx = ocl::Context::getDefault();
    ocl::Queue queue = ocl::Queue::getDefault();
    cl_image_desc desc;
    cl_image_format format;

    memset(&desc, 0, sizeof(desc));
    int src_offset = sizeof(Dtype) * offset;
    if (!is_matrix_a && transpose)
    {
        // For matrix B with transpose, we need to handle them differently.
        // As we can't use the sub group block read to get a row easily,
        // we have to use CL_FLOAT type with read_imagef to get the row.
        cl_int err;
        format.image_channel_data_type = CL_FLOAT;
        desc.image_type = CL_MEM_OBJECT_IMAGE2D;
        desc.image_width = width;
        format.image_channel_order = CL_R;

        desc.image_height = height;
        if (*image == NULL)
        {
            *image = clCreateImage((cl_context)ctx.ptr(),
                                   CL_MEM_READ_WRITE,
                                   &format,
                                   &desc,
                                   NULL,
                                   &err);
            OCL_CHECK(err);
        }

        if (ld == width)
        {
            size_t origin[] = {0, 0, 0};
            size_t region[] = {(size_t)desc.image_width, (size_t)desc.image_height, 1};

            OCL_CHECK(clEnqueueCopyBufferToImage((cl_command_queue)queue.ptr(),
                                                 buffer, *image, src_offset,
                                                 origin, region, wait_list_size,
                                                 wait_list, event));
        } else {
            ocl::Kernel oclk_gemm_copy("gemm_buffer_copy_image_transpose_float", ocl::dnn::gemm_image_oclsrc);

            size_t global_copy[2];
            global_copy[0] = width;
            global_copy[1] = height;
            oclk_gemm_copy.set(0, (cl_mem) buffer);
            oclk_gemm_copy.set(1, (cl_mem) *image);
            oclk_gemm_copy.set(2, offset);
            oclk_gemm_copy.set(3, width);
            oclk_gemm_copy.set(4, height);
            oclk_gemm_copy.set(5, ld);
            OCL_CHECK(clEnqueueNDRangeKernel((cl_command_queue)queue.ptr(),
                                             (cl_kernel)oclk_gemm_copy.ptr(),
                                             2, NULL, global_copy, NULL,
                                             wait_list_size, wait_list,
                                             event));
        }
    } else {
        if (*image == NULL)
        {
            desc.image_type = CL_MEM_OBJECT_IMAGE2D;
            format.image_channel_data_type = CL_UNSIGNED_INT8;
            format.image_channel_order = CL_RGBA;

            if (!padding)
            {
                desc.image_width = width;
                desc.image_height = height;
            } else {
                desc.image_width = padded_width;
                desc.image_height = padded_height;
            }
            cl_int err;
            *image = clCreateImage((cl_context)ctx.ptr(),
                                   desc.buffer ? CL_MEM_READ_ONLY : CL_MEM_READ_WRITE,
                                   &format,
                                   &desc,
                                   NULL,
                                   &err);
            OCL_CHECK(err);
        }
        if (!padding && desc.buffer != NULL)
            return;
        if (!padding && desc.buffer == NULL)
        {
            // copy without padding.
            size_t origin[] = {0, 0, 0};
            size_t region[] = {(size_t)width, (size_t)height, 1};
            OCL_CHECK(clEnqueueCopyBufferToImage((cl_command_queue)queue.ptr(),
                                                 buffer, *image, src_offset,
                                                 origin, region, wait_list_size,
                                                 wait_list, event));
        } else {
            ocl::Kernel oclk_gemm_copy("gemm_buffer_copy_image_no_transpose_float",
                                       ocl::dnn::gemm_image_oclsrc);

            size_t global_copy[2];
            global_copy[0] = padding ? padded_width : width;
            global_copy[1] = padding ? padded_height : height;
            oclk_gemm_copy.set(0, (cl_mem) buffer);
            oclk_gemm_copy.set(1, (cl_mem) *image);
            oclk_gemm_copy.set(2, offset);
            oclk_gemm_copy.set(3, width);
            oclk_gemm_copy.set(4, height);
            oclk_gemm_copy.set(5, ld);
            OCL_CHECK(clEnqueueNDRangeKernel((cl_command_queue)queue.ptr(),
                                             (cl_kernel)oclk_gemm_copy.ptr(),
                                             2, NULL, global_copy, NULL,
                                             wait_list_size, wait_list,
                                             event));
        }
    }
}

template
void ocl4dnnGEMMCopyBufferToImage<float>(int32_t ctx_id,
                                         cl_mem *image, cl_mem buffer, int offset,
                                         bool is_matrix_a, bool transpose,
                                         bool padding, int padded_height,
                                         int padded_width, int height,
                                         int width,  int ld, int wait_list_size,
                                         cl_event *wait_list,
                                         cl_event *event);

enum gemm_type_t
{
    GEMM_TYPE_NONE = 0,
    GEMM_TYPE_FAST_IMAGE_32_1,
    GEMM_TYPE_FAST_IMAGE_32_2,
    GEMM_TYPE_FAST_IMAGE_B_IMAGE,
    GEMM_TYPE_FAST_BUFFER,
    GEMM_TYPE_MAX
};

template<typename Dtype>
static void ocl4dnnFastImageGEMM(const int32_t ctx_id,
                                 const CBLAS_TRANSPOSE TransA,
                                 const CBLAS_TRANSPOSE TransB, const int32_t M,
                                 const int32_t N, const int32_t K, const Dtype alpha,
                                 const cl_mem A, const int32_t offA, const cl_mem B,
                                 const int32_t offB, const Dtype beta, cl_mem C,
                                 const int32_t offC, bool is_image_a, bool is_image_b,
                                 enum gemm_type_t gemm_type,
                                 const size_t max_image_size)
{
    CHECK_EQ(gemm_type == GEMM_TYPE_FAST_IMAGE_32_1 || gemm_type == GEMM_TYPE_FAST_IMAGE_32_2 ||
             gemm_type == GEMM_TYPE_FAST_IMAGE_B_IMAGE, true) << "Invalid fast image gemm type." << std::endl;

    if (is_image_a)
        CHECK_EQ(offA, 0) << "Invalid input image offset." << std::endl;

    if (is_image_b)
        CHECK_EQ(offB, 0) << "Invalid input image offset." << std::endl;

    int widthA = (TransA == CblasNoTrans) ? K : M;
    int heightA = (TransA == CblasNoTrans) ? M : K;
    int widthB = (TransB == CblasNoTrans) ? N : K;
    int heightB = (TransB == CblasNoTrans) ? K : N;

    int ldA = widthA;
    int ldB = widthB;
    int ldC = N;

    int A_start_x = 0, A_start_y = 0, B_start_x = 0;
    int B_start_y = 0, C_start_x = 0, C_start_y = 0;
    int blocksize = 1024;
    if (gemm_type == GEMM_TYPE_FAST_IMAGE_B_IMAGE)
        blocksize = max_image_size;
    int blockA_width = blocksize;
    int blockA_height = blocksize;
    int blockB_width = blocksize;
    int blockB_height = blocksize;
    int blockC_width = blocksize;
    int blockC_height = blocksize;

    int use_buffer_indicator = 8;
    // To fix the edge problem casued by the sub group block read.
    // we have to pad the image if it's not multiple of tile.
    // just padding one line is enough as the sub group block read
    // will clamp to edge according to the spec.

    ocl::Context ctx = ocl::Context::getDefault();
    ocl::Queue queue = ocl::Queue::getDefault();

    cl_mem ImA = NULL;
    cl_mem ImB = NULL;

    std::string kernel_name("gemm_");
    if (gemm_type == GEMM_TYPE_FAST_IMAGE_32_1 || gemm_type == GEMM_TYPE_FAST_IMAGE_B_IMAGE)
        kernel_name += "32_1_";
    else
        kernel_name += "32_2_";

    if (TransA == CblasNoTrans)
        kernel_name += "N";
    else
        kernel_name += "T";

    if (TransB == CblasNoTrans)
    {
        kernel_name += "N_";
    } else {
        kernel_name += "T_";
        if (is_image_b || (K % use_buffer_indicator != 0))
        {
            kernel_name += "SCALAR_";
        } else {
            kernel_name += "BUFFER_";
        }
    }

    if (alpha == 1)
        kernel_name += "1_";
    else
        kernel_name += "0_";

    if (beta == 0)
        kernel_name += "0";
    else
        kernel_name += "1";

    kernel_name += "_float";

    ocl::Kernel oclk_gemm_float(kernel_name.c_str(), ocl::dnn::gemm_image_oclsrc);
    while (C_start_y < M)
    {
        blockC_width = std::min(static_cast<int>(N) - C_start_x, blocksize);
        blockC_height = std::min(static_cast<int>(M) - C_start_y, blocksize);

        int isFirstColBlock = 1;
        for (int k = 0; k < K; k += blocksize)
        {
            cl_event ev[5];
            cl_uint ev_idx = 0;
            memset(ev, 0, sizeof(cl_event) * 5);
            struct gemm_callback_arg * arg = new gemm_callback_arg;

            blockA_width = std::min(widthA - A_start_x, blocksize);
            blockA_height = std::min(heightA - A_start_y, blocksize);
            blockB_width = std::min(widthB - B_start_x, blocksize);
            blockB_height = std::min(heightB - B_start_y, blocksize);
            int block_Ksize = std::min(static_cast<int>(K) - k, blocksize);

            int padded_k = block_Ksize + ((block_Ksize & 7) ? (8 - (block_Ksize & 7)) : 0);
            int imageA_w = (TransA == CblasNoTrans) ? padded_k : blockA_width;
            int imageA_h = (TransA == CblasNoTrans) ? blockA_height : padded_k;
            int imageB_w = (TransB == CblasNoTrans) ? blockB_width : padded_k;
            int imageB_h = (TransB == CblasNoTrans) ? padded_k : blockB_height;

            int blockA_offset = offA + A_start_y * ldA + A_start_x;
            int blockB_offset = offB + B_start_y * ldB + B_start_x;
            int blockC_offset = offC + C_start_y * ldC + C_start_x;
            if (TransB == CblasNoTrans)
            {
                bool padding_A = false;
                bool padding_B = false;

                if (!is_image_a && !is_image_b)
                {
                    if (M * K < N * K)
                        padding_B = true;
                    else
                        padding_A = true;
                }

                if (!is_image_a)
                {
                    ocl4dnnGEMMCopyBufferToImage<Dtype>(ctx_id, &ImA,
                                                        A, blockA_offset,
                                                        true, TransA != CblasNoTrans,
                                                        padding_A, imageA_h, imageA_w,
                                                        blockA_height, blockA_width, ldA, 0,
                                                        NULL, &ev[ev_idx]);
                    if (ev[ev_idx] != NULL)
                        ev_idx++;
                }
                if (!is_image_b)
                {
                    ocl4dnnGEMMCopyBufferToImage<Dtype>(ctx_id, &ImB,
                                                        B, blockB_offset,
                                                        false, false,
                                                        padding_B, imageB_h, imageB_w,
                                                        blockB_height, blockB_width, ldB,
                                                        0, NULL, &ev[ev_idx]);
                    if (ev[ev_idx] != NULL)
                        ev_idx++;
                }
            } else {
                // We will use normal read_imagef to read image B when B has transpose.
                // thus we don't need to pad image A at all.
                if (!is_image_a)
                {
                    bool padding;
                    padding = !is_image_b;
                    ocl4dnnGEMMCopyBufferToImage<Dtype>(ctx_id, &ImA,
                                                        A, blockA_offset,
                                                        true, TransA != CblasNoTrans,
                                                        padding, imageA_h, imageA_w,
                                                        blockA_height, blockA_width, ldA,
                                                        0, NULL, &ev[ev_idx]);
                    if (ev[ev_idx] != NULL)
                        ev_idx++;
                }

                if (!is_image_b && (K % use_buffer_indicator != 0))
                {
                    ocl4dnnGEMMCopyBufferToImage<Dtype>(ctx_id, &ImB,
                                                        B, blockB_offset,
                                                        false, true, false, imageB_h, imageB_w,
                                                        blockB_height, blockB_width, ldB, 0,
                                                        NULL, &ev[ev_idx]);
                    if (ev[ev_idx] != NULL)
                        ev_idx++;
                }
            }
            if (is_image_a)
                ImA = A;
            if (is_image_b)
                ImB = B;

            size_t global[2];
            if (gemm_type == GEMM_TYPE_FAST_IMAGE_32_1 || gemm_type == GEMM_TYPE_FAST_IMAGE_B_IMAGE )
            {
                global[0] = (size_t)( blockC_width + 7 ) & ~7;
            } else {
                global[0] = (size_t)( (blockC_width / 2 ) + 7 ) ^ ~7;
            }
            global[1] = (size_t)(blockC_height + 31) / 32;

            size_t local[2];
            local[0] = 8;
            local[1] = 1;

            cl_uint arg_idx = 0;
            oclk_gemm_float.set(arg_idx++, (cl_mem) ImA);
            if (TransB == CblasNoTrans || is_image_b || (K % use_buffer_indicator != 0))
            {
                oclk_gemm_float.set(arg_idx++, (cl_mem) ImB);
            } else {
                oclk_gemm_float.set(arg_idx++, (cl_mem) B);
                oclk_gemm_float.set(arg_idx++, blockB_offset);
                oclk_gemm_float.set(arg_idx++, ldB);
            }
            oclk_gemm_float.set(arg_idx++, (cl_mem) C);
            oclk_gemm_float.set(arg_idx++, blockC_offset);
            oclk_gemm_float.set(arg_idx++, blockC_height);
            oclk_gemm_float.set(arg_idx++, blockC_width);
            oclk_gemm_float.set(arg_idx++, ldC);
            oclk_gemm_float.set(arg_idx++, alpha);
            oclk_gemm_float.set(arg_idx++, beta);
            oclk_gemm_float.set(arg_idx++, padded_k);
            if (TransB != CblasNoTrans)
                oclk_gemm_float.set(arg_idx++, block_Ksize);
            oclk_gemm_float.set(arg_idx++, isFirstColBlock);

            cl_event *wait_list = NULL;
            if (ev_idx != 0)
                wait_list = &ev[0];
            OCL_CHECK(clEnqueueNDRangeKernel((cl_command_queue)queue.ptr(),
                                             (cl_kernel)oclk_gemm_float.ptr(), 2, NULL,
                                             global, local, ev_idx,
                                             wait_list, &ev[ev_idx]));
            if (TransA == CblasNoTrans)
                A_start_x += blockA_width;
            else
                A_start_y += blockA_height;

            if (TransB == CblasNoTrans)
                B_start_y += blockB_height;
            else
                B_start_x += blockB_width;

            isFirstColBlock = 0;
            arg->evs.assign(ev, ev + ev_idx + 1);
            clSetEventCallback(ev[ev_idx], CL_COMPLETE, &gemmCallback,
                               static_cast<void*>(arg));
        }

        C_start_x += blockC_width;
        if (TransA == CblasNoTrans)
            A_start_x = 0;
        else
            A_start_y = 0;
        if (TransB == CblasNoTrans)
        {
            B_start_x += blockB_width;
            B_start_y = 0;
        } else {
            B_start_y += blockB_height;
            B_start_x = 0;
        }
        if (C_start_x >= N)
        {
            C_start_x = 0;
            B_start_x = 0;
            B_start_y = 0;
            C_start_y += blockC_height;
            if (TransA == CblasNoTrans)
                A_start_y += blockA_height;
            else
                A_start_x += blockA_width;
        }
    }

    if (ImA && !is_image_a)
        clReleaseMemObject(ImA);
    if (ImB && !is_image_b)
        clReleaseMemObject(ImB);
}

template<typename Dtype>
void ocl4dnnGEMMCommon(const int32_t ctx_id, const CBLAS_TRANSPOSE TransB,
                       const int32_t M, const int32_t N, const int32_t K,
                       const cl_mem A, const cl_mem B,
                       const cl_mem B_image, cl_mem C,
                       const size_t max_image_size)
{
    gemm_type_t gemm_type = GEMM_TYPE_FAST_IMAGE_32_1;

    if (gemm_type == GEMM_TYPE_FAST_IMAGE_32_1 ||
        gemm_type == GEMM_TYPE_FAST_IMAGE_32_2)
    {
        ocl4dnnFastImageGEMM<Dtype>(ctx_id, CblasNoTrans, TransB, M, N, K,
                                    (Dtype)1., A, 0, B, 0, (Dtype)0., C,
                                    0, false, false, gemm_type, max_image_size);
    }
    else if (gemm_type == GEMM_TYPE_FAST_IMAGE_B_IMAGE)
    {
        ocl4dnnFastImageGEMM<Dtype>(ctx_id, CblasNoTrans, TransB, M, N, K,
                                    (Dtype)1., A, 0, B_image, 0, (Dtype)0., C,
                                    0, false, true,
                                    GEMM_TYPE_FAST_IMAGE_B_IMAGE,
                                    max_image_size);
    }
}

template void ocl4dnnGEMMCommon<float>(const int32_t ctx_id, const CBLAS_TRANSPOSE TransB,
                                       const int32_t M, const int32_t N, const int32_t K,
                                       const cl_mem A, const cl_mem B,
                                       const cl_mem B_image, cl_mem C,
                                       const size_t max_image_size);

template<typename Dtype>
void ocl4dnnGEMV(const int32_t ctx_id, const CBLAS_TRANSPOSE TransA,
                 const int32_t M, const int32_t N, const Dtype alpha,
                 const cl_mem A, const int32_t offA, const cl_mem x,
                 const int32_t offx, const Dtype beta, cl_mem y,
                 const int32_t offy)
{
    ocl::Context ctx = ocl::Context::getDefault();

    if (ocl::Device::getDefault().type() == CL_DEVICE_TYPE_CPU)
    {
        LOG(FATAL) << "Not Support CPU device";
    }
    else
    {
        if (std::is_same<Dtype, float>::value && TransA == CblasNoTrans)
        {
            ocl::Kernel k(CL_KERNEL_SELECT("matvec_mul4"), cv::ocl::dnn::matvec_mul_oclsrc);
            uint row_size = M;
            uint col_size = N;
            size_t localsize = 128;
            size_t globalsize = row_size / 4 * localsize;

            uint argId = 0;
            k.set(argId++, (cl_mem) A);
            k.set(argId++, offA);
            k.set(argId++, cl_uint(col_size));
            k.set(argId++, cl_uint(col_size%4));
            k.set(argId++, (cl_mem) x);
            k.set(argId++, offx);
            k.set(argId++, alpha);
            k.set(argId++, beta);
            k.set(argId++, (cl_mem) y);
            k.set(argId++, offy);
            clSetKernelArg((cl_kernel)k.ptr(), argId++, localsize * sizeof(cl_float4), NULL);

            clEnqueueNDRangeKernel((cl_command_queue)ocl::Queue::getDefault().ptr(),
                                   (cl_kernel)k.ptr(), 1,
                                   NULL,
                                   &globalsize,
                                   &localsize, 0, NULL,
                                   NULL);
            if ((row_size % 4) != 0)
            {
                ocl::Kernel k_1(CL_KERNEL_SELECT("matvec_mul1"), cv::ocl::dnn::matvec_mul_oclsrc);
                size_t localsize = 128;
                size_t globalsize = row_size % 4 * localsize;
                uint row_offset = row_size - (row_size % 4);

                uint argId = 0;
                k_1.set(argId++, (cl_mem) A);
                k_1.set(argId++, offA);
                k_1.set(argId++, cl_uint(col_size));
                k_1.set(argId++, cl_uint(row_offset));
                k_1.set(argId++, cl_uint(col_size%4));
                k_1.set(argId++, (cl_mem) x);
                k_1.set(argId++, offx);
                k_1.set(argId++, alpha);
                k_1.set(argId++, beta);
                k_1.set(argId++, (cl_mem) y);
                k_1.set(argId++, offy);
                clSetKernelArg((cl_kernel)k_1.ptr(), argId++, localsize * sizeof(cl_float), NULL);

                clEnqueueNDRangeKernel((cl_command_queue)ocl::Queue::getDefault().ptr(),
                                       (cl_kernel)k_1.ptr(), 1,
                                       NULL,
                                       &globalsize,
                                       &localsize, 0, NULL,
                                       NULL);
            }
        }
        else
        {
            /* FIXME add implementation here */
        }
    }
}

template void ocl4dnnGEMV<float>(const int32_t ctx_id,
                                 const CBLAS_TRANSPOSE TransA,
                                 const int32_t M, const int32_t N,
                                 const float alpha, const cl_mem A,
                                 const int32_t offA, const cl_mem x,
                                 const int32_t offx, const float beta,
                                 cl_mem y, const int32_t offy);

template<typename Dtype>
void ocl4dnnAXPY(const int32_t ctx_id, const int32_t N, const Dtype alpha,
                 const cl_mem X, const int32_t offX, cl_mem Y,
                 const int32_t offY)
{
    ocl::Context ctx = ocl::Context::getDefault();

    if (ocl::Device::getDefault().type() == CL_DEVICE_TYPE_CPU)
    {
        LOG(FATAL) << "Not Support CPU device";
    }
    else
    {
        ocl::Kernel oclk_axpy(CL_KERNEL_SELECT("axpy"), cv::ocl::dnn::math_oclsrc);
        size_t global[] = { 128 * 128 };
        size_t local[] = { 128 };

        cl_uint argIdx = 0;
        oclk_axpy.set(argIdx++, N);
        oclk_axpy.set(argIdx++, alpha);
        oclk_axpy.set(argIdx++, (cl_mem) X);
        oclk_axpy.set(argIdx++, offX);
        oclk_axpy.set(argIdx++, (cl_mem) Y);
        oclk_axpy.set(argIdx++, offY);

        oclk_axpy.run(1, global, local, false);
    }
}

template void ocl4dnnAXPY<float>(const int32_t ctx_id, const int32_t N,
                                 const float alpha, const cl_mem X,
                                 const int32_t offX, cl_mem Y,
                                 const int32_t offY);

template<typename Dtype>
void ocl4dnnSet(const int32_t ctx_id, const int32_t N, const Dtype alpha,
                cl_mem Y, const int32_t offY)
{
    ocl::Kernel oclk_fill(CL_KERNEL_SELECT("fill"), cv::ocl::dnn::fillbuffer_oclsrc);
    size_t global[] = { 128 * 128 };
    size_t local[] = { 128 };

    cl_uint argIdx = 0;
    oclk_fill.set(argIdx++, N);
    oclk_fill.set(argIdx++, alpha);
    oclk_fill.set(argIdx++, (cl_mem) Y);
    oclk_fill.set(argIdx++, offY);

    oclk_fill.run(1, global, local, false);
}

template void ocl4dnnSet<int32_t>(const int32_t ctx_id, const int32_t N,
                                  const int32_t alpha, cl_mem Y,
                                  const int32_t offY);
template void ocl4dnnSet<float>(const int32_t ctx_id, const int32_t N,
                                const float alpha, cl_mem Y,
                                const int32_t offY);
#endif  // HAVE_OPENCL
