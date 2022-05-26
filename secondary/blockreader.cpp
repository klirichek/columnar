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

#include "blockreader.h"
#include "secondary.h"

#include "pgm.h"
#include "iterator.h"
#include "delta.h"
#include "interval.h"

namespace SI
{

using namespace util;
using namespace common;

static const int g_iReaderBufSize { 256 };

static uint64_t GetValueBlock ( uint64_t uPos, int iValuesPerBlock )
{
	return uPos / iValuesPerBlock;
}

BlockIter_t::BlockIter_t ( const ApproxPos_t & tFrom, uint64_t uVal, uint64_t uBlocksCount, int iValuesPerBlock )
	: m_uVal ( uVal )
{
	m_iStart = GetValueBlock ( tFrom.m_iLo, iValuesPerBlock );
	m_iPos = GetValueBlock ( tFrom.m_iPos, iValuesPerBlock ) - m_iStart;
	m_iLast = GetValueBlock ( tFrom.m_iHi, iValuesPerBlock );

	if ( m_iStart+m_iPos>=uBlocksCount )
		m_iPos = uBlocksCount - 1 - m_iStart;
	if ( m_iLast>=uBlocksCount )
		m_iLast = uBlocksCount - m_iStart;
}

struct FindValueResult_t
{
	int m_iMatchedItem { -1 };
	int m_iCmp { 0 };
};

/////////////////////////////////////////////////////////////////////

template<typename VEC>
void DecodeBlock ( VEC & dDst, IntCodec_i * pCodec, SpanResizeable_T<uint32_t> & dBuf, FileReader_c & tReader )
{
	dBuf.resize ( 0 );
	ReadVectorLen32 ( dBuf, tReader );
	pCodec->Decode ( dBuf, dDst );
	ComputeInverseDeltas ( dDst, true );
}

template<typename VEC>
void DecodeBlockWoDelta ( VEC & dDst, IntCodec_i * pCodec, SpanResizeable_T<uint32_t> & dBuf, FileReader_c & tReader )
{
	dBuf.resize ( 0 );
	ReadVectorLen32 ( dBuf, tReader );
	pCodec->Decode ( dBuf, dDst );
}

/////////////////////////////////////////////////////////////////////

struct Int64ValueCmp_t
{
	bool operator()( const uint64_t & uVal1, const uint64_t & uVal2 ) const
	{
		return ( (int64_t)uVal1<(int64_t)uVal2 );
	}
};


struct FloatValueCmp_t
{
	float AsFloat ( const uint32_t & tVal ) const { return UintToFloat ( tVal ); }
	float AsFloat ( const float & fVal ) const { return fVal; }

	template< typename T1, typename T2 >
	bool operator()( const T1 & tVal1, const T2 & tVal2 ) const
	{
		return IsLess ( tVal1, tVal2 );
	}

	template< typename T1, typename T2 >
	bool IsLess ( const T1 & tVal1, const T2 & tVal2 ) const
	{
		return AsFloat( tVal1 ) < AsFloat( tVal2 );
	}
};

/////////////////////////////////////////////////////////////////////

class BlockReader_c : public BlockReader_i
{
public:
				BlockReader_c ( std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds );

	bool		Open ( const std::string & sFileName, std::string & sError ) override;
	void		CreateBlocksIterator ( const BlockIter_t & tIt, std::vector<BlockIterator_i *> & dRes ) override;
	void		CreateBlocksIterator ( const BlockIter_t & tIt, const Filter_t & tVal, std::vector<BlockIterator_i *> & dRes ) override { assert ( 0 && "Requesting range iterators from block reader" ); }
	const std::string & GetWarning() const override { return m_sWarning; }

protected:
	std::shared_ptr<FileReader_c> m_pFileReader { nullptr };
	std::shared_ptr<IntCodec_i>	m_pCodec { nullptr };
	std::string			m_sWarning;

	SpanResizeable_T<uint32_t> m_dTypes;
	SpanResizeable_T<uint32_t> m_dSizes;
	SpanResizeable_T<uint32_t> m_dRowStart;

	SpanResizeable_T<uint32_t> m_dBufTmp;
		
	uint64_t				m_uBlockBaseOff { 0 };
	std::vector<uint64_t>	m_dBlockOffsets;
	int						m_iLoadedBlock { -1 };
	int						m_iStartBlock { -1 };
	int64_t					m_iOffPastValues { -1 };

	RowidRange_t			m_tBounds;
	bool					m_bHaveBounds = false;

	// interface for value related methods
	virtual void			LoadValues () = 0;
	virtual FindValueResult_t FindValue ( uint64_t uRefVal ) const = 0;

	BlockIterator_i	*		CreateIterator ( int iItem );
	int						BlockLoadCreateIterator ( int iBlock, uint64_t uVal, std::vector<BlockIterator_i *> & dRes );
};


BlockReader_c::BlockReader_c ( std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds )
	: m_pCodec ( pCodec )
	, m_uBlockBaseOff ( uBlockBaseOff )
{
	assert ( m_pCodec.get() );
	m_bHaveBounds = !!pBounds;
	if ( m_bHaveBounds )
		m_tBounds = *pBounds;
}


bool BlockReader_c::Open ( const std::string & sFileName, std::string & sError )
{
	m_pFileReader.reset ( new FileReader_c() );
	return m_pFileReader->Open ( sFileName, g_iReaderBufSize, sError );
}


int BlockReader_c::BlockLoadCreateIterator ( int iBlock, uint64_t uVal, std::vector<BlockIterator_i *> & dRes )
{
	if ( iBlock!=-1 )
	{
		m_pFileReader->Seek ( m_dBlockOffsets[iBlock] );
		LoadValues();
		m_iLoadedBlock = m_iStartBlock + iBlock;
	}

	auto [iItem, iCmp] = FindValue ( uVal );
	if ( iItem!=-1 )
		dRes.emplace_back ( CreateIterator ( iItem ) );

	return iCmp;
}

void BlockReader_c::CreateBlocksIterator ( const BlockIter_t & tIt, std::vector<BlockIterator_i *> & dRes )
{
	m_iStartBlock = tIt.m_iStart;

	// load offsets of all blocks for the range
	m_dBlockOffsets.resize ( tIt.m_iLast - tIt.m_iStart + 1 );
	m_pFileReader->Seek ( m_uBlockBaseOff + tIt.m_iStart * sizeof ( uint64_t) );
	for ( int iBlock=0; iBlock<m_dBlockOffsets.size(); iBlock++ )
		m_dBlockOffsets[iBlock] = m_pFileReader->Read_uint64();

	// first check already loadded block in case it fits the range and it is not the best block that will be checked
	int iLastBlockChecked = -1;
	if ( m_iLoadedBlock!=m_iStartBlock+tIt.m_iPos && m_iStartBlock>=m_iLoadedBlock && m_iLoadedBlock<=tIt.m_iLast )
	{
		// if current block fits - exit even no matches
		if ( BlockLoadCreateIterator ( -1, tIt.m_uVal, dRes )==0 )
			return;

		iLastBlockChecked = m_iLoadedBlock;
	}

	// if best block fits - exit even no matches
	if ( BlockLoadCreateIterator ( tIt.m_iPos, tIt.m_uVal, dRes )==0 )
		return;

	for ( int iBlock=0; iBlock<=tIt.m_iLast - tIt.m_iStart; iBlock++ )
	{
		if ( iBlock==tIt.m_iPos || ( iLastBlockChecked!=-1 && m_iStartBlock+iBlock==iLastBlockChecked ) )
			continue;

		int iCmp = BlockLoadCreateIterator ( iBlock, tIt.m_uVal, dRes );

		// stop ckecking blocks in case
		// - found block where the value in values range
		// - checked block with the greater value
		if ( iCmp==0 || iCmp>0 )
			return;
	}
}


BlockIterator_i * BlockReader_c::CreateIterator ( int iItem )
{
	if ( m_iOffPastValues!=-1 )
	{
		// seek right after values to load the rest of the block content as only values could be loaded
		m_pFileReader->Seek ( m_iOffPastValues );
		m_iOffPastValues = -1;

		DecodeBlockWoDelta ( m_dTypes, m_pCodec.get(), m_dBufTmp, *m_pFileReader.get() );
		DecodeBlockWoDelta ( m_dSizes, m_pCodec.get(), m_dBufTmp, *m_pFileReader.get() );

		bool bLenDelta = !!m_pFileReader->Read_uint8();
		if ( bLenDelta )
			DecodeBlock ( m_dRowStart, m_pCodec.get(), m_dBufTmp, *m_pFileReader.get() );
		else
			DecodeBlockWoDelta ( m_dRowStart, m_pCodec.get(), m_dBufTmp, *m_pFileReader.get() );
	}

	return CreateRowidIterator ( (Packing_e)m_dTypes[iItem], m_dRowStart[iItem], m_pFileReader, m_pCodec, m_bHaveBounds ? &m_tBounds : nullptr, m_sWarning.empty(), m_sWarning );
}

/////////////////////////////////////////////////////////////////////

template<typename VALUE, bool FLOAT_VALUE>
class BlockReader_T : public BlockReader_c
{
public:
			BlockReader_T ( std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds ) : BlockReader_c ( pCodec, uBlockBaseOff, pBounds ) {}

private:
	SpanResizeable_T<VALUE> m_dValues;

	void	LoadValues() override;
	FindValueResult_t FindValue ( uint64_t uRefVal ) const override;
};

template<typename VALUE, bool FLOAT_VALUE>
void BlockReader_T<VALUE, FLOAT_VALUE>::LoadValues()
{
	DecodeBlock ( m_dValues, m_pCodec.get(), m_dBufTmp, *m_pFileReader.get() );
	m_iOffPastValues = m_pFileReader->GetPos();
}

template<>
FindValueResult_t BlockReader_T<uint32_t, false>::FindValue ( uint64_t uRefVal ) const
{
	uint32_t uVal = uRefVal;
	int iItem = binary_search_idx ( m_dValues, uVal );
	if ( iItem!=-1 )
		return FindValueResult_t { iItem, 0 };

	if ( !m_dValues.size() || ( m_dValues.size() && m_dValues.front()<=uVal && uVal<=m_dValues.back() ) )
		return FindValueResult_t { -1, 0 };

	return FindValueResult_t { -1, m_dValues.back()<uVal ? 1 : -1 };
}

template<>
FindValueResult_t BlockReader_T<uint64_t, false>::FindValue ( uint64_t uRefVal ) const
{
	const auto tFirst = m_dValues.begin();
	const auto tLast = m_dValues.end();
	auto tFound = std::lower_bound ( tFirst, tLast, uRefVal, Int64ValueCmp_t() );
	if ( tFound!=tLast && *tFound==uRefVal )
		return FindValueResult_t { (int)( tFound-tFirst ), 0 };

	if ( !m_dValues.size() || ( m_dValues.size() && (int64_t)m_dValues.front()<=(int64_t)uRefVal && (int64_t)uRefVal<=(int64_t)m_dValues.back() ) )
		return FindValueResult_t { -1, 0 };

	return FindValueResult_t { -1, (int64_t)m_dValues.back()<(int64_t)uRefVal ? 1 : -1 };
}

template<>
FindValueResult_t BlockReader_T<uint32_t, true>::FindValue ( uint64_t uRefVal ) const
{
	float fVal = UintToFloat ( uRefVal );
	const auto tFirst = m_dValues.begin();
	const auto tLast = m_dValues.end();
	auto tFound = std::lower_bound ( tFirst, tLast, fVal, FloatValueCmp_t() );

	if ( tFound!=tLast && FloatEqual ( *tFound, fVal ) )
		return FindValueResult_t { (int)( tFound-tFirst ), 0 };

	if ( !m_dValues.size() || ( m_dValues.size() && m_dValues.front()<=fVal && fVal<=m_dValues.back() ) )
		return FindValueResult_t { -1, 0 };

	return FindValueResult_t { -1, m_dValues.back()<fVal ? 1 : -1 };
}

/////////////////////////////////////////////////////////////////////

BlockReader_i * CreateBlockReader ( AttrType_e eType, const Settings_t & tSettings, uint64_t uBlockBaseOff, const RowidRange_t * pBounds )
{
	auto pCodec { std::shared_ptr<IntCodec_i> ( CreateIntCodec ( tSettings.m_sCompressionUINT32, tSettings.m_sCompressionUINT64 ) ) };
	assert(pCodec);

	switch ( eType )
	{
		case AttrType_e::UINT32:
		case AttrType_e::TIMESTAMP:
		case AttrType_e::UINT32SET:
			return new BlockReader_T<uint32_t, false> ( pCodec, uBlockBaseOff, pBounds );
			break;

		case AttrType_e::FLOAT:
			return new BlockReader_T<uint32_t, true> ( pCodec, uBlockBaseOff, pBounds );
			break;

		case AttrType_e::STRING:
		case AttrType_e::INT64:
		case AttrType_e::INT64SET:
			return new BlockReader_T<uint64_t, false> ( pCodec, uBlockBaseOff, pBounds );
			break;

		default: return nullptr;
	}
}

/////////////////////////////////////////////////////////////////////

template<typename T>
int CmpRange ( T tStart, T tEnd, const Filter_t & tRange )
{
	Interval_T<T> tIntBlock ( tStart, tEnd );

	Interval_T<T> tIntRange;
	if ( std::is_floating_point<T>::value )
		tIntRange = Interval_T<T> ( tRange.m_fMinValue, tRange.m_fMaxValue );
	else
		tIntRange = Interval_T<T> ( tRange.m_iMinValue, tRange.m_iMaxValue );

	if ( tRange.m_bLeftUnbounded )
		tIntRange.m_tStart = std::numeric_limits<T>::min();
	if ( tRange.m_bRightUnbounded )
		tIntRange.m_tEnd = std::numeric_limits<T>::max();

	if ( tIntBlock.Overlaps ( tIntRange ) )
		return 0;

	return ( tIntBlock<tIntRange ? -1 : 1 );
}

/////////////////////////////////////////////////////////////////////

class RangeReader_c : public BlockReader_i
{
public:
				RangeReader_c ( std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds );

	bool		Open ( const std::string & sFileName, std::string & sError ) override;
	void		CreateBlocksIterator ( const BlockIter_t & tIt, std::vector<BlockIterator_i *> & dRes ) override { assert ( 0 && "Requesting block iterators from range reader" ); }
	void		CreateBlocksIterator ( const BlockIter_t & tIt, const Filter_t & tVal, std::vector<BlockIterator_i *> & dRes ) override;
	const std::string & GetWarning() const override { return m_sWarning; }

protected:
	std::shared_ptr<FileReader_c> m_pOffReader { nullptr };
	std::shared_ptr<FileReader_c> m_pBlockReader { nullptr };
	std::string m_sWarning;

	std::shared_ptr<IntCodec_i> m_pCodec { nullptr };

	SpanResizeable_T<uint32_t> m_dTypes;
	SpanResizeable_T<uint32_t> m_dRowStart;

	SpanResizeable_T<uint32_t> m_dBufTmp;
		
	uint64_t		m_uBlockBaseOff = 0;
	RowidRange_t	m_tBounds;
	bool			m_bHaveBounds = false;

	// interface for value related methods
	virtual int		LoadValues () = 0;
	virtual bool	EvalRangeValue ( int iItem, const Filter_t & tRange ) const = 0;
	virtual int		CmpBlock ( const Filter_t & tRange ) const = 0;

	BlockIterator_i * CreateIterator ( int iItem, bool bLoad );
};


RangeReader_c::RangeReader_c ( std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds )
	: m_pCodec ( pCodec )
	, m_uBlockBaseOff ( uBlockBaseOff )
{
	assert ( m_pCodec.get() );
	m_bHaveBounds = !!pBounds;
	if ( m_bHaveBounds )
		m_tBounds = *pBounds;
}


bool RangeReader_c::Open ( const std::string & sFileName, std::string & sError )
{
	m_pOffReader.reset ( new FileReader_c() );
	m_pBlockReader.reset ( new FileReader_c() );
	return ( m_pOffReader->Open ( sFileName, g_iReaderBufSize, sError ) && m_pBlockReader->Open ( sFileName, g_iReaderBufSize, sError ) );
}


void RangeReader_c::CreateBlocksIterator ( const BlockIter_t & tIt, const Filter_t & tRange, std::vector<BlockIterator_i *> & dRes )
{
	int iBlockCur = tIt.m_iStart;
	m_pOffReader->Seek ( m_uBlockBaseOff + iBlockCur * sizeof ( uint64_t) );

	int iValCur = 0;
	int iValCount = 0;
	int iBlockItCreated = -1;

	// warmup
	for ( ; iBlockCur<=tIt.m_iLast; iBlockCur++ )
	{
		uint64_t uBlockOff = m_pOffReader->Read_uint64();
		m_pBlockReader->Seek ( uBlockOff );

		iValCount = LoadValues();

		int iCmpLast = CmpBlock ( tRange );
		if ( iCmpLast==1 )
			break;
		if ( iCmpLast==-1 )
			continue;

		for ( iValCur=0; iValCur<iValCount; iValCur++ )
		{
			if ( EvalRangeValue ( iValCur, tRange ) )
			{
				dRes.emplace_back ( CreateIterator ( iValCur, true ) );
				iBlockItCreated = iBlockCur;
				iValCur++;
				break;
			}
		}

		// get into search in case current block has values matched
		if ( iBlockItCreated!=-1 )
			break;
	}

	if ( iBlockItCreated==-1 )
		return;

	// check end block value vs EvalRange then add all values from block as iterators
	// for openleft find start via linear scan
	// cases :
	// - whole block
	// - skip left part of values in block 
	// - stop on scan all values in block
	// FIXME!!! stop checking on range passed values 


	for ( ;; )
	{
		if ( iValCur<iValCount )
		{
			// case: all values till the end of the block matched
			if ( EvalRangeValue ( iValCount-1, tRange ) )
			{
				for ( ; iValCur<iValCount; iValCur++ )
				{
					dRes.emplace_back ( CreateIterator ( iValCur, iBlockItCreated!=iBlockCur ) );
					iBlockItCreated = iBlockCur;
				}
			} else // case: values only inside the block matched, need to check every value
			{
				for ( ; iValCur<iValCount; iValCur++ )
				{
					if ( !EvalRangeValue ( iValCur, tRange ) )
						return;

					dRes.emplace_back ( CreateIterator ( iValCur, iBlockItCreated!=iBlockCur ) );
					iBlockItCreated = iBlockCur;
				}

				break;
			}
		}

		iBlockCur++;
		if ( iBlockCur>tIt.m_iLast )
			break;

		uint64_t uBlockOff = m_pOffReader->Read_uint64();
		m_pBlockReader->Seek ( uBlockOff );

		iValCount = LoadValues();
		iValCur = 0;
		assert ( iValCount );

		// matching is over
		if ( !EvalRangeValue ( 0, tRange ) )
			break;
	}
}


BlockIterator_i * RangeReader_c::CreateIterator ( int iItem, bool bLoad )
{
	if ( bLoad )
	{
		DecodeBlockWoDelta ( m_dTypes, m_pCodec.get(), m_dBufTmp, *m_pBlockReader.get() );

		bool bLenDelta = !!m_pBlockReader->Read_uint8();
		if ( bLenDelta )
			DecodeBlock ( m_dRowStart, m_pCodec.get(), m_dBufTmp, *m_pBlockReader.get() );
		else
			DecodeBlockWoDelta ( m_dRowStart, m_pCodec.get(), m_dBufTmp, *m_pBlockReader.get() );
	}

	return CreateRowidIterator ( (Packing_e)m_dTypes[iItem], m_dRowStart[iItem], m_pBlockReader, m_pCodec, m_bHaveBounds ? &m_tBounds : nullptr, m_sWarning.empty(), m_sWarning );
}

/////////////////////////////////////////////////////////////////////

template<typename STORE_VALUE, typename DST_VALUE>
class RangeReader_T : public RangeReader_c
{
public:
	RangeReader_T ( std::shared_ptr<IntCodec_i> & pCodec, uint64_t uBlockBaseOff, const RowidRange_t * pBounds ) : RangeReader_c ( pCodec, uBlockBaseOff, pBounds ) {}

private:
	SpanResizeable_T<STORE_VALUE> m_dValues;

	int LoadValues () override
	{
		DecodeBlock ( m_dValues, m_pCodec.get(), m_dBufTmp, *m_pBlockReader.get() );
		return m_dValues.size();
	}

	bool EvalRangeValue ( int iItem, const Filter_t & tRange ) const override
	{
		if ( std::is_floating_point<DST_VALUE>::value )
			return ValueInInterval<float> ( UintToFloat ( m_dValues[iItem] ), tRange );
		else
			return ValueInInterval<DST_VALUE> ( (DST_VALUE)m_dValues[iItem], tRange );
	}

	int CmpBlock ( const Filter_t & tRange ) const override
	{
		if ( std::is_floating_point<DST_VALUE>::value )
			return CmpRange<float> ( UintToFloat ( m_dValues.front() ), UintToFloat ( m_dValues.back() ), tRange );
		else
			return CmpRange<DST_VALUE> ( m_dValues.front(), m_dValues.back(), tRange );
	}
};

/////////////////////////////////////////////////////////////////////

BlockReader_i * CreateRangeReader ( AttrType_e eType, const Settings_t & tSettings, uint64_t uBlockBaseOff, const RowidRange_t * pBounds )
{
	auto pCodec { std::shared_ptr<IntCodec_i> ( CreateIntCodec ( tSettings.m_sCompressionUINT32, tSettings.m_sCompressionUINT64 ) ) };
	assert(pCodec);

	switch ( eType )
	{
	case AttrType_e::UINT32:
	case AttrType_e::TIMESTAMP:
	case AttrType_e::UINT32SET:
	case AttrType_e::BOOLEAN:
		return new RangeReader_T<uint32_t, uint32_t> ( pCodec, uBlockBaseOff, pBounds );
		break;

	case AttrType_e::FLOAT:
		return new RangeReader_T<uint32_t, float> ( pCodec, uBlockBaseOff, pBounds );
		break;

	case AttrType_e::INT64:
	case AttrType_e::INT64SET:
		return new RangeReader_T<uint64_t, int64_t> ( pCodec, uBlockBaseOff, pBounds );
		break;

	default: return nullptr;
	}
}

}