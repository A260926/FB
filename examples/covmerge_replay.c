/*
 * covmerge_replay - benign local multi-process coverage merge demonstration.
 *
 * The parent supplies a MAP_SHARED anonymous region.  Two workers register
 * ordinary 8bit counter ranges, bind stable module IDs, and use pipe barriers
 * to order one timely reclaim and one epoch-stale reclaim.  All counter
 * submission goes through the real instrument8BitCountersCount() entry point.
 */

#include "libhfuzz/covmerge.h"
#include "libhfuzz/instrument.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern void __sanitizer_cov_8bit_counters_init(char *start, char *end);

#define REPLAY_A_HI UINT64_C(0x5245504c41590001)
#define REPLAY_A_LO UINT64_C(0x0000000000000001)
#define REPLAY_B_HI UINT64_C(0x5245504c41590002)
#define REPLAY_B_LO UINT64_C(0x0000000000000002)

#define A_POSITIONS 4U
#define B_POSITIONS 2U

static int read_byte(int fd, char *value) {
    for (;;) {
        ssize_t n = read(fd, value, 1);
        if (n == 1) {
            return 0;
        }
        if (n < 0) {
            return -1;
        }
    }
}

static int write_byte(int fd, char value) {
    ssize_t n = write(fd, &value, 1);
    return n == 1 ? 0 : -1;
}

static int replay_worker(int role, void *memory, size_t bytes,
    int command_fd, int event_fd) {
    if (!hfCovMergeAttach(memory, bytes)) {
        return 10;
    }

    uint8_t module_a[A_POSITIONS] = {0};
    uint8_t module_b[B_POSITIONS] = {0};

    __sanitizer_cov_8bit_counters_init(
        (char *)module_a, (char *)module_a + A_POSITIONS);
    __sanitizer_cov_8bit_counters_init(
        (char *)module_b, (char *)module_b + B_POSITIONS);

    if (!hfCovMergeBind((char *)module_a, (char *)module_a + A_POSITIONS,
            REPLAY_A_HI, REPLAY_A_LO) ||
        !hfCovMergeBind((char *)module_b, (char *)module_b + B_POSITIONS,
            REPLAY_B_HI, REPLAY_B_LO)) {
        return 11;
    }

    /* Epoch 1: role 0 is timely; role 1 is deliberately released late. */
    if (hfCovMergeBegin() != 1U) {
        return 12;
    }

    if (role == 0) {
        module_a[0] = 30U;
        module_a[1] = 60U;
        module_a[2] = 90U;
        module_a[3] = 120U;
        module_b[0] = 1U;
        module_b[1] = 2U;
    } else {
        module_a[0] = 200U;
        module_a[1] = 0U;
        module_a[2] = 7U;
        module_a[3] = 255U;
        module_b[0] = 9U;
        module_b[1] = 9U;
    }

    if (write_byte(event_fd, 'R') != 0) {
        return 13;
    }
    char command = 0;
    if (read_byte(command_fd, &command) != 0 || command != 'G') {
        return 14;
    }

    instrument8BitCountersCount();
    if (write_byte(event_fd, 'D') != 0) {
        return 15;
    }

    /* Epoch 2 begins only after the parent has advanced the shared epoch. */
    if (read_byte(command_fd, &command) != 0 || command != 'B') {
        return 16;
    }
    if (hfCovMergeBegin() != 2U) {
        return 17;
    }

    if (role == 0) {
        module_a[0] = 200U;
        module_a[1] = 1U;
        module_a[2] = 100U;
        module_a[3] = 255U;
        module_b[0] = 200U;
        module_b[1] = 200U;
    } else {
        module_a[0] = 100U;
        module_a[1] = 200U;
        module_a[2] = 200U;
        module_a[3] = 1U;
        module_b[0] = 100U;
        module_b[1] = 100U;
    }

    if (write_byte(event_fd, 'R') != 0) {
        return 18;
    }
    if (read_byte(command_fd, &command) != 0 || command != 'G') {
        return 19;
    }

    instrument8BitCountersCount();
    if (write_byte(event_fd, 'D') != 0) {
        return 20;
    }

    hfCovMergeDetach();
    return 0;
}

static bool read_all_positions(
    uint8_t *a, uint8_t *b) {
    for (size_t i = 0; i < A_POSITIONS; i++) {
        if (!hfCovMergeRead(REPLAY_A_HI, REPLAY_A_LO, i, &a[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < B_POSITIONS; i++) {
        if (!hfCovMergeRead(REPLAY_B_HI, REPLAY_B_LO, i, &b[i])) {
            return false;
        }
    }
    return true;
}

static void write_positions(FILE *out, const char *label,
    const uint8_t *a, const uint8_t *b) {
    fprintf(out, "%s module=A positions=%u values=[", label, A_POSITIONS);
    for (size_t i = 0; i < A_POSITIONS; i++) {
        fprintf(out, "%s%u", i ? "," : "", a[i]);
    }
    fprintf(out, "]\n");

    fprintf(out, "%s module=B positions=%u values=[", label, B_POSITIONS);
    for (size_t i = 0; i < B_POSITIONS; i++) {
        fprintf(out, "%s%u", i ? "," : "", b[i]);
    }
    fprintf(out, "]\n");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTPUT_DIRECTORY\n", argv[0]);
        return 2;
    }

    const char *output_dir = argv[1];
    mkdir(output_dir, 0755); /* Ignore EEXIST; the harness creates it too. */

    char transcript_path[4096];
    int path_len = snprintf(
        transcript_path, sizeof(transcript_path), "%s/transcript.txt", output_dir);
    if (path_len <= 0 || (size_t)path_len >= sizeof(transcript_path)) {
        fprintf(stderr, "output path is too long\n");
        return 2;
    }

    size_t bytes = hfCovMergeSize();
    void *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) {
        perror("mmap");
        return 3;
    }
    if (!hfCovMergeInit(memory, bytes, 1U) || !hfCovMergeAttach(memory, bytes)) {
        fprintf(stderr, "hfCovMergeInit/Attach failed\n");
        return 4;
    }

    int command_pipe[2][2] = {{-1, -1}, {-1, -1}};
    int event_pipe[2] = {-1, -1};
    if (pipe(command_pipe[0]) != 0 || pipe(command_pipe[1]) != 0 ||
        pipe(event_pipe) != 0) {
        perror("pipe");
        return 5;
    }

    pid_t workers[2];
    for (int role = 0; role < 2; role++) {
        workers[role] = fork();
        if (workers[role] < 0) {
            perror("fork");
            return 6;
        }
        if (workers[role] == 0) {
            int rc = replay_worker(role, memory, bytes,
                command_pipe[role][0], event_pipe[1]);
            _exit(rc);
        }
    }

    char event = 0;
    for (int i = 0; i < 2; i++) {
        if (read_byte(event_pipe[0], &event) != 0 || event != 'R') {
            fprintf(stderr, "workers did not reach round 1 barrier\n");
            return 7;
        }
    }

    /* Worker 0 is the timely epoch-1 report. */
    if (write_byte(command_pipe[0][1], 'G') != 0 ||
        read_byte(event_pipe[0], &event) != 0 || event != 'D') {
        fprintf(stderr, "normal round barrier failed\n");
        return 8;
    }

    uint8_t normal_a[A_POSITIONS] = {0};
    uint8_t normal_b[B_POSITIONS] = {0};
    if (!read_all_positions(normal_a, normal_b)) {
        fprintf(stderr, "normal round Read failed\n");
        return 9;
    }

    if (!hfCovMergeAdvance(2U)) {
        fprintf(stderr, "hfCovMergeAdvance(2) failed\n");
        return 10;
    }

    /* Worker 1 still owns epoch 1; releasing it now must be discarded. */
    if (write_byte(command_pipe[1][1], 'G') != 0 ||
        read_byte(event_pipe[0], &event) != 0 || event != 'D') {
        fprintf(stderr, "late round barrier failed\n");
        return 11;
    }

    uint8_t late_a[A_POSITIONS] = {0};
    uint8_t late_b[B_POSITIONS] = {0};
    if (!read_all_positions(late_a, late_b)) {
        fprintf(stderr, "late-round Read failed\n");
        return 12;
    }

    /* Start epoch 2 only after Advance and the stale reclaim have completed. */
    if (write_byte(command_pipe[0][1], 'B') != 0 ||
        write_byte(command_pipe[1][1], 'B') != 0) {
        fprintf(stderr, "failed to start epoch 2\n");
        return 13;
    }
    for (int i = 0; i < 2; i++) {
        if (read_byte(event_pipe[0], &event) != 0 || event != 'R') {
            fprintf(stderr, "workers did not reach round 2 barrier\n");
            return 14;
        }
    }

    if (write_byte(command_pipe[0][1], 'G') != 0 ||
        write_byte(command_pipe[1][1], 'G') != 0) {
        fprintf(stderr, "failed to release round 2\n");
        return 15;
    }
    for (int i = 0; i < 2; i++) {
        if (read_byte(event_pipe[0], &event) != 0 || event != 'D') {
            fprintf(stderr, "round 2 completion barrier failed\n");
            return 16;
        }
    }

    for (int role = 0; role < 2; role++) {
        int status = 0;
        if (waitpid(workers[role], &status, 0) < 0 ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "worker %d failed\n", role);
            return 17;
        }
    }

    uint8_t final_a[A_POSITIONS] = {0};
    uint8_t final_b[B_POSITIONS] = {0};
    if (!read_all_positions(final_a, final_b)) {
        fprintf(stderr, "final Read failed\n");
        return 18;
    }

    static const uint8_t expected_late_a[A_POSITIONS] = {0, 0, 0, 0};
    static const uint8_t expected_late_b[B_POSITIONS] = {0, 0};
    static const uint8_t expected_final_a[A_POSITIONS] = {255, 201, 255, 255};
    static const uint8_t expected_final_b[B_POSITIONS] = {255, 255};

    bool ok = memcmp(late_a, expected_late_a, sizeof(late_a)) == 0 &&
              memcmp(late_b, expected_late_b, sizeof(late_b)) == 0 &&
              memcmp(final_a, expected_final_a, sizeof(final_a)) == 0 &&
              memcmp(final_b, expected_final_b, sizeof(final_b)) == 0 &&
              normal_a[0] == 30U && normal_a[3] == 120U &&
              normal_b[0] == 1U && normal_b[1] == 2U;

    FILE *transcript = fopen(transcript_path, "w");
    if (transcript == NULL) {
        perror("transcript fopen");
        return 19;
    }

    fprintf(transcript, "hfCovMerge replay transcript\n");
    fprintf(transcript, "module=A id_hi=0x%016" PRIx64 " id_lo=0x%016" PRIx64
        " positions=%u\n", REPLAY_A_HI, REPLAY_A_LO, A_POSITIONS);
    fprintf(transcript, "module=B id_hi=0x%016" PRIx64 " id_lo=0x%016" PRIx64
        " positions=%u\n", REPLAY_B_HI, REPLAY_B_LO, B_POSITIONS);

    write_positions(transcript, "round=1-timely-worker=0", normal_a, normal_b);
    fprintf(transcript, "round=1 result=recorded-before-advance epoch=1\n");
    fprintf(transcript, "advance old_epoch=1 new_epoch=2 result=ok shared_counts_cleared=true\n");
    write_positions(transcript, "round=1-late-worker=1", late_a, late_b);
    fprintf(transcript, "round=1-late result=discarded stale_epoch=1 current_epoch=2\n");
    write_positions(transcript, "round=2-merged-workers=0+1", final_a, final_b);
    fprintf(transcript, "round=2 result=saturated-merge epoch=2\n");

    for (size_t i = 0; i < A_POSITIONS; i++) {
        fprintf(transcript, "position module=A index=%zu timely=%u late_after_advance=0 final=%u\n",
            i, normal_a[i], final_a[i]);
    }
    for (size_t i = 0; i < B_POSITIONS; i++) {
        fprintf(transcript, "position module=B index=%zu timely=%u late_after_advance=0 final=%u\n",
            i, normal_b[i], final_b[i]);
    }

    fprintf(transcript, "overall=%s\n", ok ? "PASS" : "FAIL");
    fclose(transcript);

    hfCovMergeDetach();
    munmap(memory, bytes);
    return ok ? 0 : 20;
}
