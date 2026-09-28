/*
 * Intel macOS active CPU frequency, without launching powermetrics.
 * Verified ABI: macOS 15.8 (Darwin 24.6), Intel i5-5287U.
 *
 * clang -std=c11 -O2 -Wall -Wextra -Werror macos_intel_frequency.c -o cpu-frequency
 * ./cpu-frequency [interval_ms [sample_count]]
 *
 * Private ABI: XNU osfmk/i386/Diagnostics.c, dgPowerStat (selector 17).
 * Layout also checked against this Mac's /usr/bin/powermetrics disassembly.
 * This is an interval average while CPUs are active, including Turbo Boost.
 * Other macOS releases/kernel builds need their ABI checked before use.
 * Source reference:
 * https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/i386/Diagnostics.c
 */

#if !defined(__APPLE__) || !defined(__x86_64__)
#error This example requires Intel x86_64 macOS.
#endif

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

/* Only the fields needed for frequency are named; the rest preserve the ABI. */
struct core_counters {
    uint64_t aperf;
    uint64_t mperf;
    uint64_t other_counters[36];
};

struct power_snapshot {
    uint64_t version;
    uint64_t package_counters[50];
    uint32_t ncpus;
    uint32_t padding;
    struct core_counters cores[];
};

_Static_assert(sizeof(struct core_counters) == 0x130, "core ABI size");
_Static_assert(offsetof(struct power_snapshot, ncpus) == 0x198, "ncpus ABI offset");
_Static_assert(offsetof(struct power_snapshot, cores) == 0x1a0, "core array ABI offset");

static void unsupported_call(int signal_number)
{
    (void)signal_number;
    static const char message[] = "dgPowerStat diagnostic call is unavailable or denied.\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _Exit(1);
}

static int read_sysctl(const char *name, void *value, size_t expected_size)
{
    size_t size = expected_size;
    if (sysctlbyname(name, value, &size, NULL, 0) != 0) {
        perror(name);
        return -1;
    }
    if (size != expected_size) {
        fprintf(stderr, "%s: unexpected value size %zu\n", name, size);
        return -1;
    }
    return 0;
}

static int take_snapshot(struct power_snapshot *snapshot, unsigned max_cpus)
{
    snapshot->version = UINT64_MAX;
    snapshot->ncpus = UINT32_MAX;

    /* Darwin diagnostic syscall class, exactly as used by this powermetrics.
     * libc syscall() is for BSD syscalls and must not be used here.
     */
    uint64_t result = 0x04000000;
    uint64_t selector = 17;
    void *address = snapshot;
    __asm__ volatile("syscall"
                     : "+a"(result), "+D"(selector), "+S"(address)
                     :
                     : "rcx", "r11", "cc", "memory");

    if (result != 1 || snapshot->version != 1 ||
        snapshot->ncpus == 0 || snapshot->ncpus > max_cpus) {
        fprintf(stderr, "Unexpected diagnostic result: return=%" PRIu64
                " version=%" PRIu64 " cpus=%" PRIu32 "\n",
                result, snapshot->version, snapshot->ncpus);
        return -1;
    }
    return 0;
}

static long positive_number(const char *s, long maximum)
{
    char *end;
    errno = 0;
    long value = strtol(s, &end, 10);
    if (errno || end == s || *end || value < 1 || value > maximum) {
        fprintf(stderr, "Invalid number: %s (expected 1..%ld)\n", s, maximum);
        exit(2);
    }
    return value;
}

int main(int argc, char **argv)
{
    if (argc > 3) {
        fprintf(stderr, "Usage: %s [interval_ms [sample_count]]\n", argv[0]);
        return 2;
    }
    const long interval_ms = argc > 1 ? positive_number(argv[1], 60000) : 1000;
    const long sample_count = argc > 2 ? positive_number(argv[2], 1000000) : 5;
    int max_cpus;
    uint64_t reference_hz, nominal_hz;
    if (read_sysctl("hw.logicalcpu_max", &max_cpus, sizeof(max_cpus)) ||
        read_sysctl("machdep.tsc.frequency", &reference_hz, sizeof(reference_hz)) ||
        read_sysctl("hw.cpufrequency", &nominal_hz, sizeof(nominal_hz)))
        return 1;
    if (max_cpus < 1 || max_cpus > 4096 || !reference_hz || !nominal_hz) {
        fputs("Invalid CPU count or frequency metadata.\n", stderr);
        return 1;
    }

    /* dgPowerStat accepts no buffer length. Allocate extra space as well as
     * validating the known layout. This is not a compatibility guarantee.
     */
    const size_t buffer_size = 65536 + (size_t)max_cpus * 1024;
    struct power_snapshot *previous = calloc(1, buffer_size);
    struct power_snapshot *current = calloc(1, buffer_size);
    if (!previous || !current) {
        perror("calloc");
        free(previous);
        free(current);
        return 1;
    }
    signal(SIGSYS, unsupported_call);
    int status = 1;
    if (take_snapshot(previous, (unsigned)max_cpus))
        goto done;

    printf("dgPowerStat version=%" PRIu64 ", logical CPUs=%" PRIu32
           ", euid=%u, nominal=%.2f MHz, reference=%.2f MHz\n",
           previous->version, previous->ncpus, (unsigned)geteuid(),
           nominal_hz / 1e6, reference_hz / 1e6);
    fflush(stdout);

    for (long sample = 1; sample <= sample_count; ++sample) {
        struct timespec delay = {interval_ms / 1000, (interval_ms % 1000) * 1000000};
        while (nanosleep(&delay, &delay) != 0) {
            if (errno != EINTR) {
                perror("nanosleep");
                goto done;
            }
        }
        if (take_snapshot(current, (unsigned)max_cpus))
            goto done;
        int valid = current->ncpus == previous->ncpus;
        long double sum_a = 0, sum_m = 0;
        for (uint32_t cpu = 0; valid && cpu < current->ncpus; ++cpu) {
            if (current->cores[cpu].aperf < previous->cores[cpu].aperf ||
                current->cores[cpu].mperf < previous->cores[cpu].mperf) {
                valid = 0;
                break;
            }
            sum_a += current->cores[cpu].aperf - previous->cores[cpu].aperf;
            sum_m += current->cores[cpu].mperf - previous->cores[cpu].mperf;
        }
        if (valid && sum_m > 0) {
            /* Weight by reference cycles, as powermetrics does; do not take
             * an unweighted mean of the per-CPU frequencies.
             */
            const long double hz = reference_hz * sum_a / sum_m;
            printf("Sample %ld: System %.2Lf MHz (%.2Lf%% nominal)",
                   sample, hz / 1e6L, hz * 100 / nominal_hz);
            for (uint32_t cpu = 0; cpu < current->ncpus; ++cpu) {
                uint64_t a = current->cores[cpu].aperf - previous->cores[cpu].aperf;
                uint64_t m = current->cores[cpu].mperf - previous->cores[cpu].mperf;
                if (m)
                    printf(" | CPU%u %.2Lf", cpu, (long double)reference_hz * a / m / 1e6L);
                else
                    printf(" | CPU%u idle", cpu);
            }
            putchar('\n');
        } else {
            printf("Sample %ld: %s; resetting baseline\n", sample,
                   valid ? "no active reference cycles" : "CPU count or counters changed");
        }
        fflush(stdout);
        struct power_snapshot *swap = previous;
        previous = current;
        current = swap;
    }
    status = 0;
done:
    free(previous);
    free(current);
    return status;
}
