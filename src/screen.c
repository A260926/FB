#include "vterm_internal.h"

#include <stdio.h>
#include <string.h>

#include "rect.h"
#include "utf8.h"

#define UNICODE_SPACE 0x20
#define UNICODE_LINEFEED 0x0a

#undef DEBUG_REFLOW

/* State of the pen at some moment in time, also used in a cell */
typedef struct
{
  /* After the bitfield */
  VTermColor   fg, bg;

  unsigned int bold      : 1;
  unsigned int underline : 2;
  unsigned int italic    : 1;
  unsigned int blink     : 1;
  unsigned int reverse   : 1;
  unsigned int conceal   : 1;
  unsigned int strike    : 1;
  unsigned int font      : 4; /* 0 to 9 */
  unsigned int small     : 1;
  unsigned int baseline  : 2;

  /* Extra state storage that isn't strictly pen-related */
  unsigned int protected_cell : 1;
  unsigned int dwl            : 1; /* on a DECDWL or DECDHL line */
  unsigned int dhl            : 2; /* on a DECDHL line (1=top 2=bottom) */
  /* Set on a blank cell that reflow inserted only so a double-width glyph
   * could start on the next row. Such a cell is display padding, not part of
   * the logical line content, and must be discarded when the line is re-laid
   * out at another width. A real space (U+0020) never carries this flag. */
  unsigned int reflow_padding : 1;
} ScreenPen;

/* Internal representation of a screen cell */
typedef struct
{
  uint32_t chars[VTERM_MAX_CHARS_PER_CELL];
  ScreenPen pen;
} ScreenCell;

struct VTermScreen
{
  VTerm *vt;
  VTermState *state;

  const VTermScreenCallbacks *callbacks;
  void *cbdata;
  bool callbacks_has_pushline4;

  VTermDamageSize damage_merge;
  /* start_row == -1 => no damage */
  VTermRect damaged;
  VTermRect pending_scrollrect;
  int pending_scroll_downward, pending_scroll_rightward;

  int rows;
  int cols;

  unsigned int global_reverse : 1;
  unsigned int reflow : 1;

  /* Primary and Altscreen. buffers[1] is lazily allocated as needed */
  ScreenCell *buffers[2];

  /* buffer will == buffers[0] or buffers[1], depending on altscreen */
  ScreenCell *buffer;

  /* buffer for a single screen row used in scrollback storage callbacks */
  VTermScreenCell *sb_buffer;

  ScreenPen pen;
};

static inline void clearcell(const VTermScreen *screen, ScreenCell *cell)
{
  cell->chars[0] = 0;
  cell->pen = screen->pen;
}

static inline ScreenCell *getcell(const VTermScreen *screen, int row, int col)
{
  if(row < 0 || row >= screen->rows)
    return NULL;
  if(col < 0 || col >= screen->cols)
    return NULL;
  return screen->buffer + (screen->cols * row) + col;
}

static ScreenCell *alloc_buffer(VTermScreen *screen, int rows, int cols)
{
  ScreenCell *new_buffer = vterm_allocator_malloc(screen->vt, sizeof(ScreenCell) * rows * cols);

  for(int row = 0; row < rows; row++) {
    for(int col = 0; col < cols; col++) {
      clearcell(screen, &new_buffer[row * cols + col]);
    }
  }

  return new_buffer;
}

static void damagerect(VTermScreen *screen, VTermRect rect)
{
  VTermRect emit;

  switch(screen->damage_merge) {
  case VTERM_DAMAGE_CELL:
    /* Always emit damage event */
    emit = rect;
    break;

  case VTERM_DAMAGE_ROW:
    /* Emit damage longer than one row. Try to merge with existing damage in
     * the same row */
    if(rect.end_row > rect.start_row + 1) {
      // Bigger than 1 line - flush existing, emit this
      vterm_screen_flush_damage(screen);
      emit = rect;
    }
    else if(screen->damaged.start_row == -1) {
      // None stored yet
      screen->damaged = rect;
      return;
    }
    else if(rect.start_row == screen->damaged.start_row) {
      // Merge with the stored line
      if(screen->damaged.start_col > rect.start_col)
        screen->damaged.start_col = rect.start_col;
      if(screen->damaged.end_col < rect.end_col)
        screen->damaged.end_col = rect.end_col;
      return;
    }
    else {
      // Emit the currently stored line, store a new one
      emit = screen->damaged;
      screen->damaged = rect;
    }
    break;

  case VTERM_DAMAGE_SCREEN:
  case VTERM_DAMAGE_SCROLL:
    /* Never emit damage event */
    if(screen->damaged.start_row == -1)
      screen->damaged = rect;
    else {
      rect_expand(&screen->damaged, &rect);
    }
    return;

  default:
    DEBUG_LOG("TODO: Maybe merge damage for level %d\n", screen->damage_merge);
    return;
  }

  if(screen->callbacks && screen->callbacks->damage)
    (*screen->callbacks->damage)(emit, screen->cbdata);
}

static void damagescreen(VTermScreen *screen)
{
  VTermRect rect = {
    .start_row = 0,
    .end_row   = screen->rows,
    .start_col = 0,
    .end_col   = screen->cols,
  };

  damagerect(screen, rect);
}

static int putglyph(VTermGlyphInfo *info, VTermPos pos, void *user)
{
  VTermScreen *screen = user;
  ScreenCell *cell = getcell(screen, pos.row, pos.col);

  if(!cell)
    return 0;

  int i;
  for(i = 0; i < VTERM_MAX_CHARS_PER_CELL && info->chars[i]; i++) {
    cell->chars[i] = info->chars[i];
    cell->pen = screen->pen;
  }
  if(i < VTERM_MAX_CHARS_PER_CELL)
    cell->chars[i] = 0;

  for(int col = 1; col < info->width; col++)
    getcell(screen, pos.row, pos.col + col)->chars[0] = (uint32_t)-1;

  VTermRect rect = {
    .start_row = pos.row,
    .end_row   = pos.row+1,
    .start_col = pos.col,
    .end_col   = pos.col+info->width,
  };

  cell->pen.protected_cell = info->protected_cell;
  cell->pen.dwl            = info->dwl;
  cell->pen.dhl            = info->dhl;
  /* This cell now holds real content, not reflow display padding. */
  cell->pen.reflow_padding = 0;

  damagerect(screen, rect);

  return 1;
}

static void sb_pushline_from_row(VTermScreen *screen, int row, bool continuation)
{
  VTermPos pos = { .row = row };
  for(pos.col = 0; pos.col < screen->cols; pos.col++)
    vterm_screen_get_cell(screen, pos, screen->sb_buffer + pos.col);

  if(screen->callbacks_has_pushline4 && screen->callbacks->sb_pushline4)
    (screen->callbacks->sb_pushline4)(screen->cols, screen->sb_buffer, continuation, screen->cbdata);
  else
    (screen->callbacks->sb_pushline)(screen->cols, screen->sb_buffer, screen->cbdata);
}

static int premove(VTermRect rect, void *user)
{
  VTermScreen *screen = user;

  if(((screen->callbacks && screen->callbacks->sb_pushline) ||
        (screen->callbacks_has_pushline4 && screen->callbacks && screen->callbacks->sb_pushline4)) &&
     rect.start_row == 0 && rect.start_col == 0 &&        // starts top-left corner
     rect.end_col == screen->cols &&                      // full width
     screen->buffer == screen->buffers[BUFIDX_PRIMARY]) { // not altscreen
    for(int row = 0; row < rect.end_row; row++) {
      const VTermLineInfo *lineinfo = vterm_state_get_lineinfo(screen->state, row);
      sb_pushline_from_row(screen, row, lineinfo->continuation);
    }
  }

  return 1;
}

static int moverect_internal(VTermRect dest, VTermRect src, void *user)
{
  VTermScreen *screen = user;

  int cols = src.end_col - src.start_col;
  int downward = src.start_row - dest.start_row;

  int init_row, test_row, inc_row;
  if(downward < 0) {
    init_row = dest.end_row - 1;
    test_row = dest.start_row - 1;
    inc_row  = -1;
  }
  else {
    init_row = dest.start_row;
    test_row = dest.end_row;
    inc_row  = +1;
  }

  for(int row = init_row; row != test_row; row += inc_row)
    memmove(getcell(screen, row, dest.start_col),
            getcell(screen, row + downward, src.start_col),
            cols * sizeof(ScreenCell));

  return 1;
}

static int moverect_user(VTermRect dest, VTermRect src, void *user)
{
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->moverect) {
    if(screen->damage_merge != VTERM_DAMAGE_SCROLL)
      // Avoid an infinite loop
      vterm_screen_flush_damage(screen);

    if((*screen->callbacks->moverect)(dest, src, screen->cbdata))
      return 1;
  }

  damagerect(screen, dest);

  return 1;
}

static int erase_internal(VTermRect rect, int selective, void *user)
{
  VTermScreen *screen = user;

  for(int row = rect.start_row; row < screen->state->rows && row < rect.end_row; row++) {
    const VTermLineInfo *info = vterm_state_get_lineinfo(screen->state, row);

    for(int col = rect.start_col; col < rect.end_col; col++) {
      ScreenCell *cell = getcell(screen, row, col);

      if(selective && cell->pen.protected_cell)
        continue;

      cell->chars[0] = 0;
      cell->pen = (ScreenPen){
        /* Only copy .fg and .bg; leave things like rv in reset state */
        .fg = screen->pen.fg,
        .bg = screen->pen.bg,
      };
      cell->pen.dwl = info->doublewidth;
      cell->pen.dhl = info->doubleheight;
    }
  }

  return 1;
}

static int erase_user(VTermRect rect, int selective, void *user)
{
  VTermScreen *screen = user;

  damagerect(screen, rect);

  return 1;
}

static int erase(VTermRect rect, int selective, void *user)
{
  erase_internal(rect, selective, user);
  return erase_user(rect, 0, user);
}

static int scrollrect(VTermRect rect, int downward, int rightward, void *user)
{
  VTermScreen *screen = user;

  if(screen->damage_merge != VTERM_DAMAGE_SCROLL) {
    vterm_scroll_rect(rect, downward, rightward,
        moverect_internal, erase_internal, screen);

    vterm_screen_flush_damage(screen);

    vterm_scroll_rect(rect, downward, rightward,
        moverect_user, erase_user, screen);

    return 1;
  }

  if(screen->damaged.start_row != -1 &&
     !rect_intersects(&rect, &screen->damaged)) {
    vterm_screen_flush_damage(screen);
  }

  if(screen->pending_scrollrect.start_row == -1) {
    screen->pending_scrollrect = rect;
    screen->pending_scroll_downward  = downward;
    screen->pending_scroll_rightward = rightward;
  }
  else if(rect_equal(&screen->pending_scrollrect, &rect) &&
     ((screen->pending_scroll_downward  == 0 && downward  == 0) ||
      (screen->pending_scroll_rightward == 0 && rightward == 0))) {
    screen->pending_scroll_downward  += downward;
    screen->pending_scroll_rightward += rightward;
  }
  else {
    vterm_screen_flush_damage(screen);

    screen->pending_scrollrect = rect;
    screen->pending_scroll_downward  = downward;
    screen->pending_scroll_rightward = rightward;
  }

  vterm_scroll_rect(rect, downward, rightward,
      moverect_internal, erase_internal, screen);

  if(screen->damaged.start_row == -1)
    return 1;

  if(rect_contains(&rect, &screen->damaged)) {
    /* Scroll region entirely contains the damage; just move it */
    vterm_rect_move(&screen->damaged, -downward, -rightward);
    rect_clip(&screen->damaged, &rect);
  }
  /* There are a number of possible cases here, but lets restrict this to only
   * the common case where we might actually gain some performance by
   * optimising it. Namely, a vertical scroll that neatly cuts the damage
   * region in half.
   */
  else if(rect.start_col <= screen->damaged.start_col &&
          rect.end_col   >= screen->damaged.end_col &&
          rightward == 0) {
    if(screen->damaged.start_row >= rect.start_row &&
       screen->damaged.start_row  < rect.end_row) {
      screen->damaged.start_row -= downward;
      if(screen->damaged.start_row < rect.start_row)
        screen->damaged.start_row = rect.start_row;
      if(screen->damaged.start_row > rect.end_row)
        screen->damaged.start_row = rect.end_row;
    }
    if(screen->damaged.end_row >= rect.start_row &&
       screen->damaged.end_row  < rect.end_row) {
      screen->damaged.end_row -= downward;
      if(screen->damaged.end_row < rect.start_row)
        screen->damaged.end_row = rect.start_row;
      if(screen->damaged.end_row > rect.end_row)
        screen->damaged.end_row = rect.end_row;
    }
  }
  else {
    DEBUG_LOG("TODO: Just flush and redo damaged=" STRFrect " rect=" STRFrect "\n",
        ARGSrect(screen->damaged), ARGSrect(rect));
  }

  return 1;
}

static int movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->movecursor)
    return (*screen->callbacks->movecursor)(pos, oldpos, visible, screen->cbdata);

  return 0;
}

static int setpenattr(VTermAttr attr, VTermValue *val, void *user)
{
  VTermScreen *screen = user;

  switch(attr) {
  case VTERM_ATTR_BOLD:
    screen->pen.bold = val->boolean;
    return 1;
  case VTERM_ATTR_UNDERLINE:
    screen->pen.underline = val->number;
    return 1;
  case VTERM_ATTR_ITALIC:
    screen->pen.italic = val->boolean;
    return 1;
  case VTERM_ATTR_BLINK:
    screen->pen.blink = val->boolean;
    return 1;
  case VTERM_ATTR_REVERSE:
    screen->pen.reverse = val->boolean;
    return 1;
  case VTERM_ATTR_CONCEAL:
    screen->pen.conceal = val->boolean;
    return 1;
  case VTERM_ATTR_STRIKE:
    screen->pen.strike = val->boolean;
    return 1;
  case VTERM_ATTR_FONT:
    screen->pen.font = val->number;
    return 1;
  case VTERM_ATTR_FOREGROUND:
    screen->pen.fg = val->color;
    return 1;
  case VTERM_ATTR_BACKGROUND:
    screen->pen.bg = val->color;
    return 1;
  case VTERM_ATTR_SMALL:
    screen->pen.small = val->boolean;
    return 1;
  case VTERM_ATTR_BASELINE:
    screen->pen.baseline = val->number;
    return 1;

  case VTERM_N_ATTRS:
    return 0;
  }

  return 0;
}

static int settermprop(VTermProp prop, VTermValue *val, void *user)
{
  VTermScreen *screen = user;

  switch(prop) {
  case VTERM_PROP_ALTSCREEN:
    if(val->boolean && !screen->buffers[BUFIDX_ALTSCREEN])
      return 0;

    screen->buffer = val->boolean ? screen->buffers[BUFIDX_ALTSCREEN] : screen->buffers[BUFIDX_PRIMARY];
    /* only send a damage event on disable; because during enable there's an
     * erase that sends a damage anyway
     */
    if(!val->boolean)
      damagescreen(screen);
    break;
  case VTERM_PROP_REVERSE:
    screen->global_reverse = val->boolean;
    damagescreen(screen);
    break;
  default:
    ; /* ignore */
  }

  if(screen->callbacks && screen->callbacks->settermprop)
    return (*screen->callbacks->settermprop)(prop, val, screen->cbdata);

  return 1;
}

static int bell(void *user)
{
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->bell)
    return (*screen->callbacks->bell)(screen->cbdata);

  return 0;
}

/* How many cells are non-blank
 * Returns the position of the first blank cell in the trailing blank end */
static int line_popcount(ScreenCell *buffer, int row, int rows, int cols)
{
  int col = cols - 1;
  while(col >= 0 && buffer[row * cols + col].chars[0] == 0)
    col--;
  return col + 1;
}

/* A single glyph of a logical line, as lifted out of the physical screen.
 * The collection below is the only information reflow uses to lay the line
 * out again; nothing outside the cells themselves (such as the width it used
 * to be displayed at) is remembered between resizes. */
typedef struct {
  ScreenCell cell;
  int width;
  int src_row;
  int src_col;
} ReflowItem;

/* True if any physical row of this logical line uses a double-width or
 * double-height line attribute. Reflow does not define any new layout rules
 * for those lines; they keep the previous cell-by-cell behaviour. */
static int line_has_dwl(const VTermLineInfo *lineinfo, int row_start, int row_end)
{
  if(!lineinfo)
    return 1;

  for(int row = row_start; row <= row_end; row++)
    if(lineinfo[row].doublewidth || lineinfo[row].doubleheight)
      return 1;

  return 0;
}

/* Collect the glyphs of a logical line in reading order.
 *
 * The second cell of a double-width glyph (chars[0] == -1) is not an item;
 * the glyph itself carries width 2. Cells that only hold reflow-generated
 * display padding are dropped entirely, so repeatedly laying the same logical
 * line out at different widths neither accumulates blank cells nor turns the
 * padding into real content. Real spaces (U+0020) are ordinary width 1 items
 * with their pen preserved.
 *
 * Width-1 content only wraps when a row is full, so trailing erased cells on
 * any row of the logical line are layout padding, not content. (A real
 * space is U+0020 and non-zero, so it is never dropped this way.)
 *
 * Returns the number of items; *out_width receives their total logical width.
 */
static int collect_line_items(ScreenCell *old_buffer,
                              int old_rows, int old_cols, int row_start, int row_end,
                              ReflowItem *items, int *out_width)
{
  int n = 0;
  int width = 0;

  for(int row = row_start; row <= row_end; row++) {
    int limit = line_popcount(old_buffer, row, old_rows, old_cols);

    for(int col = 0; col < limit; col++) {
      ScreenCell *cell = &old_buffer[row * old_cols + col];

      if(cell->chars[0] == (uint32_t)-1)
        /* Second half of a double-width glyph, already accounted for */
        continue;

      if(cell->chars[0] == 0 && cell->pen.reflow_padding)
        /* Display padding generated by an earlier reflow, not content */
        continue;

      ReflowItem *item = &items[n++];
      item->cell = *cell;
      item->src_row = row;
      item->src_col = col;
      item->width = 1;

      if(col + 1 < old_cols && cell->chars[0] != 0 &&
         old_buffer[row * old_cols + col + 1].chars[0] == (uint32_t)-1)
        item->width = 2;

      width += item->width;
    }
  }

  *out_width = width;
  return n;
}

/* Number of display rows the items need at the given width. */
static int reflow_needed_rows(const ReflowItem *items, int n, int cols)
{
  if(n == 0)
    return 1;

  int row = 0;
  int col = 0;

  for(int i = 0; i < n; i++) {
    int w = items[i].width;

    if(col + w > cols) {
      /* Only one position is left; leave it as display padding and put the
       * whole glyph on the next row. */
      row++;
      col = 0;
    }

    col += w;
    if(col > cols)
      col = cols;
  }

  return row + 1;
}

/* Number of logical width units that precede the given cursor position in
 * this logical line. A phantom cursor sits on the base cell of the last glyph
 * it wrote but logically stands after it. */
static int reflow_cursor_offset(const ReflowItem *items, int n, VTermPos pos, int phantom)
{
  int offset = 0;

  for(int i = 0; i < n; i++) {
    const ReflowItem *item = &items[i];

    if(item->src_row < pos.row) {
      offset += item->width;
      continue;
    }
    if(item->src_row > pos.row)
      break;

    if(item->src_col < pos.col) {
      offset += item->width;
      continue;
    }

    if(item->src_col == pos.col && phantom)
      offset += item->width;

    break;
  }

  return offset;
}

/* Map a logical offset in a line laid out at `cols` to a physical cursor
 * position. A cursor is never placed on the second half of a double-width
 * glyph. If the content ends exactly at the end of a row, the cursor takes the
 * same phantom state a direct layout would leave it in. */
static void reflow_map_offset(const ReflowItem *items, int n, int cols, int row_start,
                              int offset, VTermPos *out, int *out_phantom)
{
  *out_phantom = 0;

  if(n == 0) {
    out->row = row_start;
    out->col = 0;
    return;
  }

  int row = row_start;
  int col = 0;
  int consumed = 0;
  int last_row = row_start;
  int last_col = 0;
  int last_end = 0;

  for(int i = 0; i < n; i++) {
    int w = items[i].width;

    if(col + w > cols) {
      row++;
      col = 0;
    }

    if(offset >= consumed && offset < consumed + w) {
      /* Start of a glyph, or inside its second half; use the base cell */
      out->row = row;
      out->col = col;
      return;
    }

    consumed += w;
    last_row = row;
    last_col = col;
    last_end = col + w;

    col += w;
    if(col > cols)
      col = cols;
  }

  if(last_end >= cols) {
    /* Content precisely fills its final row: the cursor stays on the base
     * cell of the last glyph and the next printable glyph wraps. */
    out->row = last_row;
    out->col = last_col;
    *out_phantom = 1;
  }
  else {
    out->row = last_row;
    out->col = last_end;
  }
}

/* Place the items of a logical line into the freshly cleared rows
 * [row_start,row_end] of the new buffer. */
static void reflow_place_items(ScreenCell *new_buffer, int new_cols,
                               const ReflowItem *items, int n, int row_start)
{
  int row = row_start;
  int col = 0;

  for(int i = 0; i < n; i++) {
    int w = items[i].width;

    if(col + w > new_cols) {
      if(w == 2 && col == new_cols - 1) {
        /* Leave a display blank in the last position. It is not content and
         * carries reflow_padding so a later reflow drops it again. */
        ScreenCell *pad = &new_buffer[row * new_cols + col];
        pad->pen = items[i].cell.pen;
        pad->pen.reflow_padding = 1;
        pad->chars[0] = 0;
      }
      row++;
      col = 0;
    }

    ScreenCell *dst = &new_buffer[row * new_cols + col];
    *dst = items[i].cell;
    dst->pen.reflow_padding = 0;

    if(w == 2 && col + 1 < new_cols) {
      ScreenCell *gap = &new_buffer[row * new_cols + col + 1];
      /* The second half is only a marker: a bare -1 with the glyph's pen, but
       * none of the glyph's own chars (a combining mark belongs to the
       * leading cell). */
      gap->pen = items[i].cell.pen;
      gap->pen.reflow_padding = 0;
      gap->chars[0] = (uint32_t)-1;
      for(int k = 1; k < VTERM_MAX_CHARS_PER_CELL; k++)
        gap->chars[k] = 0;
    }

    col += w;
    if(col > new_cols)
      col = new_cols;
  }
}

static void resize_buffer(VTermScreen *screen, int bufidx, int new_rows, int new_cols, bool active, VTermStateFields *statefields)
{
  int old_rows = screen->rows;
  int old_cols = screen->cols;

  ScreenCell *old_buffer = screen->buffers[bufidx];
  VTermLineInfo *old_lineinfo = statefields->lineinfos[bufidx];

  ScreenCell *new_buffer = vterm_allocator_malloc(screen->vt, sizeof(ScreenCell) * new_rows * new_cols);
  VTermLineInfo *new_lineinfo = vterm_allocator_malloc(screen->vt, sizeof(new_lineinfo[0]) * new_rows);

  int old_row = old_rows - 1;
  int new_row = new_rows - 1;

  VTermPos old_cursor = statefields->pos;
  VTermPos new_cursor = { -1, -1 };

#ifdef DEBUG_REFLOW
  fprintf(stderr, "Resizing from %dx%d to %dx%d; cursor was at (%d,%d)\n",
      old_cols, old_rows, new_cols, new_rows, old_cursor.col, old_cursor.row);
#endif

  /* Keep track of the final row that is knonw to be blank, so we know what
   * spare space we have for scrolling into
   */
  int final_blank_row = new_rows;

  /* Workspace for one logical line's worth of glyphs; the largest logical
   * line the screen can hold is one glyph per cell. */
  ReflowItem *items = NULL;
  if(screen->reflow && old_rows > 0 && old_cols > 0)
    items = vterm_allocator_malloc(screen->vt, sizeof(ReflowItem) * old_rows * old_cols);

  /* A cursor sitting in the phantom column after a glyph that ended a row is
   * logically just past that glyph's last cell. */
  int old_phantom = active ? screen->state->at_phantom : 0;
  int new_phantom = 0;
  int cursor_mapped = 0;

  /* The saved cursor belongs to a specific buffer (DECSC / mode 1048). Reflow
   * it against that buffer's own content when it is the one being resized;
   * never borrow the other buffer's padding information. */
  int saved_active = screen->reflow && screen->state->saved_valid &&
                     (bufidx == screen->state->saved_bufidx);
  VTermPos saved_cursor = screen->state->saved.pos;
  int saved_phantom = screen->state->saved_at_phantom;
  int saved_mapped = 0;

  while(old_row >= 0) {
    int old_row_end = old_row;
    /* TODO: Stop if dwl or dhl */
    while(screen->reflow && old_lineinfo && old_row >= 0 && old_lineinfo[old_row].continuation)
      old_row--;
    int old_row_start = old_row;

    int width = 0;
    int n_items = 0;
    int use_reflow_layout = screen->reflow && items && new_cols >= 2 &&
                            !line_has_dwl(old_lineinfo, old_row_start, old_row_end);

    if(use_reflow_layout)
      n_items = collect_line_items(old_buffer, old_rows, old_cols,
          old_row_start, old_row_end, items, &width);
    else
      for(int row = old_row_start; row <= old_row_end; row++) {
        if(screen->reflow && row < (old_rows - 1) && old_lineinfo[row + 1].continuation)
          width += old_cols;
        else
          width += line_popcount(old_buffer, row, old_rows, old_cols);
      }

    if(final_blank_row == (new_row + 1) && width == 0)
      final_blank_row = new_row;

    int new_height;
    if(use_reflow_layout)
      new_height = reflow_needed_rows(items, n_items, new_cols);
    else
      new_height = screen->reflow
        ? width ? (width + new_cols - 1) / new_cols : 1
        : 1;

    int new_row_end = new_row;
    int new_row_start = new_row - new_height + 1;

    old_row = old_row_start;
    int old_col = 0;

    int spare_rows = new_rows - final_blank_row;

    if(new_row_start < 0 && /* we'd fall off the top */
        spare_rows >= 0 && /* we actually have spare rows */
        (!active || new_cursor.row == -1 || (new_cursor.row - new_row_start) < new_rows))
    {
      /* Attempt to scroll content down into the blank rows at the bottom to
       * make it fit
       */
      int downwards = -new_row_start;
      if(downwards > spare_rows)
        downwards = spare_rows;
      int rowcount = new_rows - downwards;

#ifdef DEBUG_REFLOW
      fprintf(stderr, "  scroll %d rows +%d downwards\n", rowcount, downwards);
#endif

      memmove(&new_buffer[downwards * new_cols], &new_buffer[0],   rowcount * new_cols * sizeof(ScreenCell));
      memmove(&new_lineinfo[downwards],          &new_lineinfo[0], rowcount            * sizeof(new_lineinfo[0]));

      new_row += downwards;
      new_row_start += downwards;
      new_row_end += downwards;

      if(new_cursor.row >= 0)
        new_cursor.row += downwards;
      if(saved_mapped)
        saved_cursor.row += downwards;

      final_blank_row += downwards;
    }

#ifdef DEBUG_REFLOW
    fprintf(stderr, "  rows [%d..%d] <- [%d..%d] width=%d\n",
        new_row_start, new_row_end, old_row_start, old_row_end, width);
#endif

    int cursor_in_group = old_cursor.row >= old_row_start && old_cursor.row <= old_row_end;
    int saved_in_group = saved_active && saved_cursor.row >= old_row_start && saved_cursor.row <= old_row_end;

    if(new_row_start < 0) {
      if(cursor_in_group) {
        new_cursor.row = 0;
        new_cursor.col = old_cursor.col;
        if(new_cursor.col >= new_cols)
          new_cursor.col = new_cols-1;
        if(active && use_reflow_layout) {
          new_phantom = old_phantom && new_cursor.col == new_cols - 1;
          cursor_mapped = 1;
        }
      }
      if(saved_in_group) {
        saved_cursor.row = 0;
        saved_cursor.col = saved_cursor.col < new_cols ? saved_cursor.col : new_cols - 1;
        saved_phantom = saved_phantom && saved_cursor.col == new_cols - 1;
        saved_mapped = 1;
      }
      break;
    }

    if(use_reflow_layout) {
      /* Clear the target rows first: the previous buffer may contain cells
       * from an unrelated line in the space this line now occupies. */
      for(int row = new_row_start; row <= new_row_end; row++) {
        for(int col = 0; col < new_cols; col++)
          clearcell(screen, &new_buffer[row * new_cols + col]);
        new_lineinfo[row] = (VTermLineInfo){ 0 };
      }

      reflow_place_items(new_buffer, new_cols, items, n_items, new_row_start);

      for(int row = new_row_start; row <= new_row_end; row++)
        new_lineinfo[row].continuation = (row > new_row_start);

      if(cursor_in_group) {
        VTermPos mapped;
        int phantom;
        int offset = reflow_cursor_offset(items, n_items, old_cursor, old_phantom);
        reflow_map_offset(items, n_items, new_cols, new_row_start, offset, &mapped, &phantom);
        new_cursor = mapped;
        if(active) {
          new_phantom = phantom;
          cursor_mapped = 1;
        }
      }

      if(saved_in_group) {
        VTermPos mapped;
        int phantom;
        int offset = reflow_cursor_offset(items, n_items, saved_cursor, saved_phantom);
        reflow_map_offset(items, n_items, new_cols, new_row_start, offset, &mapped, &phantom);
        saved_cursor = mapped;
        saved_phantom = phantom;
        saved_mapped = 1;
      }
    }
    else
    for(new_row = new_row_start, old_row = old_row_start; new_row <= new_row_end; new_row++) {
      int count = width >= new_cols ? new_cols : width;
      width -= count;

      int new_col = 0;

      while(count) {
        /* TODO: This could surely be done a lot faster by memcpy()'ing the entire range */
        new_buffer[new_row * new_cols + new_col] = old_buffer[old_row * old_cols + old_col];

        if(old_cursor.row == old_row && old_cursor.col == old_col)
          new_cursor.row = new_row, new_cursor.col = new_col;

        old_col++;
        if(old_col == old_cols) {
          old_row++;

          if(!screen->reflow) {
            new_col++;
            break;
          }
          old_col = 0;
        }

        new_col++;
        count--;
      }

      if(old_cursor.row == old_row && old_cursor.col >= old_col) {
        new_cursor.row = new_row, new_cursor.col = (old_cursor.col - old_col + new_col);
        if(new_cursor.col >= new_cols)
          new_cursor.col = new_cols-1;
      }

      while(new_col < new_cols) {
        clearcell(screen, &new_buffer[new_row * new_cols + new_col]);
        new_col++;
      }

      new_lineinfo[new_row].continuation = (new_row > new_row_start);
    }

    old_row = old_row_start - 1;
    new_row = new_row_start - 1;
  }

  if(old_cursor.row <= old_row) {
    /* cursor would have moved entirely off the top of the screen; lets just
     * bring it within range */
    new_cursor.row = 0, new_cursor.col = old_cursor.col;
    if(new_cursor.col >= new_cols)
      new_cursor.col = new_cols-1;
  }

  /* We really expect the cursor position to be set by now */
  if(active && (new_cursor.row == -1 || new_cursor.col == -1)) {
    fprintf(stderr, "screen_resize failed to update cursor position\n");
    abort();
  }

  if(old_row >= 0 && bufidx == BUFIDX_PRIMARY) {
    /* Push spare lines to scrollback buffer */
    if((screen->callbacks && screen->callbacks->sb_pushline) ||
          (screen->callbacks_has_pushline4 && screen->callbacks && screen->callbacks->sb_pushline4))
      for(int row = 0; row <= old_row; row++) {
        const VTermLineInfo *lineinfo = old_lineinfo + row;
        sb_pushline_from_row(screen, row, lineinfo->continuation);
      }
    if(active)
      statefields->pos.row -= (old_row + 1);
  }
  if(new_row >= 0 && bufidx == BUFIDX_PRIMARY &&
      screen->callbacks && screen->callbacks->sb_popline) {
    /* Try to backfill rows by popping scrollback buffer */
    while(new_row >= 0) {
      if(!(screen->callbacks->sb_popline(old_cols, screen->sb_buffer, screen->cbdata)))
        break;

      VTermPos pos = { .row = new_row };
      for(pos.col = 0; pos.col < old_cols && pos.col < new_cols; pos.col += screen->sb_buffer[pos.col].width) {
        VTermScreenCell *src = &screen->sb_buffer[pos.col];
        ScreenCell *dst = &new_buffer[pos.row * new_cols + pos.col];

        for(int i = 0; i < VTERM_MAX_CHARS_PER_CELL; i++) {
          dst->chars[i] = src->chars[i];
          if(!src->chars[i])
            break;
        }

        dst->pen.bold      = src->attrs.bold;
        dst->pen.underline = src->attrs.underline;
        dst->pen.italic    = src->attrs.italic;
        dst->pen.blink     = src->attrs.blink;
        dst->pen.reverse   = src->attrs.reverse ^ screen->global_reverse;
        dst->pen.conceal   = src->attrs.conceal;
        dst->pen.strike    = src->attrs.strike;
        dst->pen.font      = src->attrs.font;
        dst->pen.small     = src->attrs.small;
        dst->pen.baseline  = src->attrs.baseline;

        dst->pen.fg = src->fg;
        dst->pen.bg = src->bg;

        if(src->width == 2 && pos.col < (new_cols-1))
          (dst + 1)->chars[0] = (uint32_t) -1;
      }
      for( ; pos.col < new_cols; pos.col++)
        clearcell(screen, &new_buffer[pos.row * new_cols + pos.col]);
      new_row--;

      if(active)
        statefields->pos.row++;
    }
  }
  if(new_row >= 0) {
    /* Scroll new rows back up to the top and fill in blanks at the bottom */
    int moverows = new_rows - new_row - 1;
    memmove(&new_buffer[0], &new_buffer[(new_row + 1) * new_cols], moverows * new_cols * sizeof(ScreenCell));
    memmove(&new_lineinfo[0], &new_lineinfo[new_row + 1], moverows * sizeof(new_lineinfo[0]));

    if(active) {
      new_cursor.row -= (new_row + 1);
      if(new_cursor.row < 0) {
        new_cursor.row = 0;
        if(new_cursor.col >= new_cols)
          new_cursor.col = new_cols - 1;
      }
    }

    if(saved_mapped) {
      saved_cursor.row -= (new_row + 1);
      if(saved_cursor.row < 0) {
        saved_cursor.row = 0;
        if(saved_cursor.col >= new_cols)
          saved_cursor.col = new_cols - 1;
        saved_phantom = 0;
      }
    }

    for(new_row = moverows; new_row < new_rows; new_row++) {
      for(int col = 0; col < new_cols; col++)
        clearcell(screen, &new_buffer[new_row * new_cols + col]);
      new_lineinfo[new_row] = (VTermLineInfo){ 0 };
    }
  }

  vterm_allocator_free(screen->vt, old_buffer);
  screen->buffers[bufidx] = new_buffer;

  vterm_allocator_free(screen->vt, old_lineinfo);
  statefields->lineinfos[bufidx] = new_lineinfo;

  if(items)
    vterm_allocator_free(screen->vt, items);

  if(active) {
    statefields->pos = new_cursor;
    /* When we reflowed the cursor, put it (and its phantom state) exactly
     * where a direct layout at the new width would. Otherwise leave the
     * phantom flag to the state layer's existing post-resize adjustment. */
    screen->state->reflow_cursor_mapped = cursor_mapped;
    if(cursor_mapped)
      screen->state->at_phantom = new_phantom;
  }

  if(saved_mapped) {
    screen->state->saved.pos = saved_cursor;
    screen->state->saved_at_phantom = saved_phantom;
  }

  return;
}
static int resize(int new_rows, int new_cols, VTermStateFields *fields, void *user)
{
  VTermScreen *screen = user;

  int altscreen_active = (screen->buffers[BUFIDX_ALTSCREEN] && screen->buffer == screen->buffers[BUFIDX_ALTSCREEN]);

  int old_rows = screen->rows;
  int old_cols = screen->cols;

  if(new_cols > old_cols) {
    /* Ensure that ->sb_buffer is large enough for a new or and old row */
    if(screen->sb_buffer)
      vterm_allocator_free(screen->vt, screen->sb_buffer);

    screen->sb_buffer = vterm_allocator_malloc(screen->vt, sizeof(VTermScreenCell) * new_cols);
  }

  resize_buffer(screen, 0, new_rows, new_cols, !altscreen_active, fields);
  if(screen->buffers[BUFIDX_ALTSCREEN])
    resize_buffer(screen, 1, new_rows, new_cols, altscreen_active, fields);
  else if(new_rows != old_rows) {
    /* We don't need a full resize of the altscreen because it isn't enabled
     * but we should at least keep the lineinfo the right size */
    vterm_allocator_free(screen->vt, fields->lineinfos[BUFIDX_ALTSCREEN]);

    VTermLineInfo *new_lineinfo = vterm_allocator_malloc(screen->vt, sizeof(new_lineinfo[0]) * new_rows);
    for(int row = 0; row < new_rows; row++)
      new_lineinfo[row] = (VTermLineInfo){ 0 };

    fields->lineinfos[BUFIDX_ALTSCREEN] = new_lineinfo;
  }

  screen->buffer = altscreen_active ? screen->buffers[BUFIDX_ALTSCREEN] : screen->buffers[BUFIDX_PRIMARY];

  screen->rows = new_rows;
  screen->cols = new_cols;

  if(new_cols <= old_cols) {
    if(screen->sb_buffer)
      vterm_allocator_free(screen->vt, screen->sb_buffer);

    screen->sb_buffer = vterm_allocator_malloc(screen->vt, sizeof(VTermScreenCell) * new_cols);
  }

  /* TODO: Maaaaybe we can optimise this if there's no reflow happening */
  damagescreen(screen);

  if(screen->callbacks && screen->callbacks->resize)
    return (*screen->callbacks->resize)(new_rows, new_cols, screen->cbdata);

  return 1;
}

static int setlineinfo(int row, const VTermLineInfo *newinfo, const VTermLineInfo *oldinfo, void *user)
{
  VTermScreen *screen = user;

  if(newinfo->doublewidth != oldinfo->doublewidth ||
     newinfo->doubleheight != oldinfo->doubleheight) {
    for(int col = 0; col < screen->cols; col++) {
      ScreenCell *cell = getcell(screen, row, col);
      cell->pen.dwl = newinfo->doublewidth;
      cell->pen.dhl = newinfo->doubleheight;
    }

    VTermRect rect = {
      .start_row = row,
      .end_row   = row + 1,
      .start_col = 0,
      .end_col   = newinfo->doublewidth ? screen->cols / 2 : screen->cols,
    };
    damagerect(screen, rect);

    if(newinfo->doublewidth) {
      rect.start_col = screen->cols / 2;
      rect.end_col   = screen->cols;

      erase_internal(rect, 0, user);
    }
  }

  return 1;
}

static int sb_clear(void *user) {
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->sb_clear)
    if((*screen->callbacks->sb_clear)(screen->cbdata))
      return 1;

  return 0;
}

static VTermStateCallbacks state_cbs = {
  .putglyph    = &putglyph,
  .movecursor  = &movecursor,
  .premove     = &premove,
  .scrollrect  = &scrollrect,
  .erase       = &erase,
  .setpenattr  = &setpenattr,
  .settermprop = &settermprop,
  .bell        = &bell,
  .resize      = &resize,
  .setlineinfo = &setlineinfo,
  .sb_clear    = &sb_clear,
};

static VTermScreen *screen_new(VTerm *vt)
{
  VTermState *state = vterm_obtain_state(vt);
  if(!state)
    return NULL;

  VTermScreen *screen = vterm_allocator_malloc(vt, sizeof(VTermScreen));
  int rows, cols;

  vterm_get_size(vt, &rows, &cols);

  screen->vt = vt;
  screen->state = state;

  screen->damage_merge = VTERM_DAMAGE_CELL;
  screen->damaged.start_row = -1;
  screen->pending_scrollrect.start_row = -1;

  screen->rows = rows;
  screen->cols = cols;

  screen->global_reverse = false;
  screen->reflow = false;

  screen->callbacks = NULL;
  screen->cbdata    = NULL;
  screen->callbacks_has_pushline4 = false;

  screen->buffers[BUFIDX_PRIMARY] = alloc_buffer(screen, rows, cols);

  screen->buffer = screen->buffers[BUFIDX_PRIMARY];

  screen->sb_buffer = vterm_allocator_malloc(screen->vt, sizeof(VTermScreenCell) * cols);

  vterm_state_set_callbacks(screen->state, &state_cbs, screen);
  vterm_state_callbacks_has_premove(screen->state);

  return screen;
}

INTERNAL void vterm_screen_free(VTermScreen *screen)
{
  vterm_allocator_free(screen->vt, screen->buffers[BUFIDX_PRIMARY]);
  if(screen->buffers[BUFIDX_ALTSCREEN])
    vterm_allocator_free(screen->vt, screen->buffers[BUFIDX_ALTSCREEN]);

  vterm_allocator_free(screen->vt, screen->sb_buffer);

  vterm_allocator_free(screen->vt, screen);
}

void vterm_screen_reset(VTermScreen *screen, int hard)
{
  screen->damaged.start_row = -1;
  screen->pending_scrollrect.start_row = -1;
  vterm_state_reset(screen->state, hard);
  vterm_screen_flush_damage(screen);
}

static size_t _get_chars(const VTermScreen *screen, const int utf8, void *buffer, size_t len, const VTermRect rect)
{
  size_t outpos = 0;
  int padding = 0;

#define PUT(c)                                             \
  if(utf8) {                                               \
    size_t thislen = utf8_seqlen(c);                       \
    if(buffer && outpos + thislen <= len)                  \
      outpos += fill_utf8((c), (char *)buffer + outpos);   \
    else                                                   \
      outpos += thislen;                                   \
  }                                                        \
  else {                                                   \
    if(buffer && outpos + 1 <= len)                        \
      ((uint32_t*)buffer)[outpos++] = (c);                 \
    else                                                   \
      outpos++;                                            \
  }

  for(int row = rect.start_row; row < rect.end_row; row++) {
    for(int col = rect.start_col; col < rect.end_col; col++) {
      ScreenCell *cell = getcell(screen, row, col);

      if(cell->chars[0] == 0)
        // Erased cell, might need a space
        padding++;
      else if(cell->chars[0] == (uint32_t)-1)
        // Gap behind a double-width char, do nothing
        ;
      else {
        while(padding) {
          PUT(UNICODE_SPACE);
          padding--;
        }
        for(int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell->chars[i]; i++) {
          PUT(cell->chars[i]);
        }
      }
    }

    if(row < rect.end_row - 1) {
      PUT(UNICODE_LINEFEED);
      padding = 0;
    }
  }

  return outpos;
}

size_t vterm_screen_get_chars(const VTermScreen *screen, uint32_t *chars, size_t len, const VTermRect rect)
{
  return _get_chars(screen, 0, chars, len, rect);
}

size_t vterm_screen_get_text(const VTermScreen *screen, char *str, size_t len, const VTermRect rect)
{
  return _get_chars(screen, 1, str, len, rect);
}

/* Copy internal to external representation of a screen cell */
int vterm_screen_get_cell(const VTermScreen *screen, VTermPos pos, VTermScreenCell *cell)
{
  ScreenCell *intcell = getcell(screen, pos.row, pos.col);
  if(!intcell)
    return 0;

  for(int i = 0; i < VTERM_MAX_CHARS_PER_CELL; i++) {
    cell->chars[i] = intcell->chars[i];
    if(!intcell->chars[i])
      break;
  }

  cell->attrs.bold      = intcell->pen.bold;
  cell->attrs.underline = intcell->pen.underline;
  cell->attrs.italic    = intcell->pen.italic;
  cell->attrs.blink     = intcell->pen.blink;
  cell->attrs.reverse   = intcell->pen.reverse ^ screen->global_reverse;
  cell->attrs.conceal   = intcell->pen.conceal;
  cell->attrs.strike    = intcell->pen.strike;
  cell->attrs.font      = intcell->pen.font;
  cell->attrs.small     = intcell->pen.small;
  cell->attrs.baseline  = intcell->pen.baseline;

  cell->attrs.dwl = intcell->pen.dwl;
  cell->attrs.dhl = intcell->pen.dhl;

  cell->fg = intcell->pen.fg;
  cell->bg = intcell->pen.bg;

  if(pos.col < (screen->cols - 1) &&
     getcell(screen, pos.row, pos.col + 1)->chars[0] == (uint32_t)-1)
    cell->width = 2;
  else
    cell->width = 1;

  return 1;
}

int vterm_screen_is_eol(const VTermScreen *screen, VTermPos pos)
{
  /* This cell is EOL if this and every cell to the right is black */
  for(; pos.col < screen->cols; pos.col++) {
    ScreenCell *cell = getcell(screen, pos.row, pos.col);
    if(cell->chars[0] != 0)
      return 0;
  }

  return 1;
}

VTermScreen *vterm_obtain_screen(VTerm *vt)
{
  if(vt->screen)
    return vt->screen;

  VTermScreen *screen = screen_new(vt);
  vt->screen = screen;

  return screen;
}

void vterm_screen_enable_reflow(VTermScreen *screen, bool reflow)
{
  screen->reflow = reflow;
}

#undef vterm_screen_set_reflow
void vterm_screen_set_reflow(VTermScreen *screen, bool reflow)
{
  vterm_screen_enable_reflow(screen, reflow);
}

void vterm_screen_enable_altscreen(VTermScreen *screen, int altscreen)
{
  if(!screen->buffers[BUFIDX_ALTSCREEN] && altscreen) {
    int rows, cols;
    vterm_get_size(screen->vt, &rows, &cols);

    screen->buffers[BUFIDX_ALTSCREEN] = alloc_buffer(screen, rows, cols);
  }
}

void vterm_screen_set_callbacks(VTermScreen *screen, const VTermScreenCallbacks *callbacks, void *user)
{
  screen->callbacks = callbacks;
  screen->cbdata = user;
}

void *vterm_screen_get_cbdata(VTermScreen *screen)
{
  return screen->cbdata;
}

void vterm_screen_callbacks_has_pushline4(VTermScreen *screen)
{
  screen->callbacks_has_pushline4 = true;
}

void vterm_screen_set_unrecognised_fallbacks(VTermScreen *screen, const VTermStateFallbacks *fallbacks, void *user)
{
  vterm_state_set_unrecognised_fallbacks(screen->state, fallbacks, user);
}

void *vterm_screen_get_unrecognised_fbdata(VTermScreen *screen)
{
  return vterm_state_get_unrecognised_fbdata(screen->state);
}

void vterm_screen_flush_damage(VTermScreen *screen)
{
  if(screen->pending_scrollrect.start_row != -1) {
    vterm_scroll_rect(screen->pending_scrollrect, screen->pending_scroll_downward, screen->pending_scroll_rightward,
        moverect_user, erase_user, screen);

    screen->pending_scrollrect.start_row = -1;
  }

  if(screen->damaged.start_row != -1) {
    if(screen->callbacks && screen->callbacks->damage)
      (*screen->callbacks->damage)(screen->damaged, screen->cbdata);

    screen->damaged.start_row = -1;
  }
}

void vterm_screen_set_damage_merge(VTermScreen *screen, VTermDamageSize size)
{
  vterm_screen_flush_damage(screen);
  screen->damage_merge = size;
}

static int attrs_differ(VTermAttrMask attrs, ScreenCell *a, ScreenCell *b)
{
  if((attrs & VTERM_ATTR_BOLD_MASK)       && (a->pen.bold != b->pen.bold))
    return 1;
  if((attrs & VTERM_ATTR_UNDERLINE_MASK)  && (a->pen.underline != b->pen.underline))
    return 1;
  if((attrs & VTERM_ATTR_ITALIC_MASK)     && (a->pen.italic != b->pen.italic))
    return 1;
  if((attrs & VTERM_ATTR_BLINK_MASK)      && (a->pen.blink != b->pen.blink))
    return 1;
  if((attrs & VTERM_ATTR_REVERSE_MASK)    && (a->pen.reverse != b->pen.reverse))
    return 1;
  if((attrs & VTERM_ATTR_CONCEAL_MASK)    && (a->pen.conceal != b->pen.conceal))
    return 1;
  if((attrs & VTERM_ATTR_STRIKE_MASK)     && (a->pen.strike != b->pen.strike))
    return 1;
  if((attrs & VTERM_ATTR_FONT_MASK)       && (a->pen.font != b->pen.font))
    return 1;
  if((attrs & VTERM_ATTR_FOREGROUND_MASK) && !vterm_color_is_equal(&a->pen.fg, &b->pen.fg))
    return 1;
  if((attrs & VTERM_ATTR_BACKGROUND_MASK) && !vterm_color_is_equal(&a->pen.bg, &b->pen.bg))
    return 1;
  if((attrs & VTERM_ATTR_SMALL_MASK)    && (a->pen.small != b->pen.small))
    return 1;
  if((attrs & VTERM_ATTR_BASELINE_MASK)    && (a->pen.baseline != b->pen.baseline))
    return 1;

  return 0;
}

int vterm_screen_get_attrs_extent(const VTermScreen *screen, VTermRect *extent, VTermPos pos, VTermAttrMask attrs)
{
  ScreenCell *target = getcell(screen, pos.row, pos.col);

  // TODO: bounds check
  extent->start_row = pos.row;
  extent->end_row   = pos.row + 1;

  if(extent->start_col < 0)
    extent->start_col = 0;
  if(extent->end_col < 0)
    extent->end_col = screen->cols;

  int col;

  for(col = pos.col - 1; col >= extent->start_col; col--)
    if(attrs_differ(attrs, target, getcell(screen, pos.row, col)))
      break;
  extent->start_col = col + 1;

  for(col = pos.col + 1; col < extent->end_col; col++)
    if(attrs_differ(attrs, target, getcell(screen, pos.row, col)))
      break;
  extent->end_col = col - 1;

  return 1;
}

void vterm_screen_convert_color_to_rgb(const VTermScreen *screen, VTermColor *col)
{
  vterm_state_convert_color_to_rgb(screen->state, col);
}

static void reset_default_colours(VTermScreen *screen, ScreenCell *buffer)
{
  for(int row = 0; row <= screen->rows - 1; row++)
    for(int col = 0; col <= screen->cols - 1; col++) {
      ScreenCell *cell = &buffer[row * screen->cols + col];
      if(VTERM_COLOR_IS_DEFAULT_FG(&cell->pen.fg))
        cell->pen.fg = screen->pen.fg;
      if(VTERM_COLOR_IS_DEFAULT_BG(&cell->pen.bg))
        cell->pen.bg = screen->pen.bg;
    }
}

void vterm_screen_set_default_colors(VTermScreen *screen, const VTermColor *default_fg, const VTermColor *default_bg)
{
  vterm_state_set_default_colors(screen->state, default_fg, default_bg);

  if(default_fg && VTERM_COLOR_IS_DEFAULT_FG(&screen->pen.fg)) {
    screen->pen.fg = *default_fg;
    screen->pen.fg.type = (screen->pen.fg.type & ~VTERM_COLOR_DEFAULT_MASK)
                        | VTERM_COLOR_DEFAULT_FG;
  }

  if(default_bg && VTERM_COLOR_IS_DEFAULT_BG(&screen->pen.bg)) {
    screen->pen.bg = *default_bg;
    screen->pen.bg.type = (screen->pen.bg.type & ~VTERM_COLOR_DEFAULT_MASK)
                        | VTERM_COLOR_DEFAULT_BG;
  }

  reset_default_colours(screen, screen->buffers[0]);
  if(screen->buffers[1])
    reset_default_colours(screen, screen->buffers[1]);
}
