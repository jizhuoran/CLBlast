/* ************************************************************************
 * Copyright 2013 Advanced Micro Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ************************************************************************/


/*
 * Gbmv performance test cases
 */

#include <stdlib.h>             // srand()
#include <string.h>             // memcpy()
#include <gtest/gtest.h>
#include <clBLAS.h>
#include <common.h>
#include <clBLAS-wrapper.h>
#include <BlasBase.h>
#include <gbmv.h>
#include <blas-random.h>

#ifdef PERF_TEST_WITH_ACML
#include <blas-internal.h>
#include <blas-wrapper.h>
#endif

#include "PerformanceTest.h"

using namespace std;
using namespace clMath;

#define CHECK_RESULT(ret)                                                   \
do {                                                                        \
    ASSERT_GE(ret, 0) << "Fatal error: can not allocate resources or "      \
                         "perform an OpenCL request!" << endl;              \
    EXPECT_EQ(0, ret) << "The OpenCL version is slower in the case" <<      \
                         endl;                                              \
} while (0)

namespace clMath {

template <typename ElemType> class GbmvPerformanceTest : public PerformanceTest
{
public:
    virtual ~GbmvPerformanceTest();

    virtual int prepare(void);
    virtual nano_time_t etalonPerfSingle(void);
    virtual nano_time_t clblasPerfSingle(void);

    static void runInstance(BlasFunction fn, TestParams *params)
    {
        GbmvPerformanceTest<ElemType> perfCase(fn, params);
        int ret = 0;
        int opFactor = 1;
        BlasBase *base;

        base = clMath::BlasBase::getInstance();

        if ((fn == FN_DGBMV || fn == FN_ZGBMV) &&
            !base->isDevSupportDoublePrecision()) {

            std::cerr << ">> WARNING: The target device doesn't support native "
                         "double precision floating point arithmetic" <<
                         std::endl << ">> Test skipped" << std::endl;
            return;
        }

        if (!perfCase.areResourcesSufficient(params)) {
            std::cerr << ">> RESOURCE CHECK: Skip due to unsufficient resources" <<
                        std::endl;
        }
        else {
            ret = perfCase.run(opFactor);
        }

        ASSERT_GE(ret, 0) << "Fatal error: can not allocate resources or "
                             "perform an OpenCL request!" << endl;
        EXPECT_EQ(0, ret) << "The OpenCL version is slower in the case" << endl;
    }

private:
    GbmvPerformanceTest(BlasFunction fn, TestParams *params);

    bool areResourcesSufficient(TestParams *params);

    TestParams params_;
    ElemType alpha;
    ElemType beta;
    ElemType *A_;
    ElemType *X_;
    ElemType *Y_;
    ElemType *backY_;
    cl_mem mobjA_;
    cl_mem mobjX_;
    cl_mem mobjY_;
    ::clMath::BlasBase *base_;
};

template <typename ElemType>
GbmvPerformanceTest<ElemType>::GbmvPerformanceTest(
    BlasFunction fn,
    TestParams *params) : PerformanceTest(fn,
    (problem_size_t)( ( (((params->order == clblasColumnMajor)? params->N : params->M) * (params->KL + params->KU + 1)   // A-access
                          - (params->KL*(params->KL+1) + params->KU*(params->KU+1)) )       // Substract hole-part for A & X
                        +((params->transA == clblasNoTrans)? ((params->KL + params->KU + 1) * params->M + 2*params->M)   // X & Y access
                                                              : ((params->KL + params->KU + 1) * params->N + 2*params->N) ) // X & Y for Trans case
                                                                                                              ) * sizeof(ElemType) ) ),
                          params_(*params), mobjA_(NULL), mobjX_(NULL), mobjY_(NULL)
{
    size_t lenA, lenX, lenY;
    lenA = ((params_.order == clblasColumnMajor)? params_.N : params_.M) * (params_.lda) + params_.offA;
    lenX = (((params_.transA == clblasNoTrans)? params_.N : params_.M) - 1)* params_.incx + 1 + params_.offBX;
    lenY = (((params_.transA == clblasNoTrans)? params_.M : params_.N) - 1)* params_.incy + 1 + params_.offCY;
    A_ = new ElemType[ lenA ];
    X_ = new ElemType[ lenX ];
    Y_ = new ElemType[ lenY ];
    backY_ = new ElemType[ lenY ];
    alpha = convertMultiplier<ElemType>(params_.alpha);
	beta  = convertMultiplier<ElemType>(params_.beta);

    base_ = ::clMath::BlasBase::getInstance();

	mobjA_ = NULL;
	mobjX_ = NULL;
	mobjY_ = NULL;
}

template <typename ElemType>
GbmvPerformanceTest<ElemType>::~GbmvPerformanceTest()
{
    if(A_ != NULL)
    {
        delete[] A_;
    }
	if(X_ != NULL)
	{
        delete[] X_;
	}
	if(backY_ != NULL)
	{
		delete[] backY_;
	}
	if(Y_ != NULL)
	{
	    delete[] Y_;
	}

    if ( mobjA_ != NULL )
		clReleaseMemObject(mobjA_);
	if ( mobjX_ != NULL )
	    clReleaseMemObject(mobjX_);
	if ( mobjY_ != NULL )
		clReleaseMemObject(mobjY_);
}

/*
 * Check if available OpenCL resources are sufficient to
 * run the test case
 */
template <typename ElemType> bool
GbmvPerformanceTest<ElemType>::areResourcesSufficient(TestParams *params)
{
    clMath::BlasBase *base;
    size_t gmemSize, allocSize;
    size_t m = params->M, n = params->N, lda = params->lda;
    size_t lenA = (((params->order == clblasColumnMajor)? n : m) * lda  + params->offA)* sizeof(ElemType);
    size_t lenX = ((((params->transA == clblasNoTrans)? params->N : params->M) - 1)* params->incx + 1 + params->offBX) * sizeof(ElemType);
    size_t lenY = ((((params->transA == clblasNoTrans)? params->M : params->N) - 1)* params->incy + 1 + params->offCY) * sizeof(ElemType);

    if((A_ == NULL) || (X_ == NULL) || (Y_ == NULL) || (backY_ == NULL))
	{
		return 0;
	}

    base = clMath::BlasBase::getInstance();
    gmemSize = (size_t)base->availGlobalMemSize(0);
    allocSize = (size_t)base->maxMemAllocSize();

    bool suff = (lenA < allocSize) && ( (lenA + lenX + lenY) < gmemSize );

    return suff;
}

template <typename ElemType> int
GbmvPerformanceTest<ElemType>::prepare(void)
{
    size_t lenX, lenY, lenA;

    lenA = ((params_.order == clblasColumnMajor)? params_.N : params_.M) * params_.lda + params_.offA;

    if (params_.transA == clblasNoTrans) {
        lenX = (params_.N - 1)*abs(params_.incx) + 1 + params_.offBX;
        lenY = (params_.M - 1)*abs(params_.incy) + 1 + params_.offCY;
    }
    else {
        lenX = (params_.M - 1)*abs(params_.incx) + 1 + params_.offBX;
        lenY = (params_.N - 1)*abs(params_.incy) + 1 + params_.offCY;
    }

    randomGbmvMatrices(params_.order, params_.transA, params_.M, params_.N, &alpha, &beta,
                        (A_+params_.offA), params_.lda, (X_+params_.offBX), params_.incx, (Y_+params_.offCY), params_.incy );

    memcpy(backY_, Y_, lenY * sizeof(ElemType));

    mobjA_ = base_->createEnqueueBuffer(A_, lenA * sizeof(ElemType), 0, CL_MEM_READ_ONLY);
    mobjX_ = base_->createEnqueueBuffer(X_, lenX * sizeof(ElemType), 0, CL_MEM_READ_ONLY);
    mobjY_ = base_->createEnqueueBuffer(backY_, lenY * sizeof(ElemType), 0, CL_MEM_READ_WRITE);

    return ((mobjA_ != NULL) && (mobjX_ != NULL) && (mobjY_ != NULL)) ? 0 : -1;
}

template <typename ElemType> nano_time_t
GbmvPerformanceTest<ElemType>::etalonPerfSingle(void)
{
    nano_time_t time = 0;
    clblasOrder fOrder;
    clblasTranspose fTrans;
    size_t lda, lenY, lenA;
    size_t fM = params_.M, fN = params_.N, fKL = params_.KL, fKU = params_.KU;

    lenA = ((params_.order == clblasColumnMajor)? params_.N : params_.M) * params_.lda;
    lenY = (((params_.transA == clblasNoTrans)? params_.M : params_.N) - 1)* params_.incy + 1 + params_.offCY;

    memcpy(Y_, backY_, lenY * sizeof(ElemType));
    fOrder = params_.order;
    fTrans = params_.transA;
    lda = params_.lda;

    if (fOrder != clblasColumnMajor)
    {
        fOrder = clblasColumnMajor;
        fTrans = (params_.transA == clblasNoTrans)? clblasTrans : clblasNoTrans;
        fM = params_.N;
        fN = params_.M;
        fKL = params_.KU;
        fKU = params_.KL;

		if( params_.transA == clblasConjTrans )
            doConjugate( (A_+params_.offa), 1, lenA, lda );
   	}

#ifdef PERF_TEST_WITH_ACML

   	time = getCurrentTime();
   	clMath::blas::gbmv(fOrder, fTrans, fM, fN, fKL, fKU, alpha, A_, params_.offA, lda,
							X_, params_.offBX, params_.incx, beta, Y_, params_.offCY, params_.incy);
  	time = getCurrentTime() - time;

#endif  // PERF_TEST_WITH_ACML

    return time;
}


template <typename ElemType> nano_time_t
GbmvPerformanceTest<ElemType>::clblasPerfSingle(void)
{
    nano_time_t time;
    cl_event event;
    cl_int status;
    size_t lenY;
    cl_command_queue queue = base_->commandQueues()[0];

    lenY = (((params_.transA == clblasNoTrans)? params_.M : params_.N) - 1)* params_.incy + 1 + params_.offCY;

    status = clEnqueueWriteBuffer(queue, mobjY_, CL_TRUE, 0,
                                  lenY * sizeof(ElemType), backY_, 0, NULL, &event);

    if (status != CL_SUCCESS) {
        cerr << "Vector Y buffer object enqueuing error, status = " <<
                 status << endl;

        return NANOTIME_ERR;
    }

    status = clWaitForEvents(1, &event);
    if (status != CL_SUCCESS) {
        cout << "Wait on event failed, status = " <<
                status << endl;

        return NANOTIME_ERR;
    }

    event = NULL;
    time = getCurrentTime();
    int iter = 20;
	for ( int i = 1; i <= iter; i++)
	{
        status = clMath::clblas::gbmv(params_.order, params_.transA, params_.M, params_.N, params_.KL, params_.KU,
                                        alpha, mobjA_, params_.offA, params_.lda, mobjX_, params_.offBX, params_.incx,
                                        beta, mobjY_, params_.offCY, params_.incy, 1, &queue, 0, NULL, &event);

        if (status != CL_SUCCESS) {
            cerr << "The CLBLAS GBMV function failed, status = " <<
                    status << endl;
            return NANOTIME_ERR;
        }
    }
    clFinish( queue );
    time = getCurrentTime() - time;
	time /= iter;

    return time;
}

} // namespace clMath

// sgbmv performance test
TEST_P(GBMV, sgbmv)
{
    TestParams params;

    getParams(&params);
    GbmvPerformanceTest<float>::runInstance(FN_SGBMV, &params);
}

// dgbmv performance test case
TEST_P(GBMV, dgbmv)
{
    TestParams params;

    getParams(&params);
    GbmvPerformanceTest<double>::runInstance(FN_DGBMV, &params);
}

// cgbmv performance test
TEST_P(GBMV, cgbmv)
{
    TestParams params;

    getParams(&params);
    GbmvPerformanceTest<FloatComplex>::runInstance(FN_CGBMV, &params);
}

// zgbmv performance test case
TEST_P(GBMV, zgbmv)
{
    TestParams params;

    getParams(&params);
    GbmvPerformanceTest<DoubleComplex>::runInstance(FN_ZGBMV, &params);
}
