/*
	Audio File Library
	Copyright (C) 2013 Michael Pruett <michael@68k.org>

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Lesser General Public
	License as published by the Free Software Foundation; either
	version 2.1 of the License, or (at your option) any later version.

	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public
	License along with this library; if not, write to the
	Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
	Boston, MA  02110-1301  USA
*/

#ifndef PacketTable_h
#define PacketTable_h

#include "Shared.h"

#include <audiofile.h>

#include <stdint.h>
#include <sys/types.h>
#include <vector>

class PacketTable : public Shared<PacketTable>
{
public:
	PacketTable();
	PacketTable(int64_t numValidFrames,
		int32_t primingFrames,
		int32_t remainderFrames);
	~PacketTable();

	size_t numPackets() const { return m_bytesPerPacket.size(); }
	int64_t numValidFrames() const { return m_numValidFrames; }
	void setNumValidFrames(int64_t numValidFrames);
	int32_t primingFrames() const { return m_primingFrames; }
	void setPrimingFrames(int32_t primingFrames);
	int32_t remainderFrames() const { return m_remainderFrames; }
	void setRemainderFrames(int32_t remainderFrames);

	void append(size_t bytesPerPacket);
	size_t bytesPerPacket(size_t packet) const { return m_bytesPerPacket[packet]; }
	AFfileoffset startOfPacket(size_t packet) const;
	/*
		Remove packet entries at and beyond the given count. This is used
		to discard a provisional trailing packet which was written during
		a sync but is not yet part of the durable stream.
	*/
	void truncatePackets(size_t packetCount);

	/* Sum of the sizes of all packets currently in the table. */
	AFfileoffset totalBytes() const { return m_totalBytes; }

private:
	int64_t m_numValidFrames;
	int32_t m_primingFrames;
	int32_t m_remainderFrames;

	AFfileoffset m_totalBytes;

	std::vector<size_t> m_bytesPerPacket;
};

#endif
