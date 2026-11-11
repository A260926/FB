/*
 * Independent reference model for delay0r temporal history.
 *
 * A deliberately simple time-stamped image list with its own frame copies.
 * It implements the shared host-time rules, the one-second plus one boundary
 * frame retention policy, and latest-not-later-than-target frame selection.
 *
 * It does not include delay0r, does not read any production instance and does
 * not reuse the plugin's frame selection code; tests drive the plugin through
 * its public f0r_* ABI and compare it against this model.
 */
#ifndef DELAY_REF_HPP
#define DELAY_REF_HPP

#include <cstdint>
#include <deque>
#include <vector>

class DelayRef
{
public:
  DelayRef(unsigned int width, unsigned int height);

  void set_delay(double delay_seconds);

  /* Feed one frame using the host-time rules (append / same-time replace /
   * backwards reset / non-finite standalone).  The input pixels are copied;
   * the caller may overwrite its buffer afterwards.  Returns true for a
   * non-finite time, handled as a standalone single frame. */
  bool observe(double time, const uint32_t* in);

  /* Frame the plugin must output for the most recent observe() call. */
  const std::vector<uint32_t>& selected() const;

private:
  struct Entry
  {
    double time;
    std::vector<uint32_t> pixels;
  };

  unsigned int frame_size;
  double delay;
  std::deque<Entry> frames;   // Chronological, unique video times.
  double last_time;
  bool have_last_time;
  std::vector<uint32_t> standalone_frame;
  bool last_was_standalone;

  void clear();
  void prune(double current_time);
};

#endif
