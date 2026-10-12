// 内存不足时 aec48_create 必须返回 NULL、不结束宿主进程（aec48.h 的约定）。
// 做法：在子进程里让建实例过程中的第 k 次 malloc 失败，k 从 1 扫到建实例用到的最后一次；
// 子进程被信号结束（崩溃）即判失败。aec48_robust 替换的是 operator new，拦不到 C 代码（pffft）里的 malloc，这里补上。
// 只在 Linux 主机上编（借 glibc 的 __libc_malloc 转发）。用法：aec48_oom
#include "aec48.h"
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" void* __libc_malloc(size_t);
static long gFailAt = -1, gCount = 0;
extern "C" void* malloc(size_t n) {   // 覆盖 malloc（operator new、pffft、KleidiAI 打包缓冲都走它）
    if (gFailAt >= 0 && ++gCount == gFailAt) return nullptr;
    return __libc_malloc(n);
}

// 子进程退出码：0 = 建成且能处理（k 超出了分配次数），2 = 返回 NULL，3 = 建成但分配确实失败过（失败被吞掉）。
// 3 不算失败：glibc 实测有 3 处——读内嵌模型流时 stdio 给文件流分配缓冲失败，C 库自己退回无缓冲读取，结果照常正确。
static int child(int sr, int fpc, long k) {
    gCount = 0; gFailAt = k;
    Aec48* a = aec48_create(sr, fpc);
    const long used = gCount; gFailAt = -1;
    if (!a) return 2;
    std::vector<float> mic(fpc, 0.01f), ref(fpc, 0.02f), out(fpc);
    for (int i = 0; i < 50; ++i) aec48_process(a, mic.data(), ref.data(), out.data(), fpc);
    aec48_destroy(a);
    return used >= k ? 3 : 0;
}

int main() {
    int bad = 0;
    for (int sr : {48000, 44100}) {
        const int fpc = sr == 48000 ? 384 : 441;
        long k = 1, nulls = 0, swallowed = 0; const int bad0 = bad;
        for (;; ++k) {
            fflush(stdout);
            const pid_t p = fork();
            if (p == 0) _exit(child(sr, fpc, k));
            int st = 0; waitpid(p, &st, 0);
            if (WIFSIGNALED(st)) { printf("不通过  %d Hz：第 %ld 次分配失败时进程被信号 %d 结束\n", sr, k, WTERMSIG(st)); ++bad; continue; }
            const int rc = WEXITSTATUS(st);
            if (rc == 0) break;
            if (rc == 2) ++nulls; else if (rc == 3) ++swallowed;
            else { printf("不通过  %d Hz：第 %ld 次，子进程退出码 %d\n", sr, k, rc); ++bad; }
            if (k > 100000) { printf("不通过：分配次数超过 10 万，停止\n"); ++bad; break; }
        }
        printf("%s  %d Hz：建实例共 %ld 次分配，逐次让其失败：返回 NULL %ld 次，失败被吞掉但实例可用 %ld 次\n",
               bad > bad0 ? "不通过" : (swallowed ? "注意" : "通过"), sr, k - 1, nulls, swallowed);
    }
    printf(bad ? "有问题：不通过 %d 项\n" : "全部通过：不通过 %d 项\n", bad);
    return bad ? 1 : 0;
}
