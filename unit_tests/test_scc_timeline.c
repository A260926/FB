#include "task_support/check.h"

#include "caption.h"
#include "scc_timeline.h"
#include "vtt.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// All expected cue boundaries below are integer transmission frames at
// 30000/1001 fps, computed by hand from the SCC timecodes:
//   non-drop: frame = (hh*3600 + mm*60 + ss) * 30 + ff
//   drop:     frame -= 2 * (total_minutes - total_minutes / 10)
//
// Hand-written control words (EIA-608 with odd parity):
//   9420 = RCL (pop-on load)   9429 = RDC (paint-on)   942f = EOC (flip)
//   942c = EDM (erase display) 8080 = padding
//   c8e9 = "Hi"  a1a1 = "!!"  c8e5 = "He"  ecec = "ll"  efa1 = "o!"  c7ef = "Go"
//   94e0 = preamble row 14 white   9461 = preamble row 14 white + underline

#define SCC_HEADER "Scenarist_SCC V1.0\n"

static double frame_seconds(uint64_t frame) { return ((double)frame * 1001.0) / 30000.0; }

static size_t count_cues(vtt_t* vtt)
{
    size_t count = 0;

    for (vtt_block_t* cue = vtt->cue_head; cue; cue = cue->next) {
        ++count;
    }

    return count;
}

static vtt_block_t* cue_at(vtt_t* vtt, size_t index)
{
    for (vtt_block_t* cue = vtt->cue_head; cue; cue = cue->next) {
        if (0 == index--) {
            return cue;
        }
    }

    return 0;
}

static int check_cue(vtt_block_t* cue, uint64_t start_frame, uint64_t end_frame, const char* text)
{
    if (!cue) {
        return 0;
    }

    double timestamp = frame_seconds(start_frame);
    double duration = frame_seconds(end_frame) - timestamp;

    if (1e-9 < fabs(cue->timestamp - timestamp) || 1e-9 < fabs(cue->duration - duration)) {
        return 0;
    }

    return 0 == strcmp(vtt_block_data(cue), text);
}

static int check_rejection(const char* scc, size_t size, scc_timeline_error_t want_error, size_t want_line)
{
    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc, size, 0, 100000, &error, &error_line);

    if (vtt) {
        vtt_free(vtt);
        return 0;
    }

    return error == want_error && error_line == want_line;
}

#define REJECT(scc, want_error, want_line) check_rejection((scc), strlen(scc), (want_error), (want_line))

////////////////////////////////////////////////////////////////////////////////
// clock
////////////////////////////////////////////////////////////////////////////////

static int case_clock_colon_nondrop(void)
{
    // ':' timecodes, plain non-drop conversion
    const char* scc = SCC_HEADER "\n"
                                 "00:00:01:00  9420 c8e9 942f\n" // RCL@30 "Hi"@31 EOC@32
                                 "00:00:02:00  942c\n"; // EDM@60

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc, strlen(scc), 0, 1000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(error == SCC_TIMELINE_OK && error_line == 0);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 32, 60, "Hi\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_clock_colon_one_hour(void)
{
    const char* scc = SCC_HEADER "\n"
                                 "01:00:00:00  9420 c8e9 942f\n" // RCL@108000 "Hi"@108001 EOC@108002
                                 "01:00:01:00  942c\n"; // EDM@108030

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc, strlen(scc), 108000, 108100, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 108002, 108030, "Hi\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_clock_drop_one_minute(void)
{
    // drop-frame: 00:01:00;02 is the first legal label of minute 1 -> frame 1800
    const char* scc = SCC_HEADER "\n"
                                 "00:00:00;00  9420 c8e9\n" // RCL@0 "Hi"@1 (back buffer)
                                 "00:01:00;02  942f\n" // EOC@1800
                                 "00:01:00;03  942c\n"; // EDM@1801

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc, strlen(scc), 0, 100000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 1800, 1801, "Hi\r\n"));
    vtt_free(vtt);

    // 00:00:59;29 -> frame 1799, so the EOC lands on 1801; the window closes the tail
    const char* tail = SCC_HEADER "\n"
                                  "00:00:59;29  9420 c8e9 942f\n"; // RCL@1799 "Hi"@1800 EOC@1801

    vtt = scc_timeline_to_vtt(tail, strlen(tail), 0, 2000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 1801, 2000, "Hi\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_clock_drop_ten_minute(void)
{
    // ten-minute marks skip no labels: 00:10:00;00 -> 18000 - 18 = 17982
    const char* scc = SCC_HEADER "\n"
                                 "00:09:59;29  9420\n" // RCL@17981
                                 "00:10:00;00  c8e9 942f\n" // "Hi"@17982 EOC@17983
                                 "00:10:00;02  942c\n"; // EDM@17984

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc, strlen(scc), 0, 100000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 17983, 17984, "Hi\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_clock_header_only(void)
{
    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;

    // header without a trailing newline, no caption lines at all
    const char* bare = "Scenarist_SCC V1.0";
    vtt_t* vtt = scc_timeline_to_vtt(bare, strlen(bare), 0, 1000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 0);
    vtt_free(vtt);

    // header followed by blank lines only
    const char* blanks = "Scenarist_SCC V1.0\n\n\n\n";
    vtt = scc_timeline_to_vtt(blanks, strlen(blanks), 0, 1000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 0);
    vtt_free(vtt);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// display
////////////////////////////////////////////////////////////////////////////////

static int case_display_popon_cross_line_eoc(void)
{
    // pop-on text is staged in the back buffer across two lines before the
    // window; only the EOC inside the window publishes it
    const char* scc = SCC_HEADER "\n"
                                 "00:00:00:00  9420 c8e9\n" // RCL@0, "Hi"->back@1
                                 "00:00:01:00  9420 a1a1\n" // RCL@30, "!!"->back@31
                                 "00:00:02:00  942f\n" // EOC@60 -> "Hi!!" visible
                                 "00:00:03:00  942c\n"; // EDM@90 -> blank

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc, strlen(scc), 31, 90, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 60, 90, "Hi!!\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_display_painton_per_word(void)
{
    // paint-on: every word takes effect on its own transmission frame
    const char* scc = SCC_HEADER "\n"
                                 "00:00:00:00  9429 c8e5 ecec efa1\n" // RDC@0 "He"@1 "ll"@2 "o!"@3
                                 "00:00:02:00  942c\n"; // EDM@60

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc, strlen(scc), 0, 1000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 3);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 1, 2, "He\r\n"));
    TASK_CHECK(check_cue(cue_at(vtt, 1), 2, 3, "Hell\r\n"));
    TASK_CHECK(check_cue(cue_at(vtt, 2), 3, 60, "Hello!\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_display_eoc_repeat_padding(void)
{
    // repeated EOC and padding occupy frames but must not split cues
    const char* scc = SCC_HEADER "\n"
                                 "00:00:00:00  9420 c8e9 942f 942f\n" // RCL@0 "Hi"@1 EOC@2 EOC@3 (dup)
                                 "00:00:01:00  8080 8080\n" // padding@30,31
                                 "00:00:02:00  942c 8080 8080\n"; // EDM@60 padding@61,62

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc, strlen(scc), 0, 1000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 2, 60, "Hi\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_display_erase_display(void)
{
    // erase-display closes the visible caption
    const char* scc = SCC_HEADER "\n"
                                 "00:00:00:00  9420 c8e9 942f\n" // EOC@2 -> "Hi"
                                 "00:00:01:00  942c\n" // EDM@30 -> blank
                                 "00:00:02:00  9420 c7ef 942f\n" // EOC@62 -> "Go"
                                 "00:00:04:00  942c\n"; // EDM@120 -> blank

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc, strlen(scc), 0, 1000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 2);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 2, 30, "Hi\r\n"));
    TASK_CHECK(check_cue(cue_at(vtt, 1), 62, 120, "Go\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_display_style_change_splits(void)
{
    // same projected text with a different underline attribute -> two cues
    const char* scc = SCC_HEADER "\n"
                                 "00:00:00:00  9420 94e0 c8e9 942f\n" // EOC@3 -> "Hi" (white)
                                 "00:00:02:00  9420 9461 c8e9 942f\n" // EOC@63 -> "Hi" (underlined)
                                 "00:00:04:00  942c\n"; // EDM@120

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc, strlen(scc), 0, 1000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 2);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 3, 63, "Hi\r\n"));
    TASK_CHECK(check_cue(cue_at(vtt, 1), 63, 120, "Hi\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_display_line_endings(void)
{
    // CRLF line endings, tab separators, final line without a newline
    const char* scc = "Scenarist_SCC V1.0\r\n\r\n"
                      "00:00:00:00\t9420 c8e9 942f\r\n"
                      "00:00:02:00\t942c";

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc, strlen(scc), 0, 1000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 2, 60, "Hi\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_display_unterminated_input(void)
{
    // the input is not NUL terminated; nothing may be read past size
    const char* scc = SCC_HEADER "\n"
                                 "00:00:00:00  9420 c8e9 942f\n"
                                 "00:00:02:00  942c"; // no trailing newline
    size_t size = strlen(scc);
    utf8_char_t* copy = (utf8_char_t*)malloc(size); // exactly size bytes, no NUL

    TASK_CHECK(copy != NULL);
    memcpy(copy, scc, size);

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(copy, size, 0, 1000, &error, &error_line);
    free(copy);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 2, 60, "Hi\r\n"));
    vtt_free(vtt);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// window
////////////////////////////////////////////////////////////////////////////////

static int case_window_clip_absolute(void)
{
    const char* scc = SCC_HEADER "\n"
                                 "00:00:00:00  9420 c8e9 942f\n" // EOC@2 -> "Hi"
                                 "00:00:10:00  942c\n"; // EDM@300

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc, strlen(scc), 15, 45, &error, &error_line);

    // clipped to the window, but times stay absolute program time
    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 15, 45, "Hi\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_window_end_closes(void)
{
    // the erase falls outside the window; the last cue is closed at end_frame
    const char* scc = SCC_HEADER "\n"
                                 "00:00:00:00  9420 c8e9 942f\n" // EOC@2 -> "Hi"
                                 "00:00:10:00  942c\n"; // EDM@300

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc, strlen(scc), 0, 100, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 2, 100, "Hi\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_window_no_words(void)
{
    // no control words inside the window, but the caption is still on screen
    const char* scc = SCC_HEADER "\n"
                                 "00:00:00:00  9420 c8e9 942f\n"; // EOC@2 -> "Hi", never erased

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(scc, strlen(scc), 50, 80, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 50, 80, "Hi\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_window_blank_gap(void)
{
    const char* scc = SCC_HEADER "\n"
                                 "00:00:00:00  9420 c8e9 942f\n" // EOC@2 -> "Hi"
                                 "00:00:01:00  942c\n" // EDM@30 -> blank
                                 "00:00:02:00  9420 c7ef 942f\n" // EOC@62 -> "Go"
                                 "00:00:04:00  942c\n"; // EDM@120 -> blank

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;

    // a window over a blank stretch exports nothing, but succeeds
    vtt_t* vtt = scc_timeline_to_vtt(scc, strlen(scc), 31, 59, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 0);
    vtt_free(vtt);

    // a window ending exactly where the caption begins is empty too
    vtt = scc_timeline_to_vtt(scc, strlen(scc), 0, 2, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 0);
    vtt_free(vtt);
    return 0;
}

// extracts the first cue's timestamps and (single line) text from VTT output
static int first_cue(const char* vtt, double* start, double* end, char* text, size_t text_size)
{
    const char* arrow = strstr(vtt, " --> ");

    if (!arrow) {
        return 0;
    }

    const char* line = arrow;

    while (line > vtt && '\n' != line[-1]) {
        --line;
    }

    int h1, m1, s1, ms1, h2, m2, s2, ms2;

    if (8 != sscanf(line, "%2d:%2d:%2d.%3d --> %2d:%2d:%2d.%3d", &h1, &m1, &s1, &ms1, &h2, &m2, &s2, &ms2)) {
        return 0;
    }

    (*start) = h1 * 3600.0 + m1 * 60.0 + s1 + ms1 / 1000.0;
    (*end) = h2 * 3600.0 + m2 * 60.0 + s2 + ms2 / 1000.0;

    const char* body = strchr(arrow, '\n');

    if (!body) {
        return 0;
    }

    size_t size = 0;

    for (const char* p = body + 1; '\r' != *p && '\n' != *p && *p; ++p) {
        if (size + 1 < text_size) {
            text[size++] = *p;
        }
    }

    text[size] = '\0';
    return 1;
}

static size_t count_substr(const char* haystack, const char* needle)
{
    size_t count = 0;
    const char* p = haystack;

    while ((p = strstr(p, needle)) != NULL) {
        ++count, p += 1;
    }

    return count;
}

static int case_window_cli(void)
{
    // exercises the compiled example binary end to end
    const char* scc_path = ".task-build/test_cli.scc";
    const char* vtt_path = ".task-build/test_cli.vtt";
    const char* bad_path = ".task-build/test_cli_bad.scc";

    const char* scc = SCC_HEADER "\n"
                                 "00:00:00:00\t9420 c8e9 942f\n" // EOC@2 -> "Hi"
                                 "00:00:10:00\t942c\n"; // EDM@300 -> blank

    FILE* file = fopen(scc_path, "wb");
    TASK_CHECK(file != NULL);
    TASK_CHECK(fwrite(scc, 1, strlen(scc), file) == strlen(scc));
    fclose(file);

    char command[1024];
    snprintf(command, sizeof(command), ".task-build/scc2vtt %s 0 300 > %s", scc_path, vtt_path);
    TASK_CHECK(system(command) == 0);

    char output[4096];
    file = fopen(vtt_path, "rb");
    TASK_CHECK(file != NULL);
    size_t size = fread(output, 1, sizeof(output) - 1, file);
    fclose(file);
    output[size] = '\0';

    // one cue covering frames [2, 300) -> 0.066733s .. 10.01s
    double start = 0, end = 0;
    char text[256];
    TASK_CHECK(strstr(output, "WEBVTT") == output);
    TASK_CHECK(first_cue(output, &start, &end, text, sizeof(text)));
    TASK_CHECK(fabs(start - frame_seconds(2)) <= 0.001);
    TASK_CHECK(fabs(end - frame_seconds(300)) <= 0.001);
    TASK_CHECK(0 == strcmp(text, "Hi"));
    // there must be exactly one cue in the output
    TASK_CHECK(count_substr(output, " --> ") == 1);

    // a clipping window keeps absolute times: frames [15, 45) -> 0.5005s .. 1.5015s
    snprintf(command, sizeof(command), ".task-build/scc2vtt %s 15 45 > %s", scc_path, vtt_path);
    TASK_CHECK(system(command) == 0);

    file = fopen(vtt_path, "rb");
    TASK_CHECK(file != NULL);
    size = fread(output, 1, sizeof(output) - 1, file);
    fclose(file);
    output[size] = '\0';

    TASK_CHECK(first_cue(output, &start, &end, text, sizeof(text)));
    TASK_CHECK(fabs(start - 0.5005) <= 0.001);
    TASK_CHECK(fabs(end - 1.5015) <= 0.001);
    TASK_CHECK(0 == strcmp(text, "Hi"));
    TASK_CHECK(count_substr(output, " --> ") == 1);

    // a rejected input exits non-zero
    const char* bad = SCC_HEADER "\n"
                                 "00:00:00:00  9421\n"; // 0x21 fails odd parity
    file = fopen(bad_path, "wb");
    TASK_CHECK(file != NULL);
    TASK_CHECK(fwrite(bad, 1, strlen(bad), file) == strlen(bad));
    fclose(file);

    snprintf(command, sizeof(command), ".task-build/scc2vtt %s 0 300 > /dev/null", bad_path);
    TASK_CHECK(system(command) != 0);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// rejection
////////////////////////////////////////////////////////////////////////////////

static int case_reject_header(void)
{
    TASK_CHECK(REJECT("", SCC_TIMELINE_ERROR_HEADER, 1));
    TASK_CHECK(REJECT("Scenarist_SCC V1.1\n\n00:00:00:00  9420\n", SCC_TIMELINE_ERROR_HEADER, 1));
    TASK_CHECK(REJECT("Scenarist_SCC V1.0 \n\n00:00:00:00  9420\n", SCC_TIMELINE_ERROR_HEADER, 1));
    TASK_CHECK(REJECT("\nScenarist_SCC V1.0\n\n00:00:00:00  9420\n", SCC_TIMELINE_ERROR_HEADER, 1));
    TASK_CHECK(REJECT("00:00:00:00  9420\n", SCC_TIMELINE_ERROR_HEADER, 1));
    return 0;
}

static int case_reject_parity(void)
{
    // 9421: low byte 0x21 fails odd parity (it should be 0xa1)
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00:00  9421\n", SCC_TIMELINE_ERROR_PARITY, 3));
    // a failure on the second caption line reports that line's number
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00:00  9420 c8e9 942f\n00:00:01:00  942d\n", SCC_TIMELINE_ERROR_PARITY, 4));
    return 0;
}

static int case_reject_words(void)
{
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00:00  94z0\n", SCC_TIMELINE_ERROR_WORD, 3)); // bad hex digit
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00:00  942\n", SCC_TIMELINE_ERROR_WORD, 3)); // short word
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00:00  94201\n", SCC_TIMELINE_ERROR_WORD, 3)); // long word
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00:00\n", SCC_TIMELINE_ERROR_WORD, 3)); // no words
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00:00  \n", SCC_TIMELINE_ERROR_WORD, 3)); // only separators
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00:00  9420 94\n", SCC_TIMELINE_ERROR_WORD, 3)); // truncated second word
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00:00  9420x942c\n", SCC_TIMELINE_ERROR_WORD, 3)); // missing separator
    return 0;
}

static int case_reject_timecode(void)
{
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00:30  9420\n", SCC_TIMELINE_ERROR_TIMECODE, 3)); // ff 30
    TASK_CHECK(REJECT(SCC_HEADER "\n00:61:00:00  9420\n", SCC_TIMELINE_ERROR_TIMECODE, 3)); // mm 61
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:60:00  9420\n", SCC_TIMELINE_ERROR_TIMECODE, 3)); // ss 60
    TASK_CHECK(REJECT(SCC_HEADER "\n24:00:00:00  9420\n", SCC_TIMELINE_ERROR_TIMECODE, 3)); // hh 24
    TASK_CHECK(REJECT(SCC_HEADER "\n0:00:00:00  9420\n", SCC_TIMELINE_ERROR_TIMECODE, 3)); // short hour
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:0000  9420\n", SCC_TIMELINE_ERROR_TIMECODE, 3)); // missing separator
    return 0;
}

static int case_reject_drop_labels(void)
{
    // ff 00/01 do not exist at second 0 of minutes not divisible by ten
    TASK_CHECK(REJECT(SCC_HEADER "\n00:01:00;00  9420\n", SCC_TIMELINE_ERROR_TIMECODE, 3));
    TASK_CHECK(REJECT(SCC_HEADER "\n00:01:00;01  9420\n", SCC_TIMELINE_ERROR_TIMECODE, 3));
    TASK_CHECK(REJECT(SCC_HEADER "\n00:09:00;01  9420\n", SCC_TIMELINE_ERROR_TIMECODE, 3));
    TASK_CHECK(REJECT(SCC_HEADER "\n00:11:00;00  9420\n", SCC_TIMELINE_ERROR_TIMECODE, 3));

    // ten-minute marks keep every label: 00:10:00;01 -> 18001 - 18 = 17983
    const char* ok = SCC_HEADER "\n"
                                "00:10:00;01  9420 c8e9 942f\n"; // RCL@17983 "Hi"@17984 EOC@17985

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(ok, strlen(ok), 0, 100000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 17985, 100000, "Hi\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_reject_mode_mix(void)
{
    // one file must not mix ':' and ';' timecode modes
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00:00  9420 c8e9 942f\n00:00:01;00  942c\n", SCC_TIMELINE_ERROR_TIMECODE, 4));
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00;00  9420 c8e9 942f\n00:00:01:00  942c\n", SCC_TIMELINE_ERROR_TIMECODE, 4));
    return 0;
}

static int case_reject_overlap(void)
{
    // a line occupies [start, start + words); the next line must not start inside it
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00:00  9420 c8e9 942f\n00:00:00:02  942c\n", SCC_TIMELINE_ERROR_ORDER, 4));
    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:02:00  9420 c8e9 942f\n00:00:01:00  942c\n", SCC_TIMELINE_ERROR_ORDER, 4));

    // abutting is legal
    const char* ok = SCC_HEADER "\n"
                                "00:00:00:00  9420 c8e9 942f\n" // EOC@2 -> "Hi"
                                "00:00:00:03  942c\n"; // EDM@3 -> blank

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(ok, strlen(ok), 0, 100000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 2, 3, "Hi\r\n"));
    vtt_free(vtt);
    return 0;
}

static int case_reject_arguments(void)
{
    const char* scc = SCC_HEADER "\n00:00:00:00  9420 c8e9 942f\n";
    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;

    vtt_t* vtt = scc_timeline_to_vtt(NULL, 10, 0, 100, &error, &error_line);
    TASK_CHECK(vtt == NULL && error == SCC_TIMELINE_ERROR_ARGUMENT && error_line == 0);

    vtt = scc_timeline_to_vtt(scc, strlen(scc), 100, 100, &error, &error_line);
    TASK_CHECK(vtt == NULL && error == SCC_TIMELINE_ERROR_ARGUMENT);

    vtt = scc_timeline_to_vtt(scc, strlen(scc), 200, 100, &error, &error_line);
    TASK_CHECK(vtt == NULL && error == SCC_TIMELINE_ERROR_ARGUMENT);
    return 0;
}

static int case_reject_no_state_leak(void)
{
    // a rejected import must not poison the following ones
    const char* good = SCC_HEADER "\n"
                                  "00:00:00:00  9420 c8e9 942f\n"
                                  "00:00:02:00  942c\n";

    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00:00  9421\n", SCC_TIMELINE_ERROR_PARITY, 3));

    scc_timeline_error_t error = SCC_TIMELINE_OK;
    size_t error_line = 0;
    vtt_t* vtt = scc_timeline_to_vtt(good, strlen(good), 0, 1000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 2, 60, "Hi\r\n"));
    vtt_free(vtt);

    TASK_CHECK(REJECT(SCC_HEADER "\n00:00:00:30  9420\n", SCC_TIMELINE_ERROR_TIMECODE, 3));

    vtt = scc_timeline_to_vtt(good, strlen(good), 0, 1000, &error, &error_line);

    TASK_CHECK(vtt != NULL);
    TASK_CHECK(count_cues(vtt) == 1);
    TASK_CHECK(check_cue(cue_at(vtt, 0), 2, 60, "Hi\r\n"));
    vtt_free(vtt);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
const struct task_case task_cases[] = {
    { "clock", "colon_nondrop", case_clock_colon_nondrop },
    { "clock", "colon_one_hour", case_clock_colon_one_hour },
    { "clock", "drop_one_minute", case_clock_drop_one_minute },
    { "clock", "drop_ten_minute", case_clock_drop_ten_minute },
    { "clock", "header_only", case_clock_header_only },
    { "display", "popon_cross_line_eoc", case_display_popon_cross_line_eoc },
    { "display", "painton_per_word", case_display_painton_per_word },
    { "display", "eoc_repeat_padding", case_display_eoc_repeat_padding },
    { "display", "erase_display", case_display_erase_display },
    { "display", "style_change_splits", case_display_style_change_splits },
    { "display", "line_endings", case_display_line_endings },
    { "display", "unterminated_input", case_display_unterminated_input },
    { "window", "clip_absolute", case_window_clip_absolute },
    { "window", "end_closes", case_window_end_closes },
    { "window", "no_words", case_window_no_words },
    { "window", "blank_gap", case_window_blank_gap },
    { "window", "cli", case_window_cli },
    { "rejection", "header", case_reject_header },
    { "rejection", "parity", case_reject_parity },
    { "rejection", "words", case_reject_words },
    { "rejection", "timecode", case_reject_timecode },
    { "rejection", "drop_labels", case_reject_drop_labels },
    { "rejection", "mode_mix", case_reject_mode_mix },
    { "rejection", "overlap", case_reject_overlap },
    { "rejection", "arguments", case_reject_arguments },
    { "rejection", "no_state_leak", case_reject_no_state_leak },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
