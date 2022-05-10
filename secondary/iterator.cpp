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

class RowidIterator_c : public BlockIteratorSize_i
{
public:
				RowidIterator_c ( Packing_e eType, uint64_t uRowStart, uint32_t uSize, std::shared_ptr<columnar::FileReader_c> & pReader, std::shared_ptr<IntCodec_i> & pCodec, const RowidRange_t & tBounds );

	bool		HintRowID ( uint32_t tRowID ) override;
	bool		GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock ) override;
	int64_t		GetNumProcessed() const override;
	uint32_t	GetSize() const override { return m_uSize; }

private:
	Packing_e			m_eType = Packing_e::TOTAL;
	uint64_t			m_uRowStart = 0;
	uint32_t			m_uSize = 0;
	std::shared_ptr<columnar::FileReader_c> m_pReader { nullptr };
	std::shared_ptr<IntCodec_i>	m_pCodec { nullptr };
	uint64_t			m_uLastOff = 0;
	const RowidRange_t m_tBounds;

	bool				m_bStarted = false;
	bool				m_bStopped = false;

	uint32_t			m_uRowMin = 0;
	uint32_t			m_uRowMax = 0;
	uint32_t			m_uCurBlock = 0;
	uint32_t			m_uBlocksCount = 0;
	SpanResizeable_T<uint32_t>	m_dRowsDecoded;
	std::vector<uint32_t>		m_dRowsRaw;

	bool	StartBlock ( Span_T<uint32_t> & dRowIdBlock );
	bool	NextBlock ( Span_T<uint32_t> & dRowIdBlock );
	void	DecodeRowsBlock();
};


RowidIterator_c::RowidIterator_c ( Packing_e eType, uint64_t uRowStart, uint32_t uSize, std::shared_ptr<columnar::FileReader_c> & pReader, std::shared_ptr<IntCodec_i> & pCodec, const RowidRange_t & tBounds )
	: m_eType ( eType )
	, m_uRowStart ( uRowStart )
	, m_uSize ( uSize )
	, m_pReader ( pReader )
	, m_pCodec ( pCodec )
	, m_tBounds ( tBounds )
{
	m_uLastOff = m_pReader->GetPos();
}


bool RowidIterator_c::HintRowID ( uint32_t tRowID )
{
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
			m_dRowsDecoded.resize ( 1 );
			m_dRowsDecoded[0] = m_uRowStart;
		}
		break;

	case Packing_e::ROW_BLOCK:
		m_pReader->Seek ( m_uLastOff + m_uRowStart );
		// FIXME!!! block length larger dRowIdBlock
		m_bStopped = true;
		DecodeRowsBlock();
		break;

	case Packing_e::ROW_BLOCKS_LIST:
		m_pReader->Seek ( m_uLastOff + m_uRowStart );
		// FIXME!!! block length larger dRowIdBlock
		m_uCurBlock = 1;
		m_uBlocksCount = m_pReader->Unpack_uint32();
		DecodeRowsBlock();
		dRowIdBlock = Span_T<uint32_t> ( m_dRowsDecoded );
		// decode could produce empty block in case it out of @rowid range
		while ( !m_bStopped && dRowIdBlock.empty() )
			NextBlock ( dRowIdBlock );
		break;

	default:				// FIXME!!! handle ROW_FULLSCAN
		m_bStopped = true;
		break;
	}

	dRowIdBlock = Span_T<uint32_t> ( m_dRowsDecoded );
	return ( !dRowIdBlock.empty() );
}


bool RowidIterator_c::NextBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	assert ( m_bStarted && !m_bStopped );
	assert ( m_eType==Packing_e::ROW_BLOCKS_LIST );

	if ( m_uCurBlock>=m_uBlocksCount )
	{
		m_bStopped = true;
		return false;
	}

	m_uCurBlock++;
	// reader is shared among multiple blocks - need to keep offset after last operation
	m_pReader->Seek ( m_uLastOff );
	DecodeRowsBlock();
	dRowIdBlock = Span_T<uint32_t> ( m_dRowsDecoded );

	return ( !dRowIdBlock.empty() );
}


void RowidIterator_c::DecodeRowsBlock()
{
	m_dRowsRaw.resize ( 0 );
	m_dRowsDecoded.resize ( 0 );

	m_uRowMin = m_pReader->Unpack_uint32();
	m_uRowMax = m_pReader->Unpack_uint32() + m_uRowMin;

	// should rows block be skipped or unpacked
	Interval_T<uint32_t> tBlockRange ( m_uRowMin, m_uRowMax );
	Interval_T<uint32_t> tRowidBounds ( m_tBounds.m_uMin, m_tBounds.m_uMax );
	if ( m_tBounds.m_bHasRange && !tRowidBounds.Overlaps ( tBlockRange ) )
	{
		uint32_t uLen =  m_pReader->Unpack_uint32();
		m_uLastOff = m_pReader->GetPos() + sizeof ( m_dRowsRaw[0] ) * uLen;
		return;
	}

	ReadVectorLen32 ( m_dRowsRaw, *m_pReader );
		
	m_pCodec->Decode ( m_dRowsRaw, m_dRowsDecoded );
	ComputeInverseDeltas ( m_dRowsDecoded, true );

	m_uLastOff = m_pReader->GetPos();
}

/////////////////////////////////////////////////////////////////////

BlockIteratorSize_i * CreateRowidIterator ( Packing_e eType, uint64_t uRowStart, uint32_t uSize, std::shared_ptr<columnar::FileReader_c> & pReader, std::shared_ptr<IntCodec_i> & pCodec, const RowidRange_t & tBounds, bool bCreateReader, std::string & sError )
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

	return new RowidIterator_c ( eType, uRowStart, uSize, ( tBlocksReader ? tBlocksReader : pReader ), pCodec, tBounds );
}

}