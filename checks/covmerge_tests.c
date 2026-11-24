#include "task_support/check.h"

#include "libhfuzz/covmerge.h"
#include "libhfuzz/instrument.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

extern void __sanitizer_cov_8bit_counters_init(char *start, char *end);

#define ID_A_HI UINT64_C(0x00000000A1A10001)
#define ID_A_LO UINT64_C(0x0000000000000001)
#define ID_B_HI UINT64_C(0x00000000B2B20002)
#define ID_B_LO UINT64_C(0x0000000000000002)
#define ID_SHARED_HI UINT64_C(0x0000000053480009)
#define ID_SHARED_LO UINT64_C(0x0000000000000009)

/*
 * instrument8BitCountersCount() retains every registered range for the life
 * of the process, so counters that will be reclaimed must have static storage.
 */
static uint8_t disabled_counters[3] = {1U, 2U, 3U};
static uint8_t reverse_a[4];
static uint8_t reverse_b[3];
static uint8_t same_id_a[1];
static uint8_t same_id_b[1];
static uint8_t rule_counters[8];
static uint8_t epoch_a[4];
static uint8_t epoch_b[2];
static uint8_t late_counter[1];
static uint8_t repeat_counter[1];

static void register_static_ranges(void) {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    __sanitizer_cov_8bit_counters_init(
        (char *)disabled_counters, (char *)disabled_counters + sizeof(disabled_counters));
    __sanitizer_cov_8bit_counters_init((char *)reverse_a, (char *)reverse_a + sizeof(reverse_a));
    __sanitizer_cov_8bit_counters_init((char *)reverse_b, (char *)reverse_b + sizeof(reverse_b));
    __sanitizer_cov_8bit_counters_init((char *)same_id_a, (char *)same_id_a + sizeof(same_id_a));
    __sanitizer_cov_8bit_counters_init((char *)same_id_b, (char *)same_id_b + sizeof(same_id_b));
    __sanitizer_cov_8bit_counters_init((char *)rule_counters, (char *)rule_counters + sizeof(rule_counters));
    __sanitizer_cov_8bit_counters_init((char *)epoch_a, (char *)epoch_a + sizeof(epoch_a));
    __sanitizer_cov_8bit_counters_init((char *)epoch_b, (char *)epoch_b + sizeof(epoch_b));
    __sanitizer_cov_8bit_counters_init((char *)late_counter, (char *)late_counter + sizeof(late_counter));
    __sanitizer_cov_8bit_counters_init((char *)repeat_counter, (char *)repeat_counter + sizeof(repeat_counter));
}

static bool make_mapping(void **memory, size_t *bytes) {
    *bytes = hfCovMergeSize();
    *memory = mmap(NULL, *bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    return *memory != MAP_FAILED;
}

static int test_disabled_without_attach(void) {
    hfCovMergeDetach();
    register_static_ranges();

    TASK_CHECK(hfCovMergeBegin() == 0U);
    TASK_CHECK(!hfCovMergeAdvance(1U));
    TASK_CHECK(!hfCovMergeBind((char *)reverse_a, (char *)reverse_a + 1, ID_A_HI, ID_A_LO));

    uint8_t out = 0xA5U;
    TASK_CHECK(!hfCovMergeRead(ID_A_HI, ID_A_LO, 0U, &out));
    TASK_CHECK(out == 0xA5U);

    disabled_counters[0] = 1U;
    disabled_counters[1] = 2U;
    disabled_counters[2] = 3U;
    instrument8BitCountersCount();
    TASK_CHECK(disabled_counters[0] == 0U);
    TASK_CHECK(disabled_counters[1] == 0U);
    TASK_CHECK(disabled_counters[2] == 0U);

    return 0;
}

static int test_init_validation(void) {
    size_t bytes = 0;
    void *memory = MAP_FAILED;
    TASK_CHECK(make_mapping(&memory, &bytes));

    TASK_CHECK(!hfCovMergeInit(NULL, bytes, 1U));
    TASK_CHECK(!hfCovMergeInit(memory, 0U, 1U));
    TASK_CHECK(!hfCovMergeAttach(memory, bytes));

    memset(memory, 0xAB, bytes);
    TASK_CHECK(!hfCovMergeInit(memory, bytes - 1U, 1U));
    TASK_CHECK(((const unsigned char *)memory)[0] == 0xABU);
    TASK_CHECK(((const unsigned char *)memory)[137 % bytes] == 0xABU);

    TASK_CHECK(!hfCovMergeInit(memory, bytes, 0U));
    TASK_CHECK(((const unsigned char *)memory)[0] == 0xABU);

    TASK_CHECK(hfCovMergeInit(memory, bytes, 9U));
    TASK_CHECK(hfCovMergeAttach(memory, bytes));
    TASK_CHECK(hfCovMergeBegin() == 9U);

    /* A live initialized region is not silently reinitialized. */
    TASK_CHECK(!hfCovMergeInit(memory, bytes, 10U));
    TASK_CHECK(hfCovMergeBegin() == 9U);

    hfCovMergeDetach();
    TASK_CHECK(munmap(memory, bytes) == 0);
    return 0;
}

static int test_reverse_registration(void) {
    register_static_ranges();

    for (int reverse = 0; reverse < 2; reverse++) {
        size_t bytes = 0;
        void *memory = MAP_FAILED;
        TASK_CHECK(make_mapping(&memory, &bytes));
        TASK_CHECK(hfCovMergeInit(memory, bytes, 1U));
        TASK_CHECK(hfCovMergeAttach(memory, bytes));

        if (!reverse) {
            TASK_CHECK(hfCovMergeBind((char *)reverse_a, (char *)reverse_a + sizeof(reverse_a), ID_A_HI, ID_A_LO));
            TASK_CHECK(hfCovMergeBind((char *)reverse_b, (char *)reverse_b + sizeof(reverse_b), ID_B_HI, ID_B_LO));
        } else {
            TASK_CHECK(hfCovMergeBind((char *)reverse_b, (char *)reverse_b + sizeof(reverse_b), ID_B_HI, ID_B_LO));
            TASK_CHECK(hfCovMergeBind((char *)reverse_a, (char *)reverse_a + sizeof(reverse_a), ID_A_HI, ID_A_LO));
        }

        TASK_CHECK(hfCovMergeBegin() == 1U);
        reverse_a[0] = 11U;
        reverse_a[1] = 22U;
        reverse_a[2] = 33U;
        reverse_a[3] = 44U;
        reverse_b[0] = 55U;
        reverse_b[1] = 66U;
        reverse_b[2] = 77U;
        instrument8BitCountersCount();

        uint8_t out = 0U;
        static const uint8_t expected_a[4] = {11U, 22U, 33U, 44U};
        static const uint8_t expected_b[3] = {55U, 66U, 77U};
        for (size_t i = 0; i < 4U; i++) {
            TASK_CHECK(hfCovMergeRead(ID_A_HI, ID_A_LO, i, &out));
            TASK_CHECK(out == expected_a[i]);
        }
        for (size_t i = 0; i < 3U; i++) {
            TASK_CHECK(hfCovMergeRead(ID_B_HI, ID_B_LO, i, &out));
            TASK_CHECK(out == expected_b[i]);
        }

        hfCovMergeDetach();
        TASK_CHECK(munmap(memory, bytes) == 0);
    }

    return 0;
}

static int test_binding_rules(void) {
    size_t bytes = 0;
    void *memory = MAP_FAILED;
    TASK_CHECK(make_mapping(&memory, &bytes));
    TASK_CHECK(hfCovMergeInit(memory, bytes, 1U));
    TASK_CHECK(hfCovMergeAttach(memory, bytes));
    register_static_ranges();

    char stack_buf[16];
    TASK_CHECK(!hfCovMergeBind(NULL, stack_buf, 1U, 1U));
    TASK_CHECK(!hfCovMergeBind(stack_buf, NULL, 1U, 1U));
    TASK_CHECK(!hfCovMergeBind(stack_buf, stack_buf, 1U, 1U));
    TASK_CHECK(!hfCovMergeBind(stack_buf + 1, stack_buf, 1U, 1U));
    TASK_CHECK(!hfCovMergeBind(stack_buf, stack_buf + 1, 0U, 0U));

    char long_buf[HF_COVMERGE_MODULE_POSITIONS + 1U];
    TASK_CHECK(!hfCovMergeBind(long_buf, long_buf + sizeof(long_buf), 1U, 100U));

    TASK_CHECK(hfCovMergeBind(
        (char *)rule_counters, (char *)rule_counters + 4, ID_SHARED_HI, ID_SHARED_LO));

    /* Exact range with a changed identity is rejected. */
    TASK_CHECK(!hfCovMergeBind(
        (char *)rule_counters, (char *)rule_counters + 4, ID_A_HI, ID_A_LO));

    /* Partial overlap is rejected. */
    TASK_CHECK(!hfCovMergeBind(
        (char *)rule_counters + 2, (char *)rule_counters + 6, ID_B_HI, ID_B_LO));

    /* Same stable ID requires the same length. */
    char conflict_buf[8];
    TASK_CHECK(!hfCovMergeBind(conflict_buf, conflict_buf + 8, ID_SHARED_HI, ID_SHARED_LO));

    TASK_CHECK(hfCovMergeBegin() == 1U);
    rule_counters[0] = 37U;
    instrument8BitCountersCount();

    uint8_t out = 0U;
    TASK_CHECK(hfCovMergeRead(ID_SHARED_HI, ID_SHARED_LO, 0U, &out));
    TASK_CHECK(out == 37U);
    TASK_CHECK(hfCovMergeRead(ID_SHARED_HI, ID_SHARED_LO, 3U, &out));
    TASK_CHECK(out == 0U);
    out = 0x5AU;
    TASK_CHECK(!hfCovMergeRead(ID_SHARED_HI, ID_SHARED_LO, 4U, &out));
    TASK_CHECK(out == 0x5AU);
    TASK_CHECK(!hfCovMergeRead(ID_B_HI, ID_B_LO, 0U, &out));
    TASK_CHECK(out == 0x5AU);
    TASK_CHECK(!hfCovMergeRead(ID_SHARED_HI, ID_SHARED_LO, 0U, NULL));

    /* A later reclaim without Begin does not duplicate the committed round. */
    instrument8BitCountersCount();
    TASK_CHECK(hfCovMergeRead(ID_SHARED_HI, ID_SHARED_LO, 0U, &out));
    TASK_CHECK(out == 37U);

    hfCovMergeDetach();
    TASK_CHECK(munmap(memory, bytes) == 0);
    return 0;
}

static int test_same_id_distinct_ranges(void) {
    size_t bytes = 0;
    void *memory = MAP_FAILED;
    TASK_CHECK(make_mapping(&memory, &bytes));
    TASK_CHECK(hfCovMergeInit(memory, bytes, 1U));
    TASK_CHECK(hfCovMergeAttach(memory, bytes));
    register_static_ranges();

    TASK_CHECK(hfCovMergeBind((char *)same_id_a, (char *)same_id_a + 1, ID_SHARED_HI, ID_SHARED_LO));
    TASK_CHECK(hfCovMergeBind((char *)same_id_a, (char *)same_id_a + 1, ID_SHARED_HI, ID_SHARED_LO));
    TASK_CHECK(hfCovMergeBind((char *)same_id_b, (char *)same_id_b + 1, ID_SHARED_HI, ID_SHARED_LO));

    TASK_CHECK(hfCovMergeBegin() == 1U);
    same_id_a[0] = 10U;
    same_id_b[0] = 20U;
    instrument8BitCountersCount();

    uint8_t out = 0U;
    TASK_CHECK(hfCovMergeRead(ID_SHARED_HI, ID_SHARED_LO, 0U, &out));
    TASK_CHECK(out == 30U);

    hfCovMergeDetach();
    TASK_CHECK(munmap(memory, bytes) == 0);
    return 0;
}

static int test_module_limit(void) {
    size_t bytes = 0;
    void *memory = MAP_FAILED;
    TASK_CHECK(make_mapping(&memory, &bytes));
    TASK_CHECK(hfCovMergeInit(memory, bytes, 1U));
    TASK_CHECK(hfCovMergeAttach(memory, bytes));

    char full_module[HF_COVMERGE_MODULE_POSITIONS];
    TASK_CHECK(hfCovMergeBind(full_module,
        full_module + HF_COVMERGE_MODULE_POSITIONS, UINT64_C(0x4242), UINT64_C(0x4242)));

    char slots[HF_COVMERGE_MAX_MODULES + 1U][1];
    for (unsigned i = 0; i < HF_COVMERGE_MAX_MODULES - 1U; i++) {
        TASK_CHECK(hfCovMergeBind(
            slots[i], slots[i] + 1, UINT64_C(0x1000) + i, UINT64_C(0x2000) + i));
    }
    TASK_CHECK(!hfCovMergeBind(
        slots[HF_COVMERGE_MAX_MODULES - 1U],
        slots[HF_COVMERGE_MAX_MODULES - 1U] + 1,
        UINT64_C(0x9999), UINT64_C(0x9999)));

    uint8_t out = 0xEEU;
    for (unsigned i = 0; i < HF_COVMERGE_MAX_MODULES - 1U; i++) {
        TASK_CHECK(hfCovMergeRead(UINT64_C(0x1000) + i, UINT64_C(0x2000) + i, 0U, &out));
        TASK_CHECK(out == 0U);
    }
    TASK_CHECK(hfCovMergeRead(0x4242U, 0x4242U, HF_COVMERGE_MODULE_POSITIONS - 1U, &out));
    TASK_CHECK(out == 0U);
    out = 0xEEU;
    TASK_CHECK(!hfCovMergeRead(0x4242U, 0x4242U, HF_COVMERGE_MODULE_POSITIONS, &out));
    TASK_CHECK(out == 0xEEU);
    TASK_CHECK(!hfCovMergeRead(0x9999U, 0x9999U, 0U, &out));
    TASK_CHECK(out == 0xEEU);

    hfCovMergeDetach();
    TASK_CHECK(munmap(memory, bytes) == 0);
    return 0;
}

static int test_epoch_advance_read(void) {
    size_t bytes = 0;
    void *memory = MAP_FAILED;
    TASK_CHECK(make_mapping(&memory, &bytes));
    TASK_CHECK(hfCovMergeInit(memory, bytes, 42U));
    TASK_CHECK(hfCovMergeAttach(memory, bytes));
    register_static_ranges();

    TASK_CHECK(hfCovMergeBind((char *)epoch_a, (char *)epoch_a + sizeof(epoch_a), ID_A_HI, ID_A_LO));
    TASK_CHECK(hfCovMergeBind((char *)epoch_b, (char *)epoch_b + sizeof(epoch_b), ID_B_HI, ID_B_LO));

    TASK_CHECK(hfCovMergeBegin() == 42U);
    epoch_a[0] = 1U;
    epoch_a[1] = 2U;
    epoch_a[2] = 3U;
    epoch_a[3] = 4U;
    epoch_b[0] = 5U;
    epoch_b[1] = 6U;
    instrument8BitCountersCount();

    uint8_t out = 0U;
    TASK_CHECK(hfCovMergeRead(ID_A_HI, ID_A_LO, 0U, &out) && out == 1U);
    TASK_CHECK(hfCovMergeRead(ID_A_HI, ID_A_LO, 3U, &out) && out == 4U);
    TASK_CHECK(hfCovMergeRead(ID_B_HI, ID_B_LO, 1U, &out) && out == 6U);

    TASK_CHECK(hfCovMergeAdvance(43U));
    TASK_CHECK(hfCovMergeRead(ID_A_HI, ID_A_LO, 0U, &out) && out == 0U);
    TASK_CHECK(hfCovMergeRead(ID_B_HI, ID_B_LO, 1U, &out) && out == 0U);

    TASK_CHECK(!hfCovMergeAdvance(43U));
    TASK_CHECK(!hfCovMergeAdvance(42U));
    TASK_CHECK(!hfCovMergeAdvance(0U));

    TASK_CHECK(hfCovMergeBegin() == 43U);
    epoch_a[0] = 9U;
    instrument8BitCountersCount();
    TASK_CHECK(hfCovMergeRead(ID_A_HI, ID_A_LO, 0U, &out) && out == 9U);

    hfCovMergeDetach();
    TASK_CHECK(munmap(memory, bytes) == 0);
    return 0;
}

static int test_late_reclaim(void) {
    size_t bytes = 0;
    void *memory = MAP_FAILED;
    TASK_CHECK(make_mapping(&memory, &bytes));
    TASK_CHECK(hfCovMergeInit(memory, bytes, 7U));
    TASK_CHECK(hfCovMergeAttach(memory, bytes));
    register_static_ranges();

    TASK_CHECK(hfCovMergeBind((char *)late_counter, (char *)late_counter + 1, ID_A_HI, ID_A_LO));
    TASK_CHECK(hfCovMergeBegin() == 7U);
    late_counter[0] = 111U;

    TASK_CHECK(hfCovMergeAdvance(8U));
    instrument8BitCountersCount(); /* Late epoch-7 report. */
    TASK_CHECK(late_counter[0] == 0U);

    uint8_t out = 99U;
    TASK_CHECK(hfCovMergeRead(ID_A_HI, ID_A_LO, 0U, &out));
    TASK_CHECK(out == 0U);

    TASK_CHECK(hfCovMergeBegin() == 8U);
    late_counter[0] = 12U;
    instrument8BitCountersCount();
    TASK_CHECK(hfCovMergeRead(ID_A_HI, ID_A_LO, 0U, &out));
    TASK_CHECK(out == 12U);

    TASK_CHECK(hfCovMergeAdvance(9U));
    TASK_CHECK(hfCovMergeRead(ID_A_HI, ID_A_LO, 0U, &out));
    TASK_CHECK(out == 0U);

    hfCovMergeDetach();
    TASK_CHECK(munmap(memory, bytes) == 0);
    return 0;
}

static int test_repeat_reclaim(void) {
    size_t bytes = 0;
    void *memory = MAP_FAILED;
    TASK_CHECK(make_mapping(&memory, &bytes));
    TASK_CHECK(hfCovMergeInit(memory, bytes, 1U));
    TASK_CHECK(hfCovMergeAttach(memory, bytes));
    register_static_ranges();

    TASK_CHECK(hfCovMergeBind((char *)repeat_counter, (char *)repeat_counter + 1, ID_A_HI, ID_A_LO));
    TASK_CHECK(hfCovMergeBegin() == 1U);
    repeat_counter[0] = 3U;
    instrument8BitCountersCount();

    uint8_t out = 0U;
    TASK_CHECK(hfCovMergeRead(ID_A_HI, ID_A_LO, 0U, &out) && out == 3U);
    TASK_CHECK(repeat_counter[0] == 0U);

    /* New local hits before the next Begin still belong to the spent round. */
    repeat_counter[0] = 5U;
    instrument8BitCountersCount();
    TASK_CHECK(repeat_counter[0] == 0U);
    TASK_CHECK(hfCovMergeRead(ID_A_HI, ID_A_LO, 0U, &out) && out == 3U);

    instrument8BitCountersCount();
    TASK_CHECK(hfCovMergeRead(ID_A_HI, ID_A_LO, 0U, &out) && out == 3U);

    hfCovMergeDetach();
    TASK_CHECK(munmap(memory, bytes) == 0);
    return 0;
}

static int saturation_worker(void *memory, size_t bytes, int ready_fd, int gate_fd) {
    if (!hfCovMergeAttach(memory, bytes)) {
        return 20;
    }

    uint8_t counter = 0U;
    __sanitizer_cov_8bit_counters_init((char *)&counter, (char *)&counter + 1);
    if (!hfCovMergeBind((char *)&counter, (char *)&counter + 1, 0x7001U, 0x0001U)) {
        return 21;
    }
    if (hfCovMergeBegin() != 1U) {
        return 22;
    }

    counter = 200U;
    if (write(ready_fd, "R", 1) != 1) {
        return 23;
    }

    char gate = 0;
    if (read(gate_fd, &gate, 1) != 1 || gate != 'G') {
        return 24;
    }

    instrument8BitCountersCount();
    if (counter != 0U) {
        return 25;
    }

    hfCovMergeDetach();
    return 0;
}

static int test_process_saturation(void) {
    enum { WORKERS = 8 };

    size_t bytes = hfCovMergeSize();
    void *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    TASK_CHECK(memory != MAP_FAILED);
    TASK_CHECK(hfCovMergeInit(memory, bytes, 1U));

    int ready_pipe[2] = {-1, -1};
    int gate_pipe[2] = {-1, -1};
    TASK_CHECK(pipe(ready_pipe) == 0);
    TASK_CHECK(pipe(gate_pipe) == 0);

    pid_t pids[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        pids[i] = fork();
        TASK_CHECK(pids[i] >= 0);
        if (pids[i] == 0) {
            int rc = saturation_worker(memory, bytes, ready_pipe[1], gate_pipe[0]);
            _exit(rc);
        }
    }

    for (int i = 0; i < WORKERS; i++) {
        char ready = 0;
        ssize_t n = read(ready_pipe[0], &ready, 1);
        TASK_CHECK(n == 1 && ready == 'R');
    }

    for (int i = 0; i < WORKERS; i++) {
        TASK_CHECK(write(gate_pipe[1], "G", 1) == 1);
    }

    for (int i = 0; i < WORKERS; i++) {
        int status = 0;
        TASK_CHECK(waitpid(pids[i], &status, 0) == pids[i]);
        TASK_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    TASK_CHECK(hfCovMergeAttach(memory, bytes));
    uint8_t out = 0U;
    TASK_CHECK(hfCovMergeRead(0x7001U, 0x0001U, 0U, &out));
    TASK_CHECK(out == 255U);

    hfCovMergeDetach();
    close(ready_pipe[0]);
    close(ready_pipe[1]);
    close(gate_pipe[0]);
    close(gate_pipe[1]);
    TASK_CHECK(munmap(memory, bytes) == 0);
    return 0;
}

static int late_pair_worker(
    int role, void *memory, size_t bytes, int command_fd, int event_fd) {
    if (!hfCovMergeAttach(memory, bytes)) {
        return 40;
    }

    uint8_t counter[1] = {0U};
    __sanitizer_cov_8bit_counters_init((char *)counter, (char *)counter + 1);
    if (!hfCovMergeBind((char *)counter, (char *)counter + 1, ID_A_HI, ID_A_LO)) {
        return 41;
    }

    if (hfCovMergeBegin() != 1U) {
        return 42;
    }
    counter[0] = (role == 0) ? 40U : 60U;
    if (write(event_fd, "R", 1) != 1) {
        return 43;
    }

    char command = 0;
    if (read(command_fd, &command, 1) != 1 || command != 'G') {
        return 44;
    }
    instrument8BitCountersCount();
    if (counter[0] != 0U || write(event_fd, "D", 1) != 1) {
        return 45;
    }

    if (read(command_fd, &command, 1) != 1 || command != 'B') {
        return 46;
    }
    if (hfCovMergeBegin() != 2U) {
        return 47;
    }
    counter[0] = (role == 0) ? 3U : 4U;
    if (write(event_fd, "R", 1) != 1) {
        return 48;
    }

    if (read(command_fd, &command, 1) != 1 || command != 'G') {
        return 49;
    }
    instrument8BitCountersCount();
    if (write(event_fd, "D", 1) != 1) {
        return 50;
    }

    hfCovMergeDetach();
    return 0;
}

static int test_process_normal_and_late(void) {
    size_t bytes = hfCovMergeSize();
    void *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    TASK_CHECK(memory != MAP_FAILED);
    TASK_CHECK(hfCovMergeInit(memory, bytes, 1U));

    int command_pipe[2][2] = {{-1, -1}, {-1, -1}};
    int event_pipe[2] = {-1, -1};
    TASK_CHECK(pipe(command_pipe[0]) == 0);
    TASK_CHECK(pipe(command_pipe[1]) == 0);
    TASK_CHECK(pipe(event_pipe) == 0);

    pid_t workers[2];
    for (int role = 0; role < 2; role++) {
        workers[role] = fork();
        TASK_CHECK(workers[role] >= 0);
        if (workers[role] == 0) {
            int rc = late_pair_worker(
                role, memory, bytes, command_pipe[role][0], event_pipe[1]);
            _exit(rc);
        }
    }

    TASK_CHECK(hfCovMergeAttach(memory, bytes));

    for (int i = 0; i < 2; i++) {
        char event = 0;
        TASK_CHECK(read(event_pipe[0], &event, 1) == 1 && event == 'R');
    }

    TASK_CHECK(write(command_pipe[0][1], "G", 1) == 1);
    char event = 0;
    TASK_CHECK(read(event_pipe[0], &event, 1) == 1 && event == 'D');

    uint8_t out = 0U;
    TASK_CHECK(hfCovMergeRead(ID_A_HI, ID_A_LO, 0U, &out));
    TASK_CHECK(out == 40U);

    TASK_CHECK(hfCovMergeAdvance(2U));

    /* Worker 1's epoch-1 report is released only after the Advance. */
    TASK_CHECK(write(command_pipe[1][1], "G", 1) == 1);
    TASK_CHECK(read(event_pipe[0], &event, 1) == 1 && event == 'D');
    TASK_CHECK(hfCovMergeRead(ID_A_HI, ID_A_LO, 0U, &out));
    TASK_CHECK(out == 0U);

    TASK_CHECK(write(command_pipe[0][1], "B", 1) == 1);
    TASK_CHECK(write(command_pipe[1][1], "B", 1) == 1);
    for (int i = 0; i < 2; i++) {
        TASK_CHECK(read(event_pipe[0], &event, 1) == 1 && event == 'R');
    }

    TASK_CHECK(write(command_pipe[0][1], "G", 1) == 1);
    TASK_CHECK(write(command_pipe[1][1], "G", 1) == 1);
    for (int i = 0; i < 2; i++) {
        TASK_CHECK(read(event_pipe[0], &event, 1) == 1 && event == 'D');
    }

    for (int role = 0; role < 2; role++) {
        int status = 0;
        TASK_CHECK(waitpid(workers[role], &status, 0) == workers[role]);
        TASK_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    TASK_CHECK(hfCovMergeRead(ID_A_HI, ID_A_LO, 0U, &out));
    TASK_CHECK(out == 7U);

    hfCovMergeDetach();
    TASK_CHECK(munmap(memory, bytes) == 0);
    return 0;
}

const struct task_case task_cases[] = {
    {"modules", "disabled_without_attach", test_disabled_without_attach},
    {"modules", "init_size_epoch_and_flags", test_init_validation},
    {"modules", "reverse_module_registration", test_reverse_registration},
    {"modules", "binding_rejections_keep_state", test_binding_rules},
    {"modules", "same_id_ranges_and_idempotent_bind", test_same_id_distinct_ranges},
    {"modules", "sixteen_module_limit", test_module_limit},
    {"epochs", "begin_advance_and_read", test_epoch_advance_read},
    {"epochs", "late_reclaim_is_discarded", test_late_reclaim},
    {"epochs", "repeat_reclaim_is_not_another_round", test_repeat_reclaim},
    {"processes", "concurrent_addition_saturation", test_process_saturation},
    {"processes", "normal_and_late_fork_workers", test_process_normal_and_late},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
