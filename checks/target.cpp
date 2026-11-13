/* Target tests for wide-glyph-aware reflow.
 *
 * Run with: bash checks/check-target.sh
 *
 * These drive the public C API only; they never spawn a terminal or a
 * browser. Input is built from UTF-8 literal bytes and fixed control
 * sequences.
 */

#include "vterm.h"

#include "cases.hpp"

#include <cstring>

namespace {

struct Term {
  VTerm *vt;
  VTermScreen *screen;

  Term(int rows, int cols)
  {
    vt = vterm_new(rows, cols);
    vterm_set_utf8(vt, 1);
    screen = vterm_obtain_screen(vt);
    vterm_screen_reset(screen, 1);
    vterm_screen_enable_reflow(screen, 1);
  }

  ~Term() { vterm_free(vt); }

  Term(const Term &) = delete;
  Term &operator=(const Term &) = delete;

  void write(const char *bytes) { vterm_input_write(vt, bytes, std::strlen(bytes)); }
  void write(const char *bytes, size_t len) { vterm_input_write(vt, bytes, len); }
  void resize(int rows, int cols) { vterm_set_size(vt, rows, cols); }

  VTermScreenCell cell(int row, int col) const
  {
    VTermScreenCell c;
    std::memset(&c, 0, sizeof c);
    if(!vterm_screen_get_cell(screen, VTermPos{row, col}, &c))
      throw std::runtime_error("get_cell out of range");
    return c;
  }

  VTermPos cursor() const
  {
    VTermPos p;
    vterm_state_get_cursorpos(vterm_obtain_state(vt), &p);
    return p;
  }

  bool continuation(int row) const
  {
    return vterm_state_get_lineinfo(vterm_obtain_state(vt), row)->continuation;
  }

  /* Physical-line text export of one row (the public interface). */
  std::string row_text(int row, int cols) const
  {
    VTermRect rect{row, row + 1, 0, cols};
    size_t len = vterm_screen_get_text(screen, nullptr, 0, rect);
    std::string out(len, '\0');
    if(len)
      vterm_screen_get_text(screen, out.data(), len, rect);
    return out;
  }

  std::vector<uint32_t> row_chars(int row, int cols) const
  {
    VTermRect rect{row, row + 1, 0, cols};
    size_t len = vterm_screen_get_chars(screen, nullptr, 0, rect);
    std::vector<uint32_t> out(len);
    if(len)
      vterm_screen_get_chars(screen, out.data(), len, rect);
    return out;
  }
};

const uint32_t MIDDLE_DOT = 0x4e2d;  /* U+4E2D, width 2 */
const uint32_t ACUTE      = 0x0301;  /* U+0301, combining */

const char *const AB_MID_CD = "ab\xe4\xb8\xad" "cd";
const char *const MID_ACUTE = "\xe4\xb8\xad\xcc\x81";

/* A row is "clean" if it neither starts with a detached second half nor ends
 * with a base cell whose second half is missing. */
bool row_is_clean(const Term &t, int row, int cols)
{
  VTermScreenCell first = t.cell(row, 0);
  if(first.chars[0] == (uint32_t)-1)
    return false;

  VTermScreenCell last = t.cell(row, cols - 1);
  /* A width-2 cell in the final column cannot have its second half on this
   * row. That is allowed only if the cell is actually blank padding. */
  if(last.chars[0] != 0 && last.chars[0] != (uint32_t)-1 && last.width == 2)
    return false;
  return true;
}

}  // namespace

CASE(wide_layout)
{
  Term t(8, 6);
  t.write(AB_MID_CD);

  {
    VTermScreenCell c = t.cell(0, 2);
    CHECK(c.chars[0] == MIDDLE_DOT);
    CHECK(c.width == 2);
  }

  t.resize(8, 3);

  /* "ab" + one display blank, then the wide char with "c", then "d". */
  CHECK(t.cell(0, 0).chars[0] == 'a');
  CHECK(t.cell(0, 1).chars[0] == 'b');
  CHECK(t.cell(0, 2).chars[0] == 0);
  CHECK(t.cell(0, 2).width == 1);

  VTermScreenCell mid = t.cell(1, 0);
  CHECK(mid.chars[0] == MIDDLE_DOT);
  CHECK(mid.width == 2);
  CHECK(t.cell(1, 1).chars[0] == (uint32_t)-1);
  CHECK(t.cell(1, 2).chars[0] == 'c');

  CHECK(t.cell(2, 0).chars[0] == 'd');
  CHECK(t.cell(2, 1).chars[0] == 0);

  /* No orphaned half cells anywhere. */
  for(int r = 0; r < 8; r++)
    CHECK(row_is_clean(t, r, 3));

  /* The physical-line interface reports the new physical breaks. */
  CHECK(t.continuation(1));
  CHECK(t.continuation(2));
  CHECK(!t.continuation(0));

  /* get_text / get_chars keep the existing physical-line interface: the
   * second half of a wide glyph is skipped and trailing erased cells are not
   * emitted, so the padding blank at the end of row 0 is not text. */
  CHECK(t.row_chars(0, 3) == (std::vector<uint32_t>{'a', 'b'}));
  CHECK(t.row_chars(1, 3) == (std::vector<uint32_t>{MIDDLE_DOT, 'c'}));
  CHECK(t.row_chars(2, 3) == (std::vector<uint32_t>{'d'}));
  CHECK(t.row_text(1, 3) == "\xe4\xb8\xad" "c");

  /* Combining mark stays in the wide glyph's leading cell. */
  Term t2(6, 6);
  t2.write("ab");
  t2.write(MID_ACUTE);
  t2.write("cd");
  t2.resize(6, 3);

  VTermScreenCell combined = t2.cell(1, 0);
  CHECK(combined.chars[0] == MIDDLE_DOT);
  CHECK(combined.chars[1] == ACUTE);
  CHECK(combined.chars[2] == 0);
  CHECK(combined.width == 2);
  CHECK(t2.cell(1, 1).chars[0] == (uint32_t)-1);
  for(int r = 0; r < 6; r++)
    CHECK(row_is_clean(t2, r, 3));
}

CASE(repeated_widths)
{
  Term t(8, 6);
  t.write("\x1b[1;31m");     /* red foreground */
  t.write("a");
  t.write("\x1b[0m");
  t.write(" \xe4\xb8\xad c", 6);  /* real space, wide char, real space, 'c' */

  /* Snapshot of the logical content: chars, widths, bold/reverse flags,
   * colours. */
  struct Snap {
    uint32_t chars[VTERM_MAX_CHARS_PER_CELL];
    int width;
    VTermColor fg, bg;
  };
  auto snapshot = [&](int cols) {
    std::vector<Snap> out;
    for(int r = 0; r < 8; r++)
      for(int c = 0; c < cols; c++) {
        VTermScreenCell cell = t.cell(r, c);
        Snap s;
        std::memcpy(s.chars, cell.chars, sizeof cell.chars);
        s.width = cell.width;
        s.fg = cell.fg;
        s.bg = cell.bg;
        out.push_back(s);
      }
    return out;
  };
  auto same = [](const std::vector<Snap>& a, const std::vector<Snap>& b) {
    if(a.size() != b.size())
      return false;
    for(size_t i = 0; i < a.size(); i++) {
      if(a[i].width != b[i].width)
        return false;
      for(int k = 0; k < VTERM_MAX_CHARS_PER_CELL; k++) {
        if(a[i].chars[k] != b[i].chars[k])
          return false;
        if(!a[i].chars[k])
          break;
      }
      if(!vterm_color_is_equal(&a[i].fg, &b[i].fg))
        return false;
      if(!vterm_color_is_equal(&a[i].bg, &b[i].bg))
        return false;
    }
    return true;
  };

  std::vector<Snap> original = snapshot(6);

  /* Cycle 6 -> 3 -> 5 -> 6 several times. Nothing may accumulate. */
  for(int i = 0; i < 3; i++) {
    t.resize(8, 3);
    t.resize(8, 5);
    t.resize(8, 6);
    CHECK(same(snapshot(6), original));
  }

  /* Real spaces survive; the wide glyph stays whole. */
  CHECK(t.cell(0, 0).chars[0] == 'a');
  CHECK(t.cell(0, 1).chars[0] == ' ');
  CHECK(t.cell(0, 2).chars[0] == MIDDLE_DOT);
  CHECK(t.cell(0, 2).width == 2);
  CHECK(t.cell(0, 4).chars[0] == ' ');
  CHECK(t.cell(0, 5).chars[0] == 'c');

  /* A real trailing space is not padding: shrinking and growing keeps it. */
  Term t3(4, 4);
  t3.write("ab  ");
  t3.resize(4, 3);
  t3.resize(4, 4);
  CHECK(t3.cell(0, 0).chars[0] == 'a');
  CHECK(t3.cell(0, 1).chars[0] == 'b');
  CHECK(t3.cell(0, 2).chars[0] == ' ');
  CHECK(t3.cell(0, 3).chars[0] == ' ');
}

CASE(cursor_followup)
{
  /* Every case here is also cross-checked against "same logical content laid
   * out directly at the target width, then keep typing" at the end of the
   * case. */

  /* Cursor just past a wide glyph (the row is exactly full at width 4). */
  Term t(4, 4);
  t.write("ab");
  t.write("\xe4\xb8\xad", 3);
  CHECK(t.cursor().row == 0);
  CHECK(t.cursor().col == 2);

  t.resize(4, 3);
  /* Direct layout at width 3 puts "ab", then wraps the wide glyph to the
   * next row; the cursor follows it to (1,2). */
  CHECK(t.cursor().row == 1);
  CHECK(t.cursor().col == 2);

  t.write("X");
  CHECK(t.cell(1, 2).chars[0] == 'X');
  CHECK(t.cursor().row == 1);
  CHECK(t.cursor().col == 2);  /* phantom at the end of that row */

  /* The same follows a wide glyph carrying a combining mark. */
  Term tc(4, 4);
  tc.write("ab");
  tc.write("\xe4\xb8\xad\xcc\x81", 5);
  CHECK(tc.cursor().col == 2);
  tc.resize(4, 3);
  CHECK(tc.cursor().row == 1);
  CHECK(tc.cursor().col == 2);
  CHECK(tc.cell(1, 0).chars[0] == MIDDLE_DOT);
  CHECK(tc.cell(1, 0).chars[1] == ACUTE);
  CHECK(tc.cell(1, 0).width == 2);
  CHECK(tc.cell(1, 1).chars[0] == (uint32_t)-1);

  /* Cursor before a wide glyph. */
  Term tb(4, 6);
  tb.write("abcd");
  tb.write("\xe4\xb8\xad", 3);
  tb.write("\x1b[1;4H");  /* on 'd', immediately before the wide char */
  CHECK(tb.cursor().row == 0);
  CHECK(tb.cursor().col == 3);
  tb.resize(4, 3);
  /* "abcd" splits as "abc"/"d", and the wide glyph cannot start in row 0's
   * last column, so it begins row 1. The cursor was on 'd', which is now the
   * base cell at (1,0). */
  CHECK(tb.cursor().row == 1);
  CHECK(tb.cursor().col == 0);
  tb.write("Z");
  CHECK(tb.cell(1, 0).chars[0] == 'Z');

  /* Row exactly filled but autowrap has not fired (phantom). */
  Term t3(4, 4);
  t3.write("abcd");
  CHECK(t3.cursor().row == 0);
  CHECK(t3.cursor().col == 3);  /* still on the last written cell */
  t3.resize(4, 3);
  CHECK(t3.cursor().row == 1);
  CHECK(t3.cursor().col == 1);  /* matches a direct layout at width 3 */
  t3.write("E");
  CHECK(t3.cell(1, 1).chars[0] == 'E');
  CHECK(t3.cursor().row == 1);
  CHECK(t3.cursor().col == 2);

  /* Wide glyph exactly filling a row: the next print wraps. */
  Term t4(4, 4);
  t4.write("ab");
  t4.write("\xe4\xb8\xad", 3);
  t4.resize(4, 2);
  CHECK(t4.cursor().row == 1);
  CHECK(t4.cursor().col == 0);
  t4.write("F");
  CHECK(t4.cell(2, 0).chars[0] == 'F');
  CHECK(t4.cursor().row == 2);
  CHECK(t4.cursor().col == 1);

  /* Resize to a single column: wide layout rules do not apply, but the
   * cursor must still end up in range. */
  Term t5(4, 8);
  t5.write("abcdefgh");
  t5.resize(4, 1);
  CHECK(t5.cursor().col == 0);
  CHECK(t5.cursor().row >= 0 && t5.cursor().row < 4);

  /* After reflow, writing continues exactly like a direct layout. */
  struct Case {
    const char *prefix;
    int dst_cols;
    const char *tail;
  } cases[] = {
    {"ab\xe4\xb8\xad", 3, "G"},
    {"ab\xe4\xb8\xad", 2, "G"},
    {"abcd\xe4\xb8\xad", 3, "GH"},
    {"abcde\xe4\xb8\xad\xcc\x81", 4, "GH"},
    {"abcdefgh", 3, "IJK"},
  };
  for(const Case &cs : cases) {
    Term reflowed(4, 6);
    reflowed.write(cs.prefix, std::strlen(cs.prefix));
    reflowed.resize(4, cs.dst_cols);
    reflowed.write(cs.tail, std::strlen(cs.tail));

    Term direct(4, cs.dst_cols);
    direct.write(cs.prefix, std::strlen(cs.prefix));
    direct.write(cs.tail, std::strlen(cs.tail));

    CHECK(reflowed.cursor().row == direct.cursor().row);
    CHECK(reflowed.cursor().col == direct.cursor().col);
    for(int r = 0; r < 4; r++)
      for(int c = 0; c < cs.dst_cols; c++) {
        VTermScreenCell a = reflowed.cell(r, c);
        VTermScreenCell b = direct.cell(r, c);
        CHECK(a.chars[0] == b.chars[0]);
        CHECK(a.width == b.width);
      }
  }
}

CASE(buffer_edits)
{
  /* Erasing a row must not let stale reflow records resurrect it. */
  Term t(6, 6);
  t.write("abcdef");
  t.resize(6, 3);            /* abc / def */
  CHECK(t.cell(0, 0).chars[0] == 'a');
  CHECK(t.cell(1, 0).chars[0] == 'd');

  t.write("\x1b[2;1H\x1b[K", 9);  /* erase whole of row 1 */
  t.resize(6, 6);
  CHECK(t.cell(0, 0).chars[0] == 'a');
  CHECK(t.cell(0, 1).chars[0] == 'b');
  CHECK(t.cell(0, 2).chars[0] == 'c');
  /* 'def' was erased and must not come back. */
  CHECK(t.cell(0, 3).chars[0] == 0);
  CHECK(t.cell(1, 0).chars[0] == 0);

  /* Partial erase at the tail of row 0. The erase cancels row 1's continuation
   * marker, so "def" becomes its own logical line and stays where it is; only
   * what was actually erased (row 0's tail) disappears. */
  Term t2(6, 6);
  t2.write("abcdef");
  t2.resize(6, 3);
  CHECK(t2.continuation(1));
  t2.write("\x1b[1;2H\x1b[K", 9);  /* erase from row 0 col 1 onwards */
  CHECK(!t2.continuation(1));
  t2.resize(6, 6);
  CHECK(t2.cell(0, 0).chars[0] == 'a');
  CHECK(t2.cell(0, 1).chars[0] == 0);
  CHECK(t2.cell(0, 2).chars[0] == 0);
  CHECK(t2.cell(1, 0).chars[0] == 'd');
  CHECK(t2.cell(1, 1).chars[0] == 'e');
  CHECK(t2.cell(1, 2).chars[0] == 'f');
  CHECK(!t2.continuation(1));

  /* Insert / delete rows then resize. */
  Term t3(6, 6);
  t3.write("one\r\ntwo\r\nthree", 13);
  t3.write("\x1b[2;1H\x1b[L", 9);  /* insert line at row 1 */
  t3.resize(6, 4);
  CHECK(t3.cell(0, 0).chars[0] == 'o');
  CHECK(t3.cell(0, 1).chars[0] == 'n');
  CHECK(t3.cell(0, 2).chars[0] == 'e');
  CHECK(t3.cell(1, 0).chars[0] == 0);   /* inserted blank row */
  CHECK(t3.cell(2, 0).chars[0] == 't');
  CHECK(t3.cell(2, 1).chars[0] == 'w');
  CHECK(t3.cell(2, 2).chars[0] == 'o');

  Term t4(6, 6);
  t4.write("one\r\ntwo", 9);
  t4.write("\x1b[1;1H\x1b[M", 9);  /* delete line at row 0 */
  t4.resize(6, 4);
  CHECK(t4.cell(0, 0).chars[0] == 't');
  CHECK(t4.cell(0, 1).chars[0] == 'w');
  CHECK(t4.cell(0, 2).chars[0] == 'o');

  /* Reset then widen: nothing resurrects. */
  Term t5(6, 6);
  t5.write("ab\xe4\xb8\xad" "cd", 7);
  t5.resize(6, 3);
  CHECK(t5.cell(1, 0).chars[0] == MIDDLE_DOT);
  vterm_screen_reset(t5.screen, 1);
  t5.resize(6, 6);
  for(int r = 0; r < 6; r++)
    for(int c = 0; c < 6; c++)
      CHECK(t5.cell(r, c).chars[0] == 0);

  /* Overwriting a cell then resizing must show the overwritten text. */
  Term t6(4, 6);
  t6.write("abcdef");
  t6.write("\x1b[1;3HXY", 8);
  t6.resize(4, 4);
  CHECK(t6.cell(0, 0).chars[0] == 'a');
  CHECK(t6.cell(0, 1).chars[0] == 'b');
  CHECK(t6.cell(0, 2).chars[0] == 'X');
  CHECK(t6.cell(0, 3).chars[0] == 'Y');

  /* Explicit CRLF stays a hard break; reflow must not join records. */
  Term t7(8, 6);
  t7.write("rec1\r\nrec2");
  t7.resize(8, 4);
  CHECK(t7.cell(0, 0).chars[0] == 'r');
  CHECK(t7.cell(0, 3).chars[0] == '1');
  CHECK(t7.cell(1, 0).chars[0] == 'r');
  CHECK(t7.cell(1, 3).chars[0] == '2');
  CHECK(!t7.continuation(0));
  CHECK(!t7.continuation(1));
  t7.resize(8, 3);
  CHECK(t7.cell(0, 0).chars[0] == 'r');
  CHECK(t7.cell(1, 0).chars[0] == '1');
  CHECK(t7.continuation(1));
  CHECK(t7.cell(2, 0).chars[0] == 'r');  /* second record starts afresh */
  CHECK(t7.cell(2, 1).chars[0] == 'e');
  CHECK(!t7.continuation(2));

  /* Main and alternate screens keep their reflow information separately.
   * Resizing while the alternate screen is active must not disturb the
   * primary layout, and the cursor saved by mode 1049 must be restored at
   * its logical position in the primary buffer. */
  Term t8(6, 8);
  vterm_screen_enable_altscreen(t8.screen, 1);
  t8.write("PRIMARY", 7);
  t8.resize(6, 4);
  CHECK(t8.cell(0, 0).chars[0] == 'P');
  CHECK(t8.cell(1, 0).chars[0] == 'A');
  CHECK(t8.cursor().row == 1);
  CHECK(t8.cursor().col == 3);

  t8.write("\x1b[?1049h", 8);
  t8.write("ALT", 3);
  t8.resize(6, 2);
  CHECK(t8.cell(2, 1).chars[0] == 'A');
  CHECK(t8.cell(3, 0).chars[0] == 'L');
  CHECK(t8.cell(3, 1).chars[0] == 'T');
  CHECK(t8.cell(0, 0).chars[0] == 0);   /* primary not visible */

  t8.write("\x1b[?1049l", 8);
  /* The primary content comes back exactly as it was laid out at width 2,
   * with the saved cursor at its logical position. At width 2 it reads
   * "PR"/"IM"/"AR"/"Y". */
  CHECK(t8.cell(0, 0).chars[0] == 'P');
  CHECK(t8.cell(0, 1).chars[0] == 'R');
  CHECK(t8.cell(1, 0).chars[0] == 'I');
  CHECK(t8.cell(1, 1).chars[0] == 'M');
  CHECK(t8.cell(2, 0).chars[0] == 'A');
  CHECK(t8.cell(2, 1).chars[0] == 'R');
  CHECK(t8.cell(3, 0).chars[0] == 'Y');
  CHECK(t8.cursor().row == 3);
  CHECK(t8.cursor().col == 1);
}
