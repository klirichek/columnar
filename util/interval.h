// Copyright (c) 2020-2021, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
//
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

#pragma once

namespace columnar
{

template<typename T, bool LEFT_CLOSED, bool RIGHT_CLOSED, bool LEFT_UNBOUNDED = false, bool RIGHT_UNBOUNDED = false>
inline bool ValueInInterval ( T tValue, T tMin, T tMax )
{
	if ( LEFT_UNBOUNDED )
		return RIGHT_CLOSED ? ( tValue<=tMax ) : ( tValue<tMax );

	if ( RIGHT_UNBOUNDED )
		return  LEFT_CLOSED ? ( tValue>=tMin ) : ( tValue>tMin );

	return ( LEFT_CLOSED ? ( tValue>=tMin ) : ( tValue>tMin ) ) && ( RIGHT_CLOSED ? ( tValue<=tMax ) : ( tValue<tMax ) );
}

template<typename T>
struct Interval_T
{
	T m_tStart;
	T m_tEnd;

	Interval_T() = default;
	Interval_T ( T tStart, T tEnd )
		: m_tStart ( tStart )
		, m_tEnd ( tEnd )
	{}

	bool operator< ( const Interval_T & tOther ) const
	{
		return ( m_tStart<tOther.m_tStart || ( m_tStart==tOther.m_tStart && m_tEnd<tOther.m_tEnd ) );
	}

	bool Overlaps ( const Interval_T & tOther ) const
	{
		return ( m_tStart<=tOther.m_tEnd && tOther.m_tStart<=m_tEnd );
	}
};

template<>
inline bool Interval_T<float>::operator< ( const Interval_T<float> & tOther ) const
{
	return ( m_tStart<tOther.m_tStart || ( FloatEqual ( m_tStart, tOther.m_tStart ) && m_tEnd<tOther.m_tEnd ) );
}

} // namespace columnar
