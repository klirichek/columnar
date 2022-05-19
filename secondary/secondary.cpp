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

using namespace columnar;

class SecondaryIndex_c : public Index_i
{
public:
	bool Setup ( const std::string & sFile, std::string & sError );
	ColumnInfo_t GetColumn ( const char * sName ) const override;
	bool GetValsRows ( const FilterArgs_t & tArgs, std::string & sError, FilterContext_i * pCtx, std::vector<columnar::BlockIterator_i *> & dRes ) const override;
	bool GetRangeRows ( const FilterArgs_t & tArgs, std::string & sError, FilterContext_i * pCtx, std::vector<columnar::BlockIterator_i *> & dRes ) const override;

	bool SaveMeta ( std::string & sError ) override;
	void ColumnUpdated ( const char * sName ) override;

	FilterContext_i * CreateFilterContext () const override;

	Collation_e GetCollation() const override { return m_eCollation; }
	uint64_t GetHash ( const char * sVal ) const override;

private:
	std::string m_sCompressionUINT32;
	std::string m_sCompressionUINT64;
	int m_iValuesPerBlock = { 1 };

	uint64_t m_uMetaOff { 0 };
	uint64_t m_uNextMetaOff { 0 };

	std::vector<ColumnInfo_t> m_dAttrs;
	bool m_bUpdated { false };
	std::unordered_map<std::string, int> m_hAttrs;
	std::vector<uint64_t> m_dBlockStartOff;			// per attribute vector of offsets to every block of values-rows-meta
	std::vector<uint64_t> m_dBlocksCount;			// per attribute vector of blocks count
	std::vector<std::shared_ptr<PGM_i>> m_dIdx;
	int64_t m_iBlocksBase = 0;						// start of offsets at file

	std::string m_sFileName;

	Collation_e m_eCollation;
	StrHash_fn m_fnHash { nullptr };
};

bool SecondaryIndex_c::Setup ( const std::string & sFile, std::string & sError )
{
	columnar::FileReader_c tReader;
	if ( !tReader.Open ( sFile, sError ) )
		return false;

	uint32_t uVersion = tReader.Read_uint32();
	if ( uVersion>LIB_VERSION )
	{
		sError = FormatStr ( "Unable to load inverted index: %s is v.%d, binary is v.%d", sFile.c_str(), uVersion, LIB_VERSION );
		return false;
	}
		
	m_sFileName = sFile;
	m_uMetaOff = tReader.Read_uint64();
		
	tReader.Seek ( m_uMetaOff );

	// raw non packed data first
	m_uNextMetaOff = tReader.Read_uint64();
	int iAttrsCount = tReader.Read_uint32();

	BitVec_c dAttrsEnabled ( iAttrsCount );
	ReadVectorData ( dAttrsEnabled.GetData(), tReader );

	m_sCompressionUINT32 = tReader.Read_string();
	m_sCompressionUINT64 = tReader.Read_string();
	m_eCollation = (Collation_e)tReader.Read_uint32();
	m_iValuesPerBlock = tReader.Read_uint32();

	m_dAttrs.resize ( iAttrsCount );
	for ( int i=0; i<iAttrsCount; i++ )
	{
		ColumnInfo_t & tAttr = m_dAttrs[i];
		tAttr.m_sName = tReader.Read_string();
		tAttr.m_iSrcAttr = tReader.Unpack_uint32();
		tAttr.m_iAttr = tReader.Unpack_uint32();
		tAttr.m_eType = (AttrType_e)tReader.Unpack_uint32();
		tAttr.m_bEnabled = dAttrsEnabled.BitGet ( i );
	}

	ReadVectorPacked ( m_dBlockStartOff, tReader );
	ComputeInverseDeltas ( m_dBlockStartOff, true );
	ReadVectorPacked ( m_dBlocksCount, tReader );

	m_dIdx.resize ( m_dAttrs.size() );
	for ( int i=0; i<m_dIdx.size(); i++ )
	{
		const ColumnInfo_t & tCol = m_dAttrs[i];
		switch ( tCol.m_eType )
		{
			case AttrType_e::UINT32:
			case AttrType_e::TIMESTAMP:
			case AttrType_e::UINT32SET:
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

		int64_t iPgmLen = tReader.Unpack_uint64();
		int64_t iPgmEnd = tReader.GetPos() + iPgmLen;
		m_dIdx[i]->Load ( tReader );
		if ( tReader.GetPos()!=iPgmEnd )
		{
			sError = FormatStr ( "Out of bounds on loading PGM for attribute '%s'(%d), end expected %ll got %ll", tCol.m_sName.c_str(), i, iPgmEnd, tReader.GetPos() );
			return false;
		}

		assert ( tCol.m_iAttr==i );
		m_hAttrs.insert ( { tCol.m_sName, tCol.m_iAttr } );
	}

	m_iBlocksBase = tReader.GetPos();

	if ( tReader.IsError() )
	{
		sError = tReader.GetError();
		return false;
	}

	m_fnHash = GetHashFn ( m_eCollation );

	return true;
}


ColumnInfo_t SecondaryIndex_c::GetColumn ( const char * sName ) const
{
	auto tIt = m_hAttrs.find ( sName );
	if ( tIt==m_hAttrs.end() )
		return ColumnInfo_t();
		
	return m_dAttrs[tIt->second];
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


uint64_t SecondaryIndex_c::GetHash ( const char * sVal ) const
{
	int iLen = ( sVal ? strlen ( sVal ) : 0 );
	return m_fnHash ( (const uint8_t *)sVal, iLen );
}


class FilterContext_c : public FilterContext_i
{
public:
	FilterContext_c ( const std::string & sCodec32, const std::string & sCodec64 )
	{
		m_pCodec.reset ( CreateIntCodec ( sCodec32, sCodec64 ) );
	}

	virtual ~FilterContext_c() {}

	std::shared_ptr<IntCodec_i> m_pCodec { nullptr };
};


FilterContext_i * SecondaryIndex_c::CreateFilterContext () const
{
	return new FilterContext_c ( m_sCompressionUINT32, m_sCompressionUINT64 );
}


bool SecondaryIndex_c::GetValsRows ( const FilterArgs_t & tArgs, std::string & sError, FilterContext_i * pCtx, std::vector<columnar::BlockIterator_i *> & dRes ) const
{
	const ColumnInfo_t & tCol = tArgs.m_tCol;
	const Span_T<uint64_t> & dVals = tArgs.m_dVals;

	if ( tCol.m_eType==AttrType_e::NONE )
	{
		sError = FormatStr( "invalid attribute %s(%d) type %d", tCol.m_sName.c_str(), tCol.m_iSrcAttr, tCol.m_eType );
		return false;
	}

	if ( !pCtx )
	{
		sError = "empty filter context";
		return false;
	}

	// m_dBlockStartOff is 0based need to set to start of offsets vector
	uint64_t uBlockBaseOff = m_iBlocksBase + m_dBlockStartOff[tCol.m_iAttr];
	uint64_t uBlocksCount = m_dBlocksCount[tCol.m_iAttr];

	std::unique_ptr<BlockReader_i> pBlockReader { CreateBlockReader ( tCol.m_eType, ( (FilterContext_c *)pCtx )->m_pCodec, uBlockBaseOff, tArgs.m_tBounds ) } ;
	if ( !pBlockReader->Open ( m_sFileName, sError ) )
		return false;

	std::vector<BlockIter_t> dBlocksIt;
 	for ( const uint64_t uVal : dVals )
		dBlocksIt.emplace_back ( BlockIter_t ( m_dIdx[tCol.m_iAttr]->Search ( uVal ), uVal, uBlocksCount, m_iValuesPerBlock ) );

	// sort by block start offset
	std::sort ( dBlocksIt.begin(), dBlocksIt.end(), [] ( const BlockIter_t & tA, const BlockIter_t & tB ) { return tA.m_iStart<tB.m_iStart; } );

	for ( int i=0; i<dBlocksIt.size(); i++ )
		pBlockReader->CreateBlocksIterator ( dBlocksIt[i], dRes );

	sError = pBlockReader->GetWarning();

	return true;
}


bool SecondaryIndex_c::GetRangeRows ( const FilterArgs_t & tArgs, std::string & sError, FilterContext_i * pCtx, std::vector<BlockIterator_i *> & dRes ) const
{
	const ColumnInfo_t & tCol = tArgs.m_tCol;
	const FilterRange_t & tVal = tArgs.m_tRange;

	if ( tCol.m_eType==AttrType_e::NONE )
	{
		sError = FormatStr( "invalid attribute %s(%d) type %d", tCol.m_sName.c_str(), tCol.m_iSrcAttr, tCol.m_eType );
		return false;
	}

	if ( !pCtx )
	{
		sError = "empty filter context";
		return false;
	}

	uint64_t uBlockBaseOff = m_iBlocksBase + m_dBlockStartOff[tCol.m_iAttr];
	uint64_t uBlocksCount = m_dBlocksCount[tCol.m_iAttr];

	const bool bFloat = ( tCol.m_eType==AttrType_e::FLOAT );

	ApproxPos_t tPos { 0, 0, ( uBlocksCount - 1 ) * m_iValuesPerBlock };
	if ( tVal.m_bOpenRight )
	{
		ApproxPos_t tFound =  ( bFloat ? m_dIdx[tCol.m_iAttr]->Search ( FloatToUint ( tVal.m_fMin ) ) : m_dIdx[tCol.m_iAttr]->Search ( tVal.m_iMin ) );
		tPos.m_iPos = tFound.m_iPos;
		tPos.m_iLo = tFound.m_iLo;
	} else if ( tVal.m_bOpenLeft )
	{
		ApproxPos_t tFound = ( bFloat ? m_dIdx[tCol.m_iAttr]->Search ( FloatToUint ( tVal.m_fMax ) ) : m_dIdx[tCol.m_iAttr]->Search ( tVal.m_iMax ) );
		tPos.m_iPos = tFound.m_iPos;
		tPos.m_iHi = tFound.m_iHi;
	} else
	{
		ApproxPos_t tFoundMin =  ( bFloat ? m_dIdx[tCol.m_iAttr]->Search ( FloatToUint ( tVal.m_fMin ) ) : m_dIdx[tCol.m_iAttr]->Search ( tVal.m_iMin ) );
		ApproxPos_t tFoundMax =  ( bFloat ? m_dIdx[tCol.m_iAttr]->Search ( FloatToUint ( tVal.m_fMax ) ) : m_dIdx[tCol.m_iAttr]->Search ( tVal.m_iMax ) );
		tPos.m_iLo = std::min ( tFoundMin.m_iLo, tFoundMax.m_iLo );
		tPos.m_iPos = std::min ( tFoundMin.m_iPos, tFoundMax.m_iPos );
		tPos.m_iHi = std::max ( tFoundMin.m_iHi, tFoundMax.m_iHi );
	}

	BlockIter_t tPosIt ( tPos, 0, uBlocksCount, m_iValuesPerBlock );

	std::unique_ptr<BlockReader_i> pReader { CreateRangeReader ( tCol.m_eType, ( (FilterContext_c *)pCtx)->m_pCodec, uBlockBaseOff, tArgs.m_tBounds ) } ;
	if ( !pReader->Open ( m_sFileName, sError ) )
		return false;

	pReader->CreateBlocksIterator ( tPosIt, tVal, dRes );
	sError = pReader->GetWarning();

	return true;
}

}

SI::Index_i * CreateSecondaryIndex ( const char * sFile, std::string & sError )
{
	std::unique_ptr<SI::SecondaryIndex_c> pIdx ( new SI::SecondaryIndex_c );

	if ( !pIdx->Setup ( sFile, sError ) )
		return nullptr;

	return pIdx.release();
}

