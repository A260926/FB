// basisu_texture_job.cpp - pausable/cancellable ETC1S .basis video texture playback.
// Copyright (C) 2019-2026 Binomial LLC. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "basisu_texture_job.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace basist
{
	namespace
	{
		struct frame_slice_layout
		{
			uint32_t m_rgb_ofs;
			uint32_t m_rgb_len;
			uint32_t m_alpha_ofs;
			uint32_t m_alpha_len;

			frame_slice_layout() :
				m_rgb_ofs(0), m_rgb_len(0), m_alpha_ofs(0), m_alpha_len(0)
			{
			}
		};
	}

	struct etc1s_texture_player::impl
	{
		bool m_open;
		std::vector<uint8_t> m_file;
		std::unique_ptr<basisu_transcoder> m_decoder;
		std::vector<frame_slice_layout> m_frames;

		uint32_t m_total_images;
		bool m_has_alpha;
		uint32_t m_num_blocks_x;
		uint32_t m_num_blocks_y;
		uint32_t m_orig_width;
		uint32_t m_orig_height;

		// Committed previous-frame reference planes (level 0 only), kept until the next successful publish.
		basisu::vector<uint32_t> m_committed_rgb_refs;
		basisu::vector<uint32_t> m_committed_alpha_refs;

		// Working copy of the reference planes; entropy decoding mutates it in place.
		basisu::vector<uint32_t> m_working_rgb_refs;
		basisu::vector<uint32_t> m_working_alpha_refs;

		etc1s_block_decoder m_rgb_block_decoder;
		etc1s_block_decoder m_alpha_block_decoder;

		// The two RGBA32 frames: the published one and the one being worked on.
		std::vector<uint8_t> m_front_pixels;
		std::vector<uint8_t> m_work_pixels;
		texture_job_frame m_front;

		bool m_working;
		bool m_frame_complete;
		uint32_t m_working_frame;
		uint32_t m_next_frame;

		uint32_t m_cur_block_x;
		uint32_t m_cur_block_y;
		bool m_alpha_half_pending;

		texture_job_progress m_progress;

		impl() :
			m_open(false),
			m_total_images(0),
			m_has_alpha(false),
			m_num_blocks_x(0),
			m_num_blocks_y(0),
			m_orig_width(0),
			m_orig_height(0),
			m_working(false),
			m_frame_complete(false),
			m_working_frame(0),
			m_next_frame(0),
			m_cur_block_x(0),
			m_cur_block_y(0),
			m_alpha_half_pending(false)
		{
		}

		void clear_playback_state()
		{
			m_committed_rgb_refs.clear();
			m_committed_alpha_refs.clear();
			m_working_rgb_refs.clear();
			m_working_alpha_refs.clear();

			m_rgb_block_decoder.clear();
			m_alpha_block_decoder.clear();

			m_front_pixels.clear();
			m_work_pixels.clear();
			m_front = texture_job_frame();

			m_working = false;
			m_frame_complete = false;
			m_working_frame = 0;
			m_next_frame = 0;
			m_cur_block_x = 0;
			m_cur_block_y = 0;
			m_alpha_half_pending = false;

			m_progress = texture_job_progress();
		}

		void discard_working_frame()
		{
			// The per-frame counters (including the half-block indicator) are deliberately left intact so that the
			// work done by the cancelled/failed attempt can still be queried; begin_frame() clears them.
			m_working = false;
			m_alpha_half_pending = false;
			m_working_rgb_refs.clear();
			m_working_alpha_refs.clear();
		}

		texture_job_result fail_working_frame()
		{
			discard_working_frame();
			m_frame_complete = false;
			return texture_job_result::kTextureJobDecodeError;
		}

		bool expand_rgb_block(uint32_t endpoint_index, uint32_t selector_index)
		{
			const basisu_lowlevel_etc1s_transcoder::endpoint_vec& endpoints = m_decoder->get_lowlevel_etc1s_decoder().get_endpoints();
			const basisu_lowlevel_etc1s_transcoder::selector_vec& selectors = m_decoder->get_lowlevel_etc1s_decoder().get_selectors();

			const etc1s_rgba32_channel_mode mode = m_has_alpha ? cEtc1sRGBA32RGB : cEtc1sRGBA32RGBA;
			etc1s_expand_block_rgba32(endpoints[endpoint_index], selectors[selector_index], mode,
				m_work_pixels.data(), m_orig_width, m_orig_height, m_cur_block_x, m_cur_block_y);
			return true;
		}

		bool expand_alpha_block(uint32_t endpoint_index, uint32_t selector_index)
		{
			const basisu_lowlevel_etc1s_transcoder::endpoint_vec& endpoints = m_decoder->get_lowlevel_etc1s_decoder().get_endpoints();
			const basisu_lowlevel_etc1s_transcoder::selector_vec& selectors = m_decoder->get_lowlevel_etc1s_decoder().get_selectors();

			etc1s_expand_block_rgba32(endpoints[endpoint_index], selectors[selector_index], cEtc1sRGBA32Alpha,
				m_work_pixels.data(), m_orig_width, m_orig_height, m_cur_block_x, m_cur_block_y);
			return true;
		}

		bool begin_slice_decoders()
		{
			const frame_slice_layout& layout = m_frames[m_working_frame];
			basisu_lowlevel_etc1s_transcoder& lowlevel = m_decoder->get_lowlevel_etc1s_decoder();

			if (!lowlevel.begin_block_decoder(m_rgb_block_decoder,
				m_file.data() + layout.m_rgb_ofs, layout.m_rgb_len,
				m_num_blocks_x, m_num_blocks_y, true, false, &m_working_rgb_refs))
			{
				return false;
			}

			if (m_has_alpha)
			{
				if (!lowlevel.begin_block_decoder(m_alpha_block_decoder,
					m_file.data() + layout.m_alpha_ofs, layout.m_alpha_len,
					m_num_blocks_x, m_num_blocks_y, true, true, &m_working_alpha_refs))
				{
					return false;
				}
			}

			return true;
		}

		void advance_block_position()
		{
			m_cur_block_x++;
			if (m_cur_block_x == m_num_blocks_x)
			{
				m_cur_block_x = 0;
				m_cur_block_y++;
			}
		}

		bool frame_finished() const
		{
			return m_cur_block_y >= m_num_blocks_y;
		}

		texture_job_result publish_frame()
		{
			if (!m_rgb_block_decoder.finish())
				return fail_working_frame();

			if (m_has_alpha && !m_alpha_block_decoder.finish())
				return fail_working_frame();

			// Commit pixels and both reference planes together, exactly once, on the last successful source block.
			// The reference planes are staged first so that a successful return is the only way either plane moves
			// from "working" to "committed".
			m_committed_rgb_refs = std::move(m_working_rgb_refs);
			m_committed_alpha_refs = std::move(m_working_alpha_refs);
			m_working_rgb_refs.clear();
			m_working_alpha_refs.clear();

			m_front_pixels.swap(m_work_pixels);

			m_front.m_frame_index = m_working_frame;
			m_front.m_pixels = m_front_pixels.data();
			m_front.m_width = m_orig_width;
			m_front.m_height = m_orig_height;

			m_progress.m_published_frames++;
			m_progress.m_half_block_pending = false;

			m_working = false;
			m_frame_complete = true;
			m_alpha_half_pending = false;
			m_next_frame = m_working_frame + 1;

			return texture_job_result::kTextureJobFrameComplete;
		}
	};

	etc1s_texture_player::etc1s_texture_player() :
		m_pImpl(new impl())
	{
	}

	etc1s_texture_player::~etc1s_texture_player()
	{
		delete m_pImpl;
	}

	bool etc1s_texture_player::open(const void* pData, uint32_t data_size)
	{
		if ((!pData) || (!data_size))
			return false;

		// The transcoder requires its global lookup tables even for the low-level ETC1S path.
		basisu_transcoder_init();

		// Build the complete new session in local state first, so a failed open() cannot disturb the current instance.
		std::unique_ptr<basisu_transcoder> decoder(new basisu_transcoder());

		std::vector<uint8_t> file(static_cast<const uint8_t*>(pData), static_cast<const uint8_t*>(pData) + data_size);
		const uint8_t* pFile = file.data();
		const uint32_t file_size = static_cast<uint32_t>(file.size());

		// Container + codebook validation.
		if (!decoder->validate_file_checksums(pFile, file_size, true))
			return false;

		basisu_file_info file_info;
		if (!decoder->get_file_info(pFile, file_size, file_info))
			return false;

		// Only .basis ETC1S VideoFrames, mip 0 only.
		if (file_info.m_tex_type != cBASISTexTypeVideoFrames)
			return false;
		if ((file_info.m_tex_format != basis_tex_format::cETC1S) || (!file_info.m_etc1s))
			return false;
		if ((!file_info.m_total_images) || (file_info.m_image_mipmap_levels.size() != file_info.m_total_images))
			return false;

		for (uint32_t i = 0; i < file_info.m_total_images; i++)
		{
			if (file_info.m_image_mipmap_levels[i] != 1)
				return false;
		}

		if (file_info.m_block_width != 4 || file_info.m_block_height != 4)
			return false;

		if (!decoder->start_transcoding(pFile, file_size))
			return false;

		basisu_lowlevel_etc1s_transcoder& lowlevel = decoder->get_lowlevel_etc1s_decoder();
		if ((!lowlevel.get_endpoints().size()) || (!lowlevel.get_selectors().size()) || (!lowlevel.get_selector_history_buf_size()))
			return false;

		const bool has_alpha = file_info.m_has_alpha_slices;

		std::vector<frame_slice_layout> frames(file_info.m_total_images);
		uint32_t orig_width = 0, orig_height = 0, num_blocks_x = 0, num_blocks_y = 0;

		for (uint32_t image_index = 0; image_index < file_info.m_total_images; image_index++)
		{
			basisu_image_level_info level_info;
			if (!decoder->get_image_level_info(pFile, file_size, level_info, image_index, 0))
				return false;

			if (level_info.m_level_index != 0)
				return false;

			if ((!level_info.m_orig_width) || (!level_info.m_orig_height) ||
				(!level_info.m_num_blocks_x) || (!level_info.m_num_blocks_y))
				return false;

			if (level_info.m_alpha_flag != has_alpha)
				return false;

			if (!image_index)
			{
				orig_width = level_info.m_orig_width;
				orig_height = level_info.m_orig_height;
				num_blocks_x = level_info.m_num_blocks_x;
				num_blocks_y = level_info.m_num_blocks_y;
			}
			else if ((level_info.m_orig_width != orig_width) || (level_info.m_orig_height != orig_height) ||
				(level_info.m_num_blocks_x != num_blocks_x) || (level_info.m_num_blocks_y != num_blocks_y))
			{
				// The player keeps one frame geometry; video frames must all match.
				return false;
			}

			frame_slice_layout& layout = frames[image_index];
			layout.m_rgb_ofs = level_info.m_rgb_file_ofs;
			layout.m_rgb_len = level_info.m_rgb_file_len;

			if (has_alpha)
			{
				if ((!level_info.m_alpha_file_len) || level_info.m_alpha_file_ofs >= file_size)
					return false;
				layout.m_alpha_ofs = level_info.m_alpha_file_ofs;
				layout.m_alpha_len = level_info.m_alpha_file_len;
			}

			if ((static_cast<uint64_t>(layout.m_rgb_ofs) + layout.m_rgb_len) > file_size)
				return false;
			if (has_alpha && ((static_cast<uint64_t>(layout.m_alpha_ofs) + layout.m_alpha_len) > file_size))
				return false;
		}

		// Commit the new session; only now is the previous instance state replaced.
		impl& s = *m_pImpl;
		s.m_open = false;
		s.m_file.swap(file);
		file.clear();
		s.m_decoder.swap(decoder);
		s.m_frames.swap(frames);
		s.m_total_images = file_info.m_total_images;
		s.m_has_alpha = has_alpha;
		s.m_num_blocks_x = num_blocks_x;
		s.m_num_blocks_y = num_blocks_y;
		s.m_orig_width = orig_width;
		s.m_orig_height = orig_height;
		s.clear_playback_state();

		// Exactly two RGBA32 frames are retained: the published one and the working one.
		const size_t pixel_bytes = static_cast<size_t>(orig_width) * orig_height * 4;
		s.m_work_pixels.assign(pixel_bytes, 0);
		s.m_front_pixels.assign(pixel_bytes, 0);

		s.m_open = true;

		return true;
	}

	texture_job_result etc1s_texture_player::begin_frame(uint32_t image_index)
	{
		impl& s = *m_pImpl;

		if (!s.m_open)
			return texture_job_result::kTextureJobNotOpen;
		if (s.m_working)
			return texture_job_result::kTextureJobBusy;
		if ((image_index != s.m_next_frame) || (image_index >= s.m_total_images))
			return texture_job_result::kTextureJobInvalidFrame;

		// Fresh working references start from the last committed frame, so a cancelled or failed attempt
		// can never contaminate the committed references.
		s.m_working_rgb_refs = s.m_committed_rgb_refs;
		s.m_working_alpha_refs = s.m_committed_alpha_refs;

		s.m_working_frame = image_index;
		s.m_cur_block_x = 0;
		s.m_cur_block_y = 0;
		s.m_alpha_half_pending = false;
		s.m_frame_complete = false;

		s.m_progress.m_frame_blocks_completed = 0;
		s.m_progress.m_frame_alpha_blocks_completed = 0;
		s.m_progress.m_half_block_pending = false;

		if (!s.begin_slice_decoders())
		{
			s.discard_working_frame();
			return texture_job_result::kTextureJobDecodeError;
		}

		s.m_working = true;
		return texture_job_result::kTextureJobFrameStarted;
	}

	texture_job_result etc1s_texture_player::step(uint32_t budget)
	{
		impl& s = *m_pImpl;

		if (!s.m_open)
			return texture_job_result::kTextureJobNotOpen;

		if (!s.m_working)
			return s.m_frame_complete ? texture_job_result::kTextureJobFrameComplete : texture_job_result::kTextureJobIdle;

		if (!budget)
			return texture_job_result::kTextureJobRunning;

		while (budget > 0)
		{
			if (s.frame_finished())
				break;

			if (s.m_has_alpha && !s.m_alpha_half_pending)
			{
				// Each dual-slice position decodes alpha first, then RGB.
				if (!s.m_alpha_block_decoder.decode_block(s.m_cur_block_x, s.m_cur_block_y))
					return s.fail_working_frame();

				s.expand_alpha_block(s.m_alpha_block_decoder.get_endpoint_index(), s.m_alpha_block_decoder.get_selector_index());

				s.m_progress.m_total_source_blocks_decoded++;
				s.m_progress.m_frame_alpha_blocks_completed++;
				s.m_alpha_half_pending = true;
				s.m_progress.m_half_block_pending = true;
				budget--;

				if (!budget)
					return texture_job_result::kTextureJobRunning;
			}

			if (!s.m_rgb_block_decoder.decode_block(s.m_cur_block_x, s.m_cur_block_y))
				return s.fail_working_frame();

			s.expand_rgb_block(s.m_rgb_block_decoder.get_endpoint_index(), s.m_rgb_block_decoder.get_selector_index());

			s.m_progress.m_total_source_blocks_decoded++;
			s.m_alpha_half_pending = false;
			s.m_progress.m_half_block_pending = false;
			s.advance_block_position();
			s.m_progress.m_frame_blocks_completed++;
			budget--;

			if (s.frame_finished())
				break;
		}

		if (!s.frame_finished())
			return texture_job_result::kTextureJobRunning;

		return s.publish_frame();
	}

	void etc1s_texture_player::cancel()
	{
		impl& s = *m_pImpl;

		// Idle cancellation is a no-op, and cancellation never touches the published frame or committed refs.
		if (!s.m_working)
			return;

		s.discard_working_frame();
	}

	const texture_job_frame& etc1s_texture_player::front() const
	{
		return m_pImpl->m_front;
	}

	const texture_job_progress& etc1s_texture_player::progress() const
	{
		return m_pImpl->m_progress;
	}

	void etc1s_texture_player::close()
	{
		impl& s = *m_pImpl;

		s.clear_playback_state();
		s.m_frames.clear();
		s.m_decoder.reset();
		s.m_file.clear();
		s.m_file.shrink_to_fit();
		s.m_total_images = 0;
		s.m_has_alpha = false;
		s.m_num_blocks_x = 0;
		s.m_num_blocks_y = 0;
		s.m_orig_width = 0;
		s.m_orig_height = 0;
		s.m_open = false;
	}
}
