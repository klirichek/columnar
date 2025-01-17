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

#include "secondary.h"

#include "pgm.h"
#include "delta.h"
#include "codec.h"
#include "blockreader.h"

#include <unordered_map>

namespace SI
{

using namespace util;
using namespace common;


void ColumnInfo_t::Load ( util::FileReader_c & tReader )
{
	m_sName = tReader.Read_string();
	m_eType = (AttrType_e)tReader.Unpack_uint32();
	m_uCountDistinct = tReader.Unpack_uint32();
}


void ColumnInfo_t::Save ( util::FileWriter_c & tWriter ) const
{
	tWriter.Write_string ( m_sName );
	tWriter.Pack_uint32 ( (int)m_eType );
	tWriter.Pack_uint32 ( m_uCountDistinct );
}

/////////////////////////////////////////////////////////////////////

void Settings_t::Load ( FileReader_c & tReader )
{
	m_sCompressionUINT32 = tReader.Read_string();
	m_sCompressionUINT64 = tReader.Read_string();
}


void Settings_t::Save ( FileWriter_c & tWriter ) const
{
	tWriter.Write_string(m_sCompressionUINT32);
	tWriter.Write_string(m_sCompressionUINT64);
}

/////////////////////////////////////////////////////////////////////

class SecondaryIndex_c : public Index_i
{
public:
	bool		Setup ( const std::string & sFile, std::string & sError );

	bool		CreateIterators ( std::vector<BlockIterator_i *> & dIterators, const Filter_t & tFilter, const RowidRange_t * pBounds, std::string & sError ) const override;
	uint32_t	GetNumIterators ( const common::Filter_t & tFilter ) const override;
	bool		IsEnabled ( const std::string & sName ) const override;
	int64_t		GetCountDistinct ( const std::string & sName ) const override;
	bool		SaveMeta ( std::string & sError ) override;
	void		ColumnUpdated ( const char * sName ) override;

private:
	Settings_t	m_tSettings;
	int			m_iValuesPerBlock = { 1 };

	uint64_t	m_uMetaOff { 0 };
	uint64_t	m_uNextMetaOff { 0 };

	util::FileReader_c m_tReader;

	std::vector<ColumnInfo_t> m_dAttrs;
	bool m_bUpdated { false };
	std::unordered_map<std::string, int> m_hAttrs;
	std::vector<uint64_t> m_dBlockStartOff;			// per attribute vector of offsets to every block of values-rows-meta
	std::vector<uint64_t> m_dBlocksCount;			// per attribute vector of blocks count
	std::vector<std::shared_ptr<PGM_i>> m_dIdx;
	int64_t m_iBlocksBase = 0;						// start of offsets at file

	std::string m_sFileName;

	int64_t		GetValsRows ( std::vector<BlockIterator_i *> * pIterators, const Filter_t & tFilter, const RowidRange_t * pBounds ) const;
	int64_t		GetRangeRows ( std::vector<BlockIterator_i *> * pIterators, const Filter_t & tFilter, const RowidRange_t * pBounds ) const;
	int			GetColumnId ( const std::string & sName ) const;
	const ColumnInfo_t * GetAttr ( const Filter_t & tFilter, std::string & sError ) const;
};

bool SecondaryIndex_c::Setup ( const std::string & sFile, std::string & sError )
{
	if ( !m_tReader.Open ( sFile, sError ) )
		return false;

	uint32_t uVersion = m_tReader.Read_uint32();
	if ( uVersion!=LIB_VERSION )
	{
		sError = FormatStr ( "Unable to load inverted index: %s is v.%d, binary is v.%d", sFile.c_str(), uVersion, LIB_VERSION );
		return false;
	}
		
	m_sFileName = sFile;
	m_uMetaOff = m_tReader.Read_uint64();
		
	m_tReader.Seek ( m_uMetaOff );

	// raw non packed data first
	m_uNextMetaOff = m_tReader.Read_uint64();
	int iAttrsCount = m_tReader.Read_uint32();

	BitVec_c dAttrsEnabled ( iAttrsCount );
	ReadVectorData ( dAttrsEnabled.GetData(), m_tReader );

	m_tSettings.Load(m_tReader);
	m_iValuesPerBlock = m_tReader.Read_uint32();

	m_dAttrs.resize ( iAttrsCount );
	for ( int i=0; i<iAttrsCount; i++ )
	{
		ColumnInfo_t & tAttr = m_dAttrs[i];
		tAttr.Load(m_tReader);
		tAttr.m_bEnabled = dAttrsEnabled.BitGet ( i );
	}

	ReadVectorPacked ( m_dBlockStartOff, m_tReader );
	ComputeInverseDeltas ( m_dBlockStartOff, true );
	ReadVectorPacked ( m_dBlocksCount, m_tReader );

	m_dIdx.resize ( m_dAttrs.size() );
	for ( int i=0; i<m_dIdx.size(); i++ )
	{
		const ColumnInfo_t & tCol = m_dAttrs[i];
		switch ( tCol.m_eType )
		{
			case AttrType_e::UINT32:
			case AttrType_e::TIMESTAMP:
			case AttrType_e::UINT32SET:
			case AttrType_e::BOOLEAN:
				m_dIdx[i].reset ( new PGM_T<uint32_t>() );
				break;

			case AttrType_e::FLOAT:
				m_dIdx[i].reset ( new PGM_T<float>() );
				break;

			case AttrType_e::STRING:
				m_dIdx[i].reset ( new PGM_T<uint64_t>() );
				break;

			case AttrType_e::INT64:
			case AttrType_e::INT64SET:
				m_dIdx[i].reset ( new PGM_T<int64_t>() );
				break;

			default:
				sError = FormatStr ( "Unknown attribute '%s'(%d) with type %d", tCol.m_sName.c_str(), i, tCol.m_eType );
				return false;
		}

		int64_t iPgmLen = m_tReader.Unpack_uint64();
		int64_t iPgmEnd = m_tReader.GetPos() + iPgmLen;
		m_dIdx[i]->Load ( m_tReader );
		if ( m_tReader.GetPos()!=iPgmEnd )
		{
			sError = FormatStr ( "Out of bounds on loading PGM for attribute '%s'(%d), end expected %ll got %ll", tCol.m_sName.c_str(), i, iPgmEnd, m_tReader.GetPos() );
			return false;
		}

		m_hAttrs.insert ( { tCol.m_sName, i } );
	}

	m_iBlocksBase = m_tReader.GetPos();

	if ( m_tReader.IsError() )
	{
		sError = m_tReader.GetError();
		return false;
	}

	return true;
}


int SecondaryIndex_c::GetColumnId ( const std::string & sName ) const
{
	auto tIt = m_hAttrs.find ( sName );
	if ( tIt==m_hAttrs.end() )
		return -1;
		
	return tIt->second;
}


bool SecondaryIndex_c::IsEnabled ( const std::string & sName ) const
{
	int iId = GetColumnId(sName);
	if ( iId<0 )
		return false;

	auto & tCol = m_dAttrs[iId];
	return tCol.m_eType!=AttrType_e::NONE && tCol.m_bEnabled;
}


int64_t SecondaryIndex_c::GetCountDistinct ( const std::string & sName ) const
{
	int iId = GetColumnId(sName);
	if ( iId<0 )
		return -1;

	auto & tCol = m_dAttrs[iId];
	return tCol.m_bEnabled ? tCol.m_uCountDistinct : -1;
}


bool SecondaryIndex_c::SaveMeta ( std::string & sError )
{
	if ( !m_bUpdated || !m_dAttrs.size() )
		return true;

	BitVec_c dAttrEnabled ( m_dAttrs.size() );
	for ( int i=0; i<m_dAttrs.size(); i++ )
	{
		const ColumnInfo_t & tAttr = m_dAttrs[i];
		if ( tAttr.m_bEnabled )
			dAttrEnabled.BitSet ( i );
	}

	FileWriter_c tDstFile;
	if ( !tDstFile.Open ( m_sFileName, false, false, false, sError ) )
		return false;

	// seek to meta offset and skip attrbutes count
	tDstFile.Seek ( m_uMetaOff + sizeof(uint64_t) + sizeof(uint32_t) );
	WriteVector ( dAttrEnabled.GetData(), tDstFile );
	return true;
}
	
void SecondaryIndex_c::ColumnUpdated ( const char * sName )
{
	auto tIt = m_hAttrs.find ( sName );
	if ( tIt==m_hAttrs.end() )
		return;
		
	ColumnInfo_t & tCol = m_dAttrs[tIt->second];
	m_bUpdated |= tCol.m_bEnabled; // already disabled indexes should not cause flush
	tCol.m_bEnabled = false;
}


int64_t SecondaryIndex_c::GetValsRows ( std::vector<BlockIterator_i *> * pIterators,  const Filter_t & tFilter, const RowidRange_t * pBounds ) const
{
	int iCol = GetColumnId ( tFilter.m_sName );
	assert ( iCol>=0 );

	const auto & tCol = m_dAttrs[iCol];
	
 	// m_dBlockStartOff is 0based need to set to start of offsets vector
	uint64_t uBlockBaseOff = m_iBlocksBase + m_dBlockStartOff[iCol];
	uint64_t uBlocksCount = m_dBlocksCount[iCol];

	std::vector<BlockIter_t> dBlocksIt;
	int64_t iNumIterators = 0;
 	for ( const uint64_t uVal : tFilter.m_dValues )
	{
		ApproxPos_t tPos = m_dIdx[iCol]->Search(uVal);
		iNumIterators += tPos.m_iHi-tPos.m_iLo;
		if ( pIterators )
			dBlocksIt.emplace_back ( BlockIter_t ( tPos, uVal, uBlocksCount, m_iValuesPerBlock ) );
	}

	if ( !pIterators )
		return iNumIterators;

	// sort by block start offset
	std::sort ( dBlocksIt.begin(), dBlocksIt.end(), [] ( const BlockIter_t & tA, const BlockIter_t & tB ) { return tA.m_iStart<tB.m_iStart; } );

	std::unique_ptr<BlockReader_i> pBlockReader { CreateBlockReader ( m_tReader.GetFD(), tCol, m_tSettings, uBlockBaseOff, pBounds ) } ;
	for ( auto & i : dBlocksIt )
		pBlockReader->CreateBlocksIterator ( i, *pIterators );

	return iNumIterators;
}


int64_t SecondaryIndex_c::GetRangeRows ( std::vector<BlockIterator_i *> * pIterators,  const Filter_t & tFilter, const RowidRange_t * pBounds ) const
{
	int iCol = GetColumnId ( tFilter.m_sName );
	assert ( iCol>=0 );

	const auto & tCol = m_dAttrs[iCol];

	uint64_t uBlockBaseOff = m_iBlocksBase + m_dBlockStartOff[iCol];
	uint64_t uBlocksCount = m_dBlocksCount[iCol];

	const bool bFloat = ( tCol.m_eType==AttrType_e::FLOAT );

	ApproxPos_t tPos { 0, 0, ( uBlocksCount - 1 ) * m_iValuesPerBlock };
	if ( tFilter.m_bRightUnbounded )
	{
		ApproxPos_t tFound =  ( bFloat ? m_dIdx[iCol]->Search ( FloatToUint ( tFilter.m_fMinValue ) ) : m_dIdx[iCol]->Search ( tFilter.m_iMinValue ) );
		tPos.m_iPos = tFound.m_iPos;
		tPos.m_iLo = tFound.m_iLo;
	}
	else if ( tFilter.m_bLeftUnbounded )
	{
		ApproxPos_t tFound = ( bFloat ? m_dIdx[iCol]->Search ( FloatToUint ( tFilter.m_fMaxValue ) ) : m_dIdx[iCol]->Search ( tFilter.m_iMaxValue ) );
		tPos.m_iPos = tFound.m_iPos;
		tPos.m_iHi = tFound.m_iHi;
	}
	else
	{
		ApproxPos_t tFoundMin =  ( bFloat ? m_dIdx[iCol]->Search ( FloatToUint ( tFilter.m_fMinValue ) ) : m_dIdx[iCol]->Search ( tFilter.m_iMinValue ) );
		ApproxPos_t tFoundMax =  ( bFloat ? m_dIdx[iCol]->Search ( FloatToUint ( tFilter.m_fMaxValue ) ) : m_dIdx[iCol]->Search ( tFilter.m_iMaxValue ) );
		tPos.m_iLo = std::min ( tFoundMin.m_iLo, tFoundMax.m_iLo );
		tPos.m_iPos = std::min ( tFoundMin.m_iPos, tFoundMax.m_iPos );
		tPos.m_iHi = std::max ( tFoundMin.m_iHi, tFoundMax.m_iHi );
	}

	int64_t iNumIterators = tPos.m_iHi-tPos.m_iLo;
	if ( !pIterators )
		return iNumIterators;

	BlockIter_t tPosIt ( tPos, 0, uBlocksCount, m_iValuesPerBlock );

	std::unique_ptr<BlockReader_i> pReader { CreateRangeReader ( m_tReader.GetFD(), tCol, m_tSettings, uBlockBaseOff, pBounds ) };
	pReader->CreateBlocksIterator ( tPosIt, tFilter, *pIterators );
	return iNumIterators;
}


const ColumnInfo_t * SecondaryIndex_c::GetAttr ( const Filter_t & tFilter, std::string & sError ) const
{
	int iCol = GetColumnId ( tFilter.m_sName );
	if ( iCol==-1 )
	{
		sError = FormatStr ( "secondary index not found for attribute '%s'", tFilter.m_sName.c_str() );
		return nullptr;
	}

	const auto & tCol = m_dAttrs[iCol];

	if ( tCol.m_eType==AttrType_e::NONE )
	{
		sError = FormatStr( "invalid attribute %s type %d", tCol.m_sName.c_str(), tCol.m_eType );
		return nullptr;
	}

	return &tCol;
}


Filter_t FixupFilter ( const Filter_t & tFilter, const ColumnInfo_t & tCol )
{
	Filter_t tFixedFilter = tFilter;
	FixupFilterSettings ( tFixedFilter, tCol.m_eType );
	if ( tFixedFilter.m_eType==FilterType_e::STRINGS )
		tFixedFilter = StringFilterToHashFilter ( tFixedFilter, false );

	return tFixedFilter;
}


bool SecondaryIndex_c::CreateIterators ( std::vector<BlockIterator_i *> & dIterators, const Filter_t & tFilter, const RowidRange_t * pBounds, std::string & sError ) const
{
	const auto * pCol = GetAttr ( tFilter, sError );
	if ( !pCol )
		return false;

	Filter_t tFixedFilter = FixupFilter ( tFilter, *pCol );
	switch ( tFixedFilter.m_eType )
	{
	case FilterType_e::VALUES:
		GetValsRows ( &dIterators, tFixedFilter, pBounds );
		return true;

	case FilterType_e::RANGE:
	case FilterType_e::FLOATRANGE:
		GetRangeRows ( &dIterators, tFixedFilter, pBounds );
		return true;

	default:
		sError = FormatStr ( "unhandled filter type '%d'", to_underlying ( tFixedFilter.m_eType ) );
		return false;
	}
}


uint32_t SecondaryIndex_c::GetNumIterators ( const common::Filter_t & tFilter ) const
{
	std::string sError;
	const auto * pCol = GetAttr ( tFilter, sError );
	if ( !pCol )
		return 0;

	Filter_t tFixedFilter = FixupFilter ( tFilter, *pCol );
	switch ( tFixedFilter.m_eType )
	{
	case FilterType_e::VALUES:
		return GetValsRows ( nullptr, tFixedFilter, nullptr );

	case FilterType_e::RANGE:
	case FilterType_e::FLOATRANGE:
		return GetRangeRows ( nullptr, tFixedFilter, nullptr );

	default:
		return 0;
	}
}

}

SI::Index_i * CreateSecondaryIndex ( const char * sFile, std::string & sError )
{
	std::unique_ptr<SI::SecondaryIndex_c> pIdx ( new SI::SecondaryIndex_c );

	if ( !pIdx->Setup ( sFile, sError ) )
		return nullptr;

	return pIdx.release();
}

