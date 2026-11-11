/*
 * Independent reference model for delay0r temporal history.
 *
 * Straightforward implementation of the header contract; independent from
 * the production plugin's code and selection logic.
 */
#include "delay_ref.hpp"

#include <algorithm>
#include <cmath>

DelayRef::DelayRef(unsigned int width, unsigned int height)
  : frame_size(width * height),
    delay(0.0),
    have_last_time(false),
    standalone_frame(frame_size),
    last_was_standalone(false)
{
}

void
DelayRef::set_delay(double delay_seconds)
{
  delay = delay_seconds;
}

void
DelayRef::clear()
{
  frames.clear();
}

void
DelayRef::prune(double current_time)
{
  const double cutoff = current_time - 1.0;
  while (frames.size() > 2 && frames[1].time < cutoff)
    frames.pop_front();
  // After this, the oldest entry is the single boundary frame preceding the
  // retained one-second window (or the deque holds at most two frames).
}

bool
DelayRef::observe(double time, const uint32_t* in)
{
  last_was_standalone = !std::isfinite(time);

  if (last_was_standalone)
  {
    clear();
    have_last_time = false;
    std::copy(in, in + frame_size, standalone_frame.begin());
    return true;
  }

  if (!have_last_time)
  {
    Entry entry;
    entry.time = time;
    entry.pixels.assign(in, in + frame_size);
    frames.push_back(std::move(entry));
    last_time = time;
    have_last_time = true;
  }
  else if (time == last_time)
  {
    // Redraw of the same video time: replace in place, no extra entry.
    std::copy(in, in + frame_size, frames.back().pixels.begin());
    frames.back().time = time;
  }
  else if (time < last_time)
  {
    // Time went backwards: new playback segment.
    clear();
    Entry entry;
    entry.time = time;
    entry.pixels.assign(in, in + frame_size);
    frames.push_back(std::move(entry));
    last_time = time;
  }
  else
  {
    Entry entry;
    entry.time = time;
    entry.pixels.assign(in, in + frame_size);
    frames.push_back(std::move(entry));
    last_time = time;
  }

  prune(time);
  return false;
}

const std::vector<uint32_t>&
DelayRef::selected() const
{
  if (last_was_standalone)
    return standalone_frame;

  const double target = last_time - delay;
  const Entry* best = &frames.front();
  for (std::deque<Entry>::const_iterator i = frames.begin();
       i != frames.end(); ++i)
  {
    if (i->time <= target)
      best = &(*i);
    else
      break;  // chronological list
  }
  return best->pixels;
}
