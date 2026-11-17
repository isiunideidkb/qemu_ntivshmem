//written by LLM
#define _GNU_SOURCE
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ===================== 测试配置 =====================
#define TEST_ROUNDS     5
#define WARMUP_ROUNDS   2
#define ALIGNMENT       64

// 额外增加的测试规格：1KB / 32KB / 256KB / 1MB / 4MB / 16MB / 32MB / 64MB
static const size_t extra_sizes[] = {
    1ULL   * 1024,
    32ULL  * 1024,
    256ULL * 1024,
    1ULL   * 1024 * 1024,
    4ULL   * 1024 * 1024,
    16ULL  * 1024 * 1024,
    32ULL  * 1024 * 1024,
    64ULL  * 1024 * 1024
};
#define EXTRA_SIZE_COUNT (sizeof(extra_sizes) / sizeof(extra_sizes[0]))

// ===================== 嵌入源数据（完整原始数据）=====================
char data[] __attribute__((section(".custom_data"), aligned(ALIGNMENT))) = {
#embed "/mount_points/internalHDD/Serial Experiments Lain (Japan) (Disc 1).chd"
};
#define RAW_DATA_SIZE (sizeof(data))

// ===================== 高精度计时 =====================
static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// ===================== 1. ERMSB rep movsb =====================
static void rep_cpy(void* src, void* dest, unsigned long length) {
    asm volatile(
        "movq %[src], %%rsi \n\t"
        "movq %[dest], %%rdi \n\t"
        "mov  %[length], %%rcx \n\t"
        "rep movsb \n\t"
        :
        : [src]"rm"(src), [dest]"rm"(dest), [length]"rm"(length)
        : "rdi", "rsi", "rcx", "memory"
    );
}

// ===================== 2. AVX 缓存拷贝 (Cached) =====================
static void simd_cached_cpy(void* src, void* dest, unsigned long length) {
    unsigned long offset;
    for (offset = 0; offset + 64 <= length; offset += 64) {
        asm volatile(
            "vmovdqa   (%[s], %[off]), %%ymm0\n\t"
            "vmovdqa 32(%[s], %[off]), %%ymm1\n\t"
            "vmovdqa %%ymm0,   (%[d], %[off])\n\t"
            "vmovdqa %%ymm1, 32(%[d], %[off])\n\t"
            :
            : [s]   "r" (src),
              [d]   "r" (dest),
              [off] "r" (offset)
            : "ymm0", "ymm1", "memory"
        );
    }
}

// ===================== 3. AVX 非暂存拷贝 (NT Store) =====================
static void simd_nt_cpy(void* src, void* dest, unsigned long length) {
    unsigned long offset;
    for (offset = 0; offset + 64 <= length; offset += 64) {
        asm volatile(
            "vmovdqa   (%[s], %[off]), %%ymm0\n\t"
            "vmovdqa 32(%[s], %[off]), %%ymm1\n\t"
            "vmovntdq %%ymm0,   (%[d], %[off])\n\t"
            "vmovntdq %%ymm1, 32(%[d], %[off])\n\t"
            :
            : [s]   "r" (src),
              [d]   "r" (dest),
              [off] "r" (offset)
            : "ymm0", "ymm1", "memory"
        );
    }
    asm volatile ("sfence" ::: "memory");
}

// ===================== 单次基准测试 =====================
static void bench_single(const char* name,
                         void (*copy_fn)(void*, void*, unsigned long),
                         void* src, void* dest, size_t size) {
    // 预热
    for (int i = 0; i < WARMUP_ROUNDS; ++i) {
        copy_fn(src, dest, size);
    }

    double total_ns = 0.0;
    double min_ns = 1e18;

    for (int i = 0; i < TEST_ROUNDS; ++i) {
        memset(dest, 0, size);

        uint64_t t0 = get_time_ns();
        copy_fn(src, dest, size);
        uint64_t t1 = get_time_ns();

        double elapsed = (double)(t1 - t0);
        total_ns += elapsed;
        if (elapsed < min_ns) min_ns = elapsed;
    }

    double avg_sec = (total_ns / TEST_ROUNDS) / 1e9;
    double min_sec = min_ns / 1e9;
    double size_mb = (double)size / (1024.0 * 1024.0);

    printf("%-18s | 平均: %.6f s | 最佳: %.6f s | 带宽: %.2f MB/s\n",
           name, avg_sec, min_sec, size_mb / avg_sec);
}

// ===================== 主函数 =====================
int main(void) {
    printf("===== 内存拷贝基准测试 =====\n");
    printf("完整文件大小: %.2f MB\n", (double)RAW_DATA_SIZE / (1024*1024));
    printf("测试轮数: %d（预热 %d 轮）\n", TEST_ROUNDS, WARMUP_ROUNDS);
    printf("对齐粒度: %d 字节\n\n", ALIGNMENT);

    // 计算对齐后的完整测试大小
    size_t full_size = RAW_DATA_SIZE - (RAW_DATA_SIZE % ALIGNMENT);

    // 分配最大所需内存（完整文件大小）
    void* dest_buf = aligned_alloc(ALIGNMENT, full_size);
    if (!dest_buf) {
        perror("aligned_alloc 失败");
        return 1;
    }

    // ========== 第一部分：额外增加的多规格测试 ==========
    printf("============ 额外规格测试 ============\n");
    for (int i = 0; i < EXTRA_SIZE_COUNT; ++i) {
        size_t size = extra_sizes[i];
        if (size > full_size) {
            printf("\n>>> 尺寸 %.2f MB 超出文件大小，跳过\n", (double)size/(1024*1024));
            continue;
        }
        size -= size % ALIGNMENT;

        printf("----------------------------------------\n");
        printf(">>> 测试尺寸: %.2f MB (%zu 字节)\n",
               (double)size / (1024*1024), size);

        bench_single("ERMSB rep movsb", rep_cpy, data, dest_buf, size);
        bench_single("AVX Cached",     simd_cached_cpy, data, dest_buf, size);
        bench_single("AVX NT Store",   simd_nt_cpy, data, dest_buf, size);
        printf("\n");
    }

    // ========== 第二部分：原始完整文件大小测试 ==========
    printf("============ 原始完整文件测试 ============\n");
    printf("----------------------------------------\n");
    printf(">>> 测试尺寸: %.2f MB (%zu 字节)\n",
           (double)full_size / (1024*1024), full_size);

    bench_single("ERMSB rep movsb", rep_cpy, data, dest_buf, full_size);
    bench_single("AVX Cached",     simd_cached_cpy, data, dest_buf, full_size);
    bench_single("AVX NT Store",   simd_nt_cpy, data, dest_buf, full_size);
    printf("\n");

    free(dest_buf);
    asm volatile ("vzeroupper" ::: "ymm0", "ymm1");

    return 0;
}
