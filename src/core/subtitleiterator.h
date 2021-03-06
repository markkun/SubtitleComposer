#ifndef SUBTITLEITERATOR_H
#define SUBTITLEITERATOR_H

/*
 * Copyright (C) 2007-2009 Sergio Pistone <sergio_pistone@yahoo.com.ar>
 * Copyright (C) 2010-2018 Mladen Milinkovic <max@smoothware.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "core/range.h"
#include "core/rangelist.h"
#include "core/subtitle.h"

#include <QObject>
#include <QList>

namespace SubtitleComposer {
class SubtitleLine;
class Subtitle;
class SubtitleIterator : public QObject
{
	Q_OBJECT
	Q_PROPERTY(int index READ index WRITE toIndex)

public:
	enum {
		AfterLast = -1,
		BehindFirst = -2,
		Invalid = -3
	};

	explicit SubtitleIterator(const Subtitle &subtitle, const RangeList &ranges = Range::full(), bool toLast = false);
	SubtitleIterator(const SubtitleIterator &it);
	SubtitleIterator & operator=(const SubtitleIterator &it);
	virtual ~SubtitleIterator();

	RangeList ranges() const;

	void toFirst();
	void toLast();
	bool toIndex(int index);

	inline int index() { return m_index; }
	inline int firstIndex() { return m_index == Invalid ? -1 : m_ranges.firstIndex(); }
	inline int lastIndex() { return m_index == Invalid ? -1 : m_ranges.lastIndex(); }

	inline SubtitleLine * current() const { return m_index < 0 ? nullptr : const_cast<Subtitle *>(m_subtitle)->line(m_index); }
	inline operator SubtitleLine *() const { return m_index < 0 ? nullptr : const_cast<Subtitle *>(m_subtitle)->line(m_index); }

	SubtitleIterator & operator++();
	SubtitleIterator & operator+=(int steps);
	SubtitleIterator & operator--();
	SubtitleIterator & operator-=(int steps);

private:
	const Subtitle *m_subtitle;
	RangeList m_ranges;
	int m_index;
	RangeList::ConstIterator m_rangesIterator;
};
}

#endif
