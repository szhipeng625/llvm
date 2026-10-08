#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cuda_runtime.h>

// 非融合版本：朴素矩阵乘，无共享内存优化
__global__ void naive_matmul(const float* __restrict__ A,
                             const float* __restrict__ B,
                             float* __restrict__ C, int N) {
    int row = blockIdx.y * blockDim.y + threadIdx.y;
    int col = blockIdx.x * blockDim.x + threadIdx.x;
    if (row < N && col < N) {
        float sum = 0.0f;
        for (int k = 0; k < N; ++k)
            sum += A[row * N + k] * B[k * N + col];
        C[row * N + col] = sum;
    }
}

// 融合版本：共享内存 tiling + 融合 bias + GELU 激活
__global__ void fused_matmul_gelu(const float* __restrict__ A,
                                   const float* __restrict__ B,
                                   const float* __restrict__ bias,
                                   float* __restrict__ C, int N) {
    __shared__ float As[16][16];
    __shared__ float Bs[16][16];
    int tx = threadIdx.x, ty = threadIdx.y;
    int row = blockIdx.y * 16 + ty;
    int col = blockIdx.x * 16 + tx;
    float sum = 0.0f;
    for (int m = 0; m < (N + 15) / 16; ++m) {
        As[ty][tx] = (row < N && (m * 16 + tx) < N) ? A[row * N + m * 16 + tx] : 0.0f;
        Bs[ty][tx] = ((m * 16 + ty) < N && col < N) ? B[(m * 16 + ty) * N + col] : 0.0f;
        __syncthreads();
        for (int k = 0; k < 16; ++k)
            sum += As[ty][k] * Bs[k][tx];
        __syncthreads();
    }
    if (row < N && col < N) {
        sum += bias[col];
        float x = sum;
        float x3 = x * x * x;
        float inner = 0.7978845608f * (x + 0.044715f * x3);
        C[row * N + col] = 0.5f * x * (1.0f + tanhf(inner));
    }
}

int main(int argc, char** argv) {
    int mode = 0;
    int phase_seconds = 300;
    int N = 1024;

    if (argc > 1) mode = atoi(argv[1]);
    if (argc > 2) phase_seconds = atoi(argv[2]);
    if (argc > 3) N = atoi(argv[3]);

    printf("[CUDA 压测] 模式=%s N=%d 时长=%ds\n",
           mode ? "融合优化" : "非融合", N, phase_seconds);
    fflush(stdout);

    size_t bytes = (size_t)N * N * sizeof(float);
    float* h_A = (float*)malloc(bytes);
    float* h_B = (float*)malloc(bytes);
    float* h_bias = (float*)malloc((size_t)N * sizeof(float));
    for (int i = 0; i < N * N; ++i) {
        h_A[i] = (float)(i % 100) / 100.0f;
        h_B[i] = (float)(i % 50) / 50.0f;
    }
    for (int i = 0; i < N; ++i) h_bias[i] = 0.1f * i;

    float *d_A, *d_B, *d_C, *d_bias;
    cudaMalloc(&d_A, bytes);
    cudaMalloc(&d_B, bytes);
    cudaMalloc(&d_C, bytes);
    cudaMalloc(&d_bias, (size_t)N * sizeof(float));
    cudaMemcpy(d_A, h_A, bytes, cudaMemcpyHostToDevice);
    cudaMemcpy(d_B, h_B, bytes, cudaMemcpyHostToDevice);
    cudaMemcpy(d_bias, h_bias, (size_t)N * sizeof(float), cudaMemcpyHostToDevice);

    dim3 block(16, 16);
    dim3 grid((N + 15) / 16, (N + 15) / 16);

    time_t start = time(NULL);
    long iterations = 0;

    while (time(NULL) - start < phase_seconds) {
        if (mode == 0) {
            for (int r = 0; r < 10; ++r) {
                naive_matmul<<<grid, block>>>(d_A, d_B, d_C, N);
            }
        } else {
            fused_matmul_gelu<<<grid, block>>>(d_A, d_B, d_bias, d_C, N);
        }
        cudaDeviceSynchronize();
        iterations++;
    }

    printf("[CUDA 压测] 完成 迭代=%ld\n", iterations);
    fflush(stdout);

    cudaFree(d_A);
    cudaFree(d_B);
    cudaFree(d_C);
    cudaFree(d_bias);
    free(h_A);
    free(h_B);
    free(h_bias);
    return 0;
}
