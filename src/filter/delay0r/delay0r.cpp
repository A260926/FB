#include "frei0r.hpp"

#include <algorithm>
#include <deque>
#include <cmath>

/*
 * delay0r -- video delay
 *
 * DelayTime is interpreted in the range 0..1 second.  For every update the
 * target time is (time - DelayTime) and the output is the newest retained
 * frame whose timestamp is not later than the target.  If no frame that old
 * exists yet, the oldest retained frame is used; with zero delay the current
 * frame is used.  No frames are synthesized for uneven time intervals.
 *
 * Host time handling (shared temporal contract):
 *   - a time strictly greater than the previous one appends a new frame;
 *   - the exact same time replaces the current frame, so a redraw is stored
 *     once and does not occupy an extra history entry;
 *   - a time smaller than the previous one starts a new playback segment and
 *     discards every previously retained frame;
 *   - a non-finite time (NaN/infinity) is handled as a standalone frame and
 *     clears the history; the next finite time starts afresh.
 *
 * Because the host may animate DelayTime upwards at any moment, every actual
 * frame of the most recent second is retained, plus the single frame just
 * before that second as a boundary support frame.  Frames older than this
 * range are reclaimed promptly.  Every retained frame is a private copy owned
 * by the plugin: the host is free to overwrite the input buffer afterwards.
 */

class delay0r : public frei0r::filter
{
public:
  delay0r(unsigned int width, unsigned int height)
    : frame_size(width * height), have_last_time(false)
  {
    delay = 0.0;
    register_param(delay,"DelayTime","the delay time");
  }

  ~delay0r()
  {
    while (!buffer.empty())
    {
      delete[] buffer.front().pixels;
      buffer.pop_front();
    }
  }

  virtual void update(double time,
                      uint32_t* out,
                      const uint32_t* in)
  {
    const bool standalone = !std::isfinite(time);

    if (standalone)
    {
      clear_buffer();
      have_last_time = false;
    }
    else if (!have_last_time)
    {
      have_last_time = true;
      last_time = time;
      append_frame(time, in);
    }
    else if (time == last_time)
    {
      // Redraw of the same video time: replace the stored frame in place.
      std::copy(in, in + frame_size, buffer.back().pixels);
      buffer.back().time = time;
    }
    else if (time < last_time)
    {
      // Time went backwards: start a new playback segment.
      clear_buffer();
      last_time = time;
      append_frame(time, in);
    }
    else
    {
      last_time = time;
      append_frame(time, in);
    }

    // Reclaim frames outside the retainable range: keep every actual frame of
    // the most recent second, plus one boundary frame older than that second.
    // This never depends on the current delay, so growing DelayTime can still
    // reach a full second of history.
    if (!standalone)
      prune(time);

    if (standalone)
    {
      // Standalone frame: output this input directly, retain nothing.
      std::copy(in, in + frame_size, out);
      return;
    }

    // Select the newest frame not later than (time - delay); fall back to the
    // oldest frame when history does not reach that far back.  Zero delay
    // naturally selects the current frame.
    const double target = time - delay;
    const Frame* best = &buffer.front();
    for (std::deque<Frame>::const_iterator i = buffer.begin();
         i != buffer.end(); ++i)
    {
      if (i->time <= target)
        best = &(*i);
      else
        break;  // buffer is chronological
    }
    std::copy(best->pixels, best->pixels + frame_size, out);
  }

private:
  struct Frame
  {
    Frame(double t, uint32_t* p) : time(t), pixels(p) {}
    double time;
    uint32_t* pixels;
  };

  unsigned int frame_size;
  double delay;
  std::deque<Frame> buffer;   // Chronological, unique video times.
  double last_time;
  bool have_last_time;

  void clear_buffer()
  {
    while (!buffer.empty())
    {
      delete[] buffer.front().pixels;
      buffer.pop_front();
    }
  }

  void append_frame(double time, const uint32_t* in)
  {
    uint32_t* pixels = new uint32_t[frame_size];
    std::copy(in, in + frame_size, pixels);
    buffer.push_back(Frame(time, pixels));
  }

  void prune(double current_time)
  {
    // Frames strictly older than one second before the current time are
    // candidates for removal, but the single newest one of them is kept as
    // boundary support for a delay grown to a full second.
    const double cutoff = current_time - 1.0;
    while (buffer.size() > 2 && buffer[1].time < cutoff)
    {
      delete[] buffer.front().pixels;
      buffer.pop_front();
    }
    // After this, either fewer than two frames remain, or the second-oldest
    // frame lies within the last second: the oldest frame is then the single
    // boundary support preceding the retained one-second window.
  }
};


static frei0r::construct<delay0r> plugin("delay0r",
				  "video delay",
				  "Martin Bayer",
				  0,2);
