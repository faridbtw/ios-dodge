/* Run as /redwood/bin/op13_cpu_check [expected CPUs, default 8].
 * A scheduler sample proves that this workload actually ran on a CPU;
 * a reported hw.ncpu alone does not. No CPU affinity is assumed on Darwin.
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define LIMIT 16
#define ROUNDS 120000000ULL
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int go;
static unsigned seen[LIMIT];
static uint64_t sums[LIMIT];

static int number(const char *name)
{
    int n = -1;
    size_t size = sizeof(n);
    return sysctlbyname(name, &n, &size, NULL, 0) == 0 ? n : -1;
}

static void *work(void *argument)
{
    unsigned worker = (unsigned)(uintptr_t)argument;
    volatile uint64_t sum = 0;
    pthread_mutex_lock(&lock);
    while (!go) pthread_cond_wait(&ready, &lock);
    pthread_mutex_unlock(&lock);
    for (uint64_t i = 0; i < ROUNDS; i++) {
        sum += i * i;
        if ((i & 0x3ffff) == 0) {
            size_t cpu;
            if (pthread_cpu_number_np(&cpu) == 0 && cpu < LIMIT)
                seen[worker] |= 1u << cpu;
        }
    }
    sums[worker] = sum;
    return NULL;
}

int main(int argc, char **argv)
{
    int expected = argc > 1 ? atoi(argv[1]) : 8;
    int active = number("hw.activecpu"), count = number("hw.ncpu");
    pthread_t threads[LIMIT];
    mach_timebase_info_data_t tb;
    struct rusage before, after;
    unsigned mask = 0;
    int started = 0, failed = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("op13_cpu_check: expected=%d ncpu=%d active=%d physical=%d logical=%d\n",
        expected, count, active, number("hw.physicalcpu"), number("hw.logicalcpu"));
    if (expected < 1 || expected > LIMIT || active < 1 || active > LIMIT) return 1;
    /* GenericARMPE deliberately starts slower processors after userland boots. */
    for (int seconds = 0; active < expected && seconds < 45; seconds++) {
        if (seconds % 5 == 0)
            printf("op13_cpu_check: waiting for delayed CPU startup (%d active)\n", active);
        sleep(1);
        active = number("hw.activecpu");
    }
    printf("op13_cpu_check: settled active=%d\n", active);
    if (active < 1 || active > LIMIT) return 1;
    mach_timebase_info(&tb);
    getrusage(RUSAGE_SELF, &before);
    uint64_t start = mach_absolute_time();
    for (int i = 0; i < active; i++) {
        int error = pthread_create(&threads[i], NULL, work, (void *)(uintptr_t)i);
        if (error != 0) {
            printf("op13_cpu_check: thread %d: %s\n", i, strerror(error));
            failed = 1;
            break;
        }
        started++;
    }
    pthread_mutex_lock(&lock);
    go = 1;
    pthread_cond_broadcast(&ready);
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < started; i++) {
        if (pthread_join(threads[i], NULL) != 0) failed = 1;
        mask |= seen[i];
        if (sums[i] != sums[0]) failed = 1;
    }
    uint64_t wall_us = (mach_absolute_time() - start) * tb.numer / tb.denom / 1000;
    getrusage(RUSAGE_SELF, &after);
    int64_t used_us = (after.ru_utime.tv_sec + after.ru_stime.tv_sec -
        before.ru_utime.tv_sec - before.ru_stime.tv_sec) * 1000000LL +
        after.ru_utime.tv_usec + after.ru_stime.tv_usec -
        before.ru_utime.tv_usec - before.ru_stime.tv_usec;
    printf("op13_cpu_check: workers=%d wall_us=%llu process_us=%lld concurrency=%.2f observed_mask=0x%x\n",
        started, (unsigned long long)wall_us, (long long)used_us,
        wall_us ? (double)used_us / wall_us : 0, mask);
    for (int i = 0; i < expected; i++)
        printf("op13_cpu_check: cpu %d workload %s\n", i,
            mask & (1u << i) ? "observed" : "NOT OBSERVED");
    if (failed || count != expected || active != expected) {
        puts("op13_cpu_check: FAIL (CPU count, thread execution or arithmetic mismatch)");
        return 1;
    }
    if ((mask & ((1u << expected) - 1)) != (1u << expected) - 1) {
        puts("op13_cpu_check: INCONCLUSIVE (scheduler did not place this workload on every CPU)");
        return 2;
    }
    puts("op13_cpu_check: PASS (expected CPUs online and workload observed on each)");
    return 0;
}
