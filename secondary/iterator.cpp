// Copyright (c) 2020-2022, Manticore Software LTD (https://manticoresearch.com)
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

#include "iterator.h"
#include "secondary.h"
#include "delta.h"
#include "interval.h"

namespace SI
{

using namespace columnar;

static const int g_iBlocksReaderBufSize { 1024 };

class RowidIterator_c : public columnar::BlockIterator_i
{
public:
				RowidIterator_c ( Packing_e eType, uint64_t uRowStart, std::shared_ptr<columnar::FileReader_c> & pReader, std::shared_ptr<IntCodec_i> & pCodec, const RowidRange_t & tBounds );

	bool		HintRowID ( uint32_t tRowID ) override;
	bool		GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock ) override;
	int64_t		GetNumProcessed() const override;

private:
	Packing_e			m_eType = Packing_e::TOTAL;
	uint64_t			m_uRowStart = 0;
	std::shared_ptr<columnar::FileReader_c> m_pReader { nullptr };
	std::shared_ptr<IntCodec_i>	m_pCodec { nullptr };
	int64_t				m_iMetaOffset = 0;
	int64_t				m_iDataOffset = 0;
	const RowidRange_t	m_tBounds;

	bool				m_bStarted = false;
	bool				m_bStopped = false;

	uint32_t			m_uRowMin = 0;
	uint32_t			m_uRowMax = 0;
	int					m_iCurBlock = 0;
	SpanResizeable_T<uint32_t>	m_dRows;
	SpanResizeable_T<uint32_t>	m_dMinMax;
	SpanResizeable_T<uint32_t>	m_dBlockOffsets;
	SpanResizeable_T<uint32_t>	m_dTmp;
	BitVec_c			m_dMatchingBlocks{0};

	bool	StartBlock ( Span_T<uint32_t> & dRowIdBlock );
	bool	NextBlock ( Span_T<uint32_t> & dRowIdBlock );

	void	DecodeDeltaVector ( SpanResizeable_T<uint32_t> & dDecoded );
	void	MarkMatchingBlocks();
};


RowidIterator_c::RowidIterator_c ( Packing_e eType, uint64_t uRowStart, std::shared_ptr<columnar::FileReader_c> & pReader, std::shared_ptr<IntCodec_i> & pCodec, const RowidRange_t & tBounds )
	: m_eType ( eType )
	, m_uRowStart ( uRowStart )
	, m_pReader ( pReader )
	, m_pCodec ( pCodec )
	, m_tBounds ( tBounds )
{
	m_iMetaOffset = m_pReader->GetPos();
}


bool RowidIterator_c::HintRowID ( uint32_t tRowID )
{
	//assert ( 0 && "NIY" );
	// implement proper rewinding in all packings
	return !m_bStopped;
}


bool RowidIterator_c::GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	if ( m_bStopped )
		return false;
	if ( !m_bStarted )
		return StartBlock ( dRowIdBlock );

	return NextBlock ( dRowIdBlock );
}


int64_t	RowidIterator_c::GetNumProcessed() const
{
	return 0;
}


void RowidIterator_c::MarkMatchingBlocks()
{
	m_dMatchingBlocks.Resize ( m_dBlockOffsets.size() );
	if ( !m_tBounds.m_bHasRange )
	{
		m_dMatchingBlocks.SetAllBits();
		return;
	}

	Interval_T<uint32_t> tRowidBounds ( m_tBounds.m_uMin, m_tBounds.m_uMax );
	for ( size_t i = 0; i < m_dBlockOffsets.size(); i++ )
		if ( tRowidBounds.Overlaps ( { m_dMinMax[i<<1], m_dMinMax[(i<<1)+1] } ) )
			m_dMatchingBlocks.BitSet(i);
}


bool RowidIterator_c::StartBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	m_bStarted = true;
	switch ( m_eType )
	{
	case Packing_e::ROW:
		m_bStopped = true;
		m_uRowMin = m_uRowMax = m_uRowStart;
		if ( !m_tBounds.m_bHasRange || ( m_tBounds.m_uMin<=m_uRowStart && m_uRowStart<=m_tBounds.m_uMax ) )
		{
			m_dRows.resize(1);
			m_dRows[0] = m_uRowStart;
		}
		break;

	case Packing_e::ROW_BLOCK:
		m_pReader->Seek ( m_iMetaOffset + m_uRowStart );
		// FIXME!!! block length larger dRowIdBlock
		m_bStopped = true;
		m_uRowMin = m_pReader->Unpack_uint32();
		m_uRowMax = m_pReader->Unpack_uint32() + m_uRowMin;
		DecodeDeltaVector ( m_dRows );
		break;

	case Packing_e::ROW_BLOCKS_LIST:
		m_pReader->Seek ( m_iMetaOffset + m_uRowStart );
		DecodeDeltaVector ( m_dMinMax );
		DecodeDeltaVector ( m_dBlockOffsets );
		m_iDataOffset = m_pReader->GetPos();

		MarkMatchingBlocks();
		if ( !m_dMatchingBlocks.GetLength() )
		{
			m_bStopped = true;
			return false;
		}

		m_iCurBlock = 0;
		return NextBlock(dRowIdBlock);

	default:				// FIXME!!! handle ROW_FULLSCAN
		m_bStopped = true;
		break;
	}

	dRowIdBlock = Span_T<uint32_t> ( m_dRows );
	return ( !dRowIdBlock.empty() );
}


bool RowidIterator_c::NextBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	assert ( m_bStarted && !m_bStopped );
	assert ( m_eType==Packing_e::ROW_BLOCKS_LIST );

	if ( m_iCurBlock>=m_dMatchingBlocks.GetLength() )
	{
		m_bStopped = true;
		return false;
	}

	m_iCurBlock = m_dMatchingBlocks.Scan(m_iCurBlock);
	if ( m_iCurBlock>=m_dMatchingBlocks.GetLength() )
	{
		m_bStopped = true;
		return false;
	}

	int64_t iBlockSize = m_dBlockOffsets[m_iCurBlock];
	int64_t iBlockOffset = m_iCurBlock ? ( m_dBlockOffsets[m_iCurBlock-1]): 0;
	iBlockSize -= iBlockOffset;

	m_pReader->Seek ( m_iDataOffset + ( iBlockOffset << 2 ) );

	m_dTmp.resize(iBlockSize);
	ReadVectorData ( m_dTmp, *m_pReader );
	m_pCodec->Decode ( m_dTmp, m_dRows );
	ComputeInverseDeltas ( m_dRows, true );

	m_iCurBlock++;

	dRowIdBlock = Span_T<uint32_t>(m_dRows);
	return ( !dRowIdBlock.empty() );
}


void RowidIterator_c::DecodeDeltaVector ( SpanResizeable_T<uint32_t> & dDecoded )
{
	m_dTmp.resize(0);
	ReadVectorLen32 ( m_dTmp, *m_pReader );
	m_pCodec->Decode ( m_dTmp, dDecoded );
	ComputeInverseDeltas ( dDecoded, true );
}

/////////////////////////////////////////////////////////////////////

columnar::BlockIterator_i * CreateRowidIterator ( Packing_e eType, uint64_t uRowStart, std::shared_ptr<columnar::FileReader_c> & pReader, std::shared_ptr<IntCodec_i> & pCodec, const RowidRange_t & tBounds, bool bCreateReader, std::string & sError )
{
	std::shared_ptr<columnar::FileReader_c> tBlocksReader { nullptr };
	if ( bCreateReader && eType==Packing_e::ROW_BLOCKS_LIST )
	{
		tBlocksReader.reset ( new columnar::FileReader_c() );
		if ( !tBlocksReader->Open ( pReader->GetFilename(), g_iBlocksReaderBufSize, sError ) )
			tBlocksReader.reset();
		else
			tBlocksReader->Seek ( pReader->GetPos() );
	}

	return new RowidIterator_c ( eType, uRowStart, ( tBlocksReader ? tBlocksReader : pReader ), pCodec, tBounds );
}

}