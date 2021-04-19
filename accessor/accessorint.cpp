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

#include "accessorint.h"

#include "accessortraits.h"
#include "builderint.h"
#include "interval.h"
#include "reader.h"

#include <algorithm>
#include <tuple>

namespace columnar
{

//////////////////////////////////////////////////////////////////////////

template <typename T>
class StoredBlock_Int_Const_T
{
public:
	FORCE_INLINE void	ReadHeader ( FileReader_c & tReader );
	FORCE_INLINE T		GetValue() const { return m_tValue; }

private:
	T					m_tValue = 0;
};

template <typename T>
void StoredBlock_Int_Const_T<T>::ReadHeader ( FileReader_c & tReader )
{
	m_tValue = (T)tReader.Unpack_uint64();
}

template <typename T>
class StoredBlock_Int_Table_T
{
public:
							StoredBlock_Int_Table_T ( int iSubblockSize );

	FORCE_INLINE void		ReadHeader ( FileReader_c & tReader );
	FORCE_INLINE void		ReadSubblock ( int iSubblockId, int iNumValues, FileReader_c & tReader );
	FORCE_INLINE T			GetValue ( int iIdInSubblock );
	FORCE_INLINE const Span_T<uint32_t> & GetValueIndexes() const { return m_tValuesRead; }
	FORCE_INLINE int		GetIndexInTable ( T tValue ) const;
	FORCE_INLINE T			GetValueFromTable ( uint8_t uIndex ) const { return m_dTableValues[uIndex]; }
	FORCE_INLINE int		GetTableSize() const { return (int)m_dTableValues.size(); }

private:
	std::vector<T>			m_dTableValues;
	std::vector<uint32_t>	m_dValueIndexes;
	std::vector<uint32_t>	m_dEncoded;
	int						m_iBits = 0;
	int64_t					m_iValuesOffset = 0;
	int						m_iSubblockId = -1;
	Span_T<uint32_t>		m_tValuesRead;
};

template <typename T>
StoredBlock_Int_Table_T<T>::StoredBlock_Int_Table_T ( int iSubblockSize )
{
	assert ( iSubblockSize==128 );
	m_dValueIndexes.resize(iSubblockSize);
}

template <typename T>
void StoredBlock_Int_Table_T<T>::ReadHeader ( FileReader_c & tReader )
{
	m_dTableValues.resize ( tReader.Read_uint8() );
	T tCurValue = 0;
	for ( auto & i : m_dTableValues )
	{
		tCurValue += (T)tReader.Unpack_uint64();
		i = tCurValue;
	}

	m_iBits = CalcNumBits ( m_dTableValues.size() );
	m_dEncoded.resize ( ( m_dValueIndexes.size() >> 5 ) * m_iBits );

	m_iValuesOffset = tReader.GetPos();
	m_iSubblockId = -1;
}

template <typename T>
void StoredBlock_Int_Table_T<T>::ReadSubblock ( int iSubblockId, int iNumValues, FileReader_c & tReader )
{
	if ( m_iSubblockId==iSubblockId )
		return;

	m_iSubblockId = iSubblockId;

	size_t uPackedSize = m_dEncoded.size()*sizeof ( m_dEncoded[0] );
	tReader.Seek ( m_iValuesOffset + uPackedSize*iSubblockId );
	tReader.Read ( (uint8_t*)m_dEncoded.data(), uPackedSize );
	BitUnpack128 ( m_dEncoded, m_dValueIndexes, m_iBits );

	m_tValuesRead = { m_dValueIndexes.data(), (size_t)iNumValues };
}

template <typename T>
T StoredBlock_Int_Table_T<T>::GetValue ( int iIdInSubblock )
{
	return m_dTableValues [ m_dValueIndexes[iIdInSubblock] ];
}

template <typename T>
int StoredBlock_Int_Table_T<T>::GetIndexInTable ( T tValue ) const
{
	const T * pValue = binary_search ( m_dTableValues, tValue );
	if ( !pValue )
		return -1;

	return int(pValue-m_dTableValues.data());
}

//////////////////////////////////////////////////////////////////////////

template <typename T>
class StoredBlock_Int_PFOR_T
{
public:
							StoredBlock_Int_PFOR_T ( const std::string & sCodec32, const std::string & sCodec64 );

	FORCE_INLINE void		ReadHeader ( FileReader_c & tReader );
	FORCE_INLINE void		ReadSubblock_Delta ( int iSubblockId, FileReader_c & tReader );
	FORCE_INLINE void		ReadSubblock_Generic ( int iSubblockId, FileReader_c & tReader );
	FORCE_INLINE T			GetValue ( int iIdInSubblock ) const;
	FORCE_INLINE const Span_T<T> & GetAllValues() const { return m_dSubblockValues; }

private:
	std::unique_ptr<IntCodec_i>	m_pCodec;
	SpanResizeable_T<uint32_t>	m_dSubblockCumulativeSizes;
	SpanResizeable_T<uint32_t>	m_dTmp;
	int64_t						m_tValuesOffset = 0;

	int							m_iSubblockId = -1;
	SpanResizeable_T<T>			m_dSubblockValues;

	template <typename DECOMPRESS>
	FORCE_INLINE void		ReadSubblock ( int iSubblockId, FileReader_c & tReader, DECOMPRESS && fnDecompress );
};

template <typename T>
StoredBlock_Int_PFOR_T<T>::StoredBlock_Int_PFOR_T ( const std::string & sCodec32, const std::string & sCodec64 )
	: m_pCodec ( CreateIntCodec ( sCodec32, sCodec64 ) )
{}

template <typename T>
void StoredBlock_Int_PFOR_T<T>::ReadHeader ( FileReader_c & tReader )
{
	uint32_t uSubblockSize = tReader.Unpack_uint32();
	DecodeValues_Delta_PFOR ( m_dSubblockCumulativeSizes, tReader, *m_pCodec, m_dTmp, uSubblockSize, false );

	m_tValuesOffset = tReader.GetPos();
	m_iSubblockId = -1;
}

template <typename T>
void StoredBlock_Int_PFOR_T<T>::ReadSubblock_Delta ( int iSubblockId, FileReader_c & tReader )
{
	ReadSubblock ( iSubblockId, tReader, [this] ( SpanResizeable_T<T> & dValues, FileReader_c & tReader, uint32_t uTotalSize )
		{ DecodeValues_Delta_PFOR ( dValues, tReader, *m_pCodec, m_dTmp, uTotalSize, true ); }
	);
}

template <typename T>
void StoredBlock_Int_PFOR_T<T>::ReadSubblock_Generic ( int iSubblockId, FileReader_c & tReader )
{
	ReadSubblock ( iSubblockId, tReader, [this] ( SpanResizeable_T<T> & dValues, FileReader_c & tReader, uint32_t uTotalSize )
		{ DecodeValues_PFOR ( dValues, tReader, *m_pCodec, m_dTmp, uTotalSize); }
	);
}

template <typename T>
template <typename DECOMPRESS>
void StoredBlock_Int_PFOR_T<T>::ReadSubblock ( int iSubblockId, FileReader_c & tReader, DECOMPRESS && fnDecompress )
{
	if ( m_iSubblockId==iSubblockId )
		return;

	m_iSubblockId = iSubblockId;

	uint32_t uSize = m_dSubblockCumulativeSizes[iSubblockId];
	uint32_t uOffset = 0;
	if ( iSubblockId>0 )
	{
		uOffset = m_dSubblockCumulativeSizes[iSubblockId-1];
		uSize -= uOffset;
	}

	tReader.Seek ( m_tValuesOffset+uOffset );
	fnDecompress ( m_dSubblockValues, tReader, uSize );
}

template <typename T>
T StoredBlock_Int_PFOR_T<T>::GetValue ( int iIdInSubblock ) const
{
	return m_dSubblockValues[iIdInSubblock];
}

//////////////////////////////////////////////////////////////////////////

template<typename T>
class Accessor_INT_T : public StoredBlockTraits_t
{
public:
					Accessor_INT_T ( const AttributeHeader_i & tHeader, FileReader_c * pReader );

protected:
	const AttributeHeader_i &		m_tHeader;
	std::unique_ptr<FileReader_c>	m_pReader;

	StoredBlock_Int_Const_T<T>		m_tBlockConst;
	StoredBlock_Int_Table_T<T>		m_tBlockTable;
	StoredBlock_Int_PFOR_T<T>		m_tBlockPFOR;

	int64_t (Accessor_INT_T<T>::*m_fnReadValue)() = nullptr;

	IntPacking_e	m_ePacking = IntPacking_e::CONST;

	FORCE_INLINE void SetCurBlock ( uint32_t uBlockId );

	int64_t			ReadValue_Const();
	int64_t			ReadValue_Table();
	int64_t			ReadValue_DeltaPFOR();
	int64_t			ReadValue_GenericPFOR();
};

template<typename T>
Accessor_INT_T<T>::Accessor_INT_T ( const AttributeHeader_i & tHeader, FileReader_c * pReader )
	: StoredBlockTraits_t ( tHeader.GetSettings().m_iSubblockSize )
	, m_tHeader ( tHeader )
	, m_pReader ( pReader )
	, m_tBlockTable ( tHeader.GetSettings().m_iSubblockSize )
	, m_tBlockPFOR ( tHeader.GetSettings().m_sCompressionUINT32, tHeader.GetSettings().m_sCompressionUINT64 )
{
	assert(pReader);
}

template<typename T>
void Accessor_INT_T<T>::SetCurBlock ( uint32_t uBlockId )
{
	m_pReader->Seek ( m_tHeader.GetBlockOffset(uBlockId) );
	m_ePacking = (IntPacking_e)m_pReader->Unpack_uint32();

	m_tRequestedRowID = INVALID_ROW_ID;

	switch ( m_ePacking )
	{
	case IntPacking_e::CONST:
		m_fnReadValue = &Accessor_INT_T<T>::ReadValue_Const;
		m_tBlockConst.ReadHeader ( *m_pReader );
		break;

	case IntPacking_e::TABLE:
		m_fnReadValue = &Accessor_INT_T<T>::ReadValue_Table;
		m_tBlockTable.ReadHeader ( *m_pReader );
		break;

	case IntPacking_e::DELTA_PFOR:
		m_fnReadValue = &Accessor_INT_T<T>::ReadValue_DeltaPFOR;
		m_tBlockPFOR.ReadHeader ( *m_pReader );
		break;

	case IntPacking_e::GENERIC_PFOR:
		m_fnReadValue = &Accessor_INT_T<T>::ReadValue_GenericPFOR;
		m_tBlockPFOR.ReadHeader ( *m_pReader );
		break;

	default:
		assert ( 0 && "Packing not implemented yet" );
	}

	SetBlockId ( uBlockId, m_tHeader.GetNumDocs(uBlockId) );
}

template<typename T>
int64_t Accessor_INT_T<T>::ReadValue_Const()
{
	return m_tBlockConst.GetValue();
}

template<typename T>
int64_t Accessor_INT_T<T>::ReadValue_Table()
{
	uint32_t uIdInBlock = m_tRequestedRowID - m_tStartBlockRowId;
	int iSubblockId = StoredBlockTraits_t::GetSubblockId(uIdInBlock);
	m_tBlockTable.ReadSubblock ( iSubblockId, StoredBlockTraits_t::GetNumSubblockValues(iSubblockId), *m_pReader );
	return m_tBlockTable.GetValue ( GetValueIdInSubblock(uIdInBlock) );
}

template<typename T>
int64_t Accessor_INT_T<T>::ReadValue_DeltaPFOR()
{
	uint32_t uIdInBlock = m_tRequestedRowID - m_tStartBlockRowId;
	m_tBlockPFOR.ReadSubblock_Delta ( StoredBlockTraits_t::GetSubblockId(uIdInBlock), *m_pReader );
	return m_tBlockPFOR.GetValue ( GetValueIdInSubblock(uIdInBlock) );
}

template<typename T>
int64_t Accessor_INT_T<T>::ReadValue_GenericPFOR()
{
	uint32_t uIdInBlock = m_tRequestedRowID - m_tStartBlockRowId;
	m_tBlockPFOR.ReadSubblock_Generic ( GetSubblockId(uIdInBlock), *m_pReader );
	return m_tBlockPFOR.GetValue ( GetValueIdInSubblock(uIdInBlock) );
}

//////////////////////////////////////////////////////////////////////////

template<typename T>
class Iterator_INT_T : public Iterator_i, public Accessor_INT_T<T>
{
	using BASE = Accessor_INT_T<T>;
	using BASE::Accessor_INT_T;

public:
	uint32_t	AdvanceTo ( uint32_t tRowID ) final;

	int64_t		Get() final;

	int			Get ( const uint8_t * & pData, bool bPack ) final;
	int			GetLength() const final;

	uint64_t	GetStringHash() final { return 0; }
	bool		HaveStringHashes() const final { return false; }
};


template<typename T>
uint32_t Iterator_INT_T<T>::AdvanceTo ( uint32_t tRowID )
{
	uint32_t uBlockId = RowId2BlockId(tRowID);
	if ( uBlockId!=BASE::m_uBlockId )
		BASE::SetCurBlock(uBlockId);

	BASE::m_tRequestedRowID = tRowID;

	return tRowID;
}

template<typename T>
int64_t Iterator_INT_T<T>::Get()
{
	assert ( BASE::m_fnReadValue );
	return (*this.*BASE::m_fnReadValue)();
}

template<typename T>
int Iterator_INT_T<T>::Get ( const uint8_t * & pData, bool bPack )
{
	assert ( 0 && "INTERNAL ERROR: requesting blob from int iterator" );
	return 0;
}

template<typename T>
int Iterator_INT_T<T>::GetLength() const
{
	assert ( 0 && "INTERNAL ERROR: requesting blob length from int iterator" );
	return 0;
}

//////////////////////////////////////////////////////////////////////////

class AnalyzerBlock_c : public Filter_t
{
public:
				AnalyzerBlock_c ( uint32_t & tRowID );

	void		Setup ( const Filter_t & tSettings );

protected:
	uint32_t &	m_tRowID;
	int64_t		m_tValue;
};


AnalyzerBlock_c::AnalyzerBlock_c ( uint32_t & tRowID )
	: m_tRowID ( tRowID )
{}


void AnalyzerBlock_c::Setup ( const Filter_t & tSettings )
{
	*(Filter_t*)this = tSettings;

	if ( m_dValues.size()==1 )
		m_tValue = m_dValues[0];
}

//////////////////////////////////////////////////////////////////////////

class AnalyzerBlock_Int_Const_c : public AnalyzerBlock_c
{
	using AnalyzerBlock_c::AnalyzerBlock_c;

public:
	template<typename T, typename RANGE_EVAL>
	FORCE_INLINE bool	SetupNextBlock ( const StoredBlock_Int_Const_T<T> & tBlock, bool bEq );
	FORCE_INLINE int	ProcessSubblock ( uint32_t * & pRowID, int iNumValues );
};


template<typename T, typename RANGE_EVAL>
bool AnalyzerBlock_Int_Const_c::SetupNextBlock ( const StoredBlock_Int_Const_T<T> & tBlock, bool bEq )
{
	int64_t tValue = tBlock.GetValue();

	switch ( m_eType )
	{
	case FilterType_e::VALUES:
	{
		bool bAnyEqual = false;
		for ( auto i : m_dValues )
			if ( i==tValue )
			{
				bAnyEqual = true;
				break;
			}
		
		return bAnyEqual ^ ( !bEq );
	}
	break;

	case FilterType_e::RANGE:
		if ( RANGE_EVAL::Eval ( tValue, m_iMinValue, m_iMaxValue ) )
			return true;

		break;

	case FilterType_e::FLOATRANGE:
		if ( RANGE_EVAL::Eval ( UintToFloat ( (uint32_t) tValue ), m_fMinValue, m_fMaxValue ) )
			return true;

		break;

	default:
		break;
	}

	return false;
}


int AnalyzerBlock_Int_Const_c::ProcessSubblock ( uint32_t * & pRowID, int iNumValues )
{
	uint32_t tRowID = m_tRowID;

	// FIXME! use SSE here
	for ( int i = 0; i < iNumValues; i++ )
		*pRowID++ = tRowID++;

	m_tRowID = tRowID;
	return iNumValues;
}

//////////////////////////////////////////////////////////////////////////

class AnalyzerBlock_Int_Table_c : public AnalyzerBlock_c
{
	using AnalyzerBlock_c::AnalyzerBlock_c;

public:
	template <bool EQ> FORCE_INLINE int	ProcessSubblock_SingleValue ( uint32_t * & pRowID, const Span_T<uint32_t> & dValueIndexes );
	template <bool EQ> FORCE_INLINE int	ProcessSubblock_ValuesLinear ( uint32_t * & pRowID, const Span_T<uint32_t> & dValueIndexes );
	template <bool EQ> FORCE_INLINE int	ProcessSubblock_ValuesBinary ( uint32_t * & pRowID, const Span_T<uint32_t> & dValueIndexes );
	FORCE_INLINE int	ProcessSubblock_Range ( uint32_t * & pRowID, const Span_T<uint32_t> & dValueIndexes );

	template <typename T, typename RANGE_EVAL>
	FORCE_INLINE bool	SetupNextBlock ( const StoredBlock_Int_Table_T<T> & tBlock, bool bEq );

private:
	int							m_iTableValueId = -1;
	std::vector<uint8_t>		m_dTableValues;
	std::array<bool,UCHAR_MAX>	m_dRangeMap;
};

template <bool EQ>
int AnalyzerBlock_Int_Table_c::ProcessSubblock_SingleValue ( uint32_t * & pRowID, const Span_T<uint32_t> & dValueIndexes )
{
	uint32_t tRowID = m_tRowID;

	// FIXME! use SSE here
	if ( !EQ && m_iTableValueId==-1 ) // accept all values
	{
		for ( auto i : dValueIndexes )
			*pRowID++ = tRowID++;

		return (int)dValueIndexes.size();
	}

	assert ( m_iTableValueId>=0 );

	for ( auto i : dValueIndexes )
	{
		if ( ( i==(uint32_t)m_iTableValueId ) ^ (!EQ) )
			*pRowID++ = tRowID;

		tRowID++;
	}

	m_tRowID = tRowID;
	return (int)dValueIndexes.size();
}

template <bool EQ>
int AnalyzerBlock_Int_Table_c::ProcessSubblock_ValuesLinear ( uint32_t * & pRowID, const Span_T<uint32_t> & dValueIndexes )
{
	uint32_t tRowID = m_tRowID;

	// FIXME! use SSE here
	if ( !EQ && m_dTableValues.empty() ) // accept all values
	{
		for ( auto i : dValueIndexes )
			*pRowID++ = tRowID++;

		return (int)dValueIndexes.size();
	}

	for ( auto i : dValueIndexes )
	{
		if ( EQ )
		{
			for ( auto j : m_dTableValues )
				if ( i==(uint32_t)j )
				{
					*pRowID++ = tRowID;
					break;
				}
		}
		else
		{
			bool bAnyEqual = false;
			for ( auto j : m_dTableValues )
				if ( i==(uint32_t)j )
				{
					bAnyEqual = true;
					break;
				}

			if ( !bAnyEqual )
				*pRowID++ = tRowID;
		}

		tRowID++;
	}

	m_tRowID = tRowID;
	return (int)dValueIndexes.size();
}

template <bool EQ>
int AnalyzerBlock_Int_Table_c::ProcessSubblock_ValuesBinary ( uint32_t * & pRowID, const Span_T<uint32_t> & dValueIndexes )
{
	uint32_t tRowID = m_tRowID;

	// FIXME! use SSE here
	if ( !EQ && m_dTableValues.empty() ) // accept all values
	{
		for ( auto i : dValueIndexes )
			*pRowID++ = tRowID++;

		return (int)dValueIndexes.size();
	}

	for ( auto i : dValueIndexes )
	{
		bool bAnyEqual = std::binary_search ( m_dTableValues.cbegin(), m_dTableValues.cend(), (uint8_t)i );
		if ( bAnyEqual ^ (!EQ) )
			*pRowID++ = tRowID;

		tRowID++;
	}

	m_tRowID = tRowID;
	return (int)dValueIndexes.size();
}

int AnalyzerBlock_Int_Table_c::ProcessSubblock_Range ( uint32_t * & pRowID, const Span_T<uint32_t> & dValueIndexes )
{
	uint32_t tRowID = m_tRowID;

	for ( auto i : dValueIndexes )
	{
		if ( m_dRangeMap[i] )
			*pRowID++ = tRowID;

		tRowID++;
	}

	m_tRowID = tRowID;
	return (int)dValueIndexes.size();
}

template<typename T, typename RANGE_EVAL>
bool AnalyzerBlock_Int_Table_c::SetupNextBlock ( const StoredBlock_Int_Table_T<T> & tBlock, bool bEq )
{
	switch ( m_eType )
	{
	case FilterType_e::VALUES:
		if ( m_dValues.size()==1 )
		{
			m_iTableValueId = tBlock.GetIndexInTable ( (T)m_tValue );
			if ( bEq && m_iTableValueId==-1 )
				return false;
		}
		else
		{
			m_dTableValues.resize(0);
			for ( auto i : m_dValues )
			{
				int iValue = tBlock.GetIndexInTable ( (T)i );
				if ( iValue!=-1 )
					m_dTableValues.push_back ( (uint8_t)iValue );
			}

			if ( bEq && m_dTableValues.empty() )
				return false;

			std::sort ( m_dTableValues.begin(), m_dTableValues.end() );
		}
		break;

	case FilterType_e::RANGE:
		{
			bool bAnyInRange = false;
			for ( int i = 0; i < tBlock.GetTableSize(); i++ )
			{
				m_dRangeMap[i] = RANGE_EVAL::Eval ( tBlock.GetValueFromTable(i), (T)m_iMinValue, (T)m_iMaxValue );
				bAnyInRange |= m_dRangeMap[i];
			}

			if ( !bAnyInRange )
				return false;
		}
		break;

	case FilterType_e::FLOATRANGE:
		{
			bool bAnyInRange = false;
			for ( int i = 0; i < tBlock.GetTableSize(); i++ )
			{
				m_dRangeMap[i] = RANGE_EVAL::Eval ( UintToFloat ( (uint32_t)tBlock.GetValueFromTable(i) ), m_fMinValue, m_fMaxValue );
				bAnyInRange |= m_dRangeMap[i];
			}

			if ( !bAnyInRange )
				return false;
		}
		break;

	default:
		break;
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////

template<typename VALUES, typename ACCESSOR_VALUES>
class AnalyzerBlock_Int_Values_T : public AnalyzerBlock_c
{
	using AnalyzerBlock_c::AnalyzerBlock_c;

public:
	template <bool EQ> FORCE_INLINE int	ProcessSubblock_SingleValue ( uint32_t * & pRowID, const Span_T<ACCESSOR_VALUES> & dValues );
	template <bool EQ> FORCE_INLINE int	ProcessSubblock_ValuesLinear ( uint32_t * & pRowID, const Span_T<ACCESSOR_VALUES> & dValues );
	template <bool EQ> FORCE_INLINE int	ProcessSubblock_ValuesBinary ( uint32_t * & pRowID, const Span_T<ACCESSOR_VALUES> & dValues );

	template<typename RANGE_EVAL> FORCE_INLINE int	ProcessSubblock_Range ( uint32_t * & pRowID, const Span_T<ACCESSOR_VALUES> & dValues );
	template<typename RANGE_EVAL> FORCE_INLINE int	ProcessSubblock_FloatRange ( uint32_t * & pRowID, const Span_T<ACCESSOR_VALUES> & dValues );
};

template<typename VALUES, typename ACCESSOR_VALUES>
template <bool EQ>
int AnalyzerBlock_Int_Values_T<VALUES,ACCESSOR_VALUES>::ProcessSubblock_SingleValue ( uint32_t * & pRowID, const Span_T<ACCESSOR_VALUES> & dValues )
{
	uint32_t tRowID = m_tRowID;

	// FIXME! use SSE here
	for ( auto & i : dValues )
	{
		if ( ( i==(ACCESSOR_VALUES)m_tValue ) ^ (!EQ) )
			*pRowID++ = tRowID;

		tRowID++;
	}

	m_tRowID = tRowID;
	return (int)dValues.size();
}

template<typename VALUES, typename ACCESSOR_VALUES>
template <bool EQ>
int AnalyzerBlock_Int_Values_T<VALUES,ACCESSOR_VALUES>::ProcessSubblock_ValuesLinear ( uint32_t * & pRowID, const Span_T<ACCESSOR_VALUES> & dValues )
{
	uint32_t tRowID = m_tRowID;

	// FIXME! use SSE here
	for ( auto i : dValues )
	{
		for ( auto j : m_dValues )
			if ( ( i==(ACCESSOR_VALUES)j ) ^ (!EQ) )
			{
				*pRowID++ = tRowID;
				break;
			}

		tRowID++;
	}

	m_tRowID = tRowID;
	return (int)dValues.size();
}

template<typename VALUES, typename ACCESSOR_VALUES>
template <bool EQ>
int AnalyzerBlock_Int_Values_T<VALUES,ACCESSOR_VALUES>::ProcessSubblock_ValuesBinary ( uint32_t * & pRowID, const Span_T<ACCESSOR_VALUES> & dValues )
{
	uint32_t tRowID = m_tRowID;

	for ( auto i : dValues )
	{
		if ( std::binary_search ( m_dValues.cbegin(), m_dValues.cend(), (VALUES)i ) ^ (!EQ) )
			*pRowID++ = tRowID;

		tRowID++;
	}

	m_tRowID = tRowID;
	return (int)dValues.size();
}

template<typename VALUES, typename ACCESSOR_VALUES>
template<typename RANGE_EVAL>
int AnalyzerBlock_Int_Values_T<VALUES,ACCESSOR_VALUES>::ProcessSubblock_Range ( uint32_t * & pRowID, const Span_T<ACCESSOR_VALUES> & dValues )
{
	uint32_t tRowID = m_tRowID;

	for ( auto i : dValues )
	{
		if ( RANGE_EVAL::Eval ( (VALUES)i, (VALUES)m_iMinValue, (VALUES)m_iMaxValue ) )
			*pRowID++ = tRowID;

		tRowID++;
	}

	m_tRowID = tRowID;
	return (int)dValues.size();
}

template<>
template<typename RANGE_EVAL>
int AnalyzerBlock_Int_Values_T<float,uint32_t>::ProcessSubblock_Range ( uint32_t * & pRowID, const Span_T<uint32_t> & dValues )
{
	uint32_t tRowID = m_tRowID;

	for ( auto i : dValues )
	{
		if ( RANGE_EVAL::Eval ( UintToFloat(i), m_fMinValue, m_fMaxValue ) )
			*pRowID++ = tRowID;

		tRowID++;
	}

	m_tRowID = tRowID;
	return (int)dValues.size();
}


// a mega-class of all integer analyzers
// splitting it into a class hierarchy would yield cleaner code
// but virtual function calls give very noticeable performance penalties
template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
class Analyzer_INT_T : public Analyzer_T<true>, public Accessor_INT_T<ACCESSOR_VALUES>
{
	using ANALYZER = Analyzer_T<true>;
	using ACCESSOR = Accessor_INT_T<ACCESSOR_VALUES>;

public:
					Analyzer_INT_T ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings );

	bool			GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock ) final;

private:
	AnalyzerBlock_Int_Const_c	m_tBlockConst;
	AnalyzerBlock_Int_Table_c	m_tBlockTable;
	AnalyzerBlock_Int_Values_T<VALUES, ACCESSOR_VALUES> m_tBlockValues;

	Filter_t 			m_tSettings;

	typedef int (Analyzer_INT_T::*ProcessSubblock_fn)( uint32_t * & pRowID, int iSubblockIdInBlock );
	std::array<ProcessSubblock_fn,to_underlying(IntPacking_e::TOTAL)> m_dProcessingFuncs;
	ProcessSubblock_fn	m_fnProcessSubblock = nullptr;

	void				SetupPackingFuncs_SingleValue();
	void				SetupPackingFuncs_ValuesLinear();
	void				SetupPackingFuncs_ValuesBinary();
	void				SetupPackingFuncs_Range();

	void				SetupPackingFuncs();
	void				FixupFilterSettings();

	int					ProcessSubblockConst ( uint32_t * & pRowID, int iSubblockIdInBlock );

	template <bool EQ>				int	ProcessSubblockGenericPFOR_SingleValue ( uint32_t * & pRowID, int iSubblockIdInBlock );
	template <bool EQ, bool LINEAR>	int	ProcessSubblockGenericPFOR_Values ( uint32_t * & pRowID, int iSubblockIdInBlock );

	int	ProcessSubblockGenericPFOR_Range ( uint32_t * & pRowID, int iSubblockIdInBlock );

	template <bool EQ>				int	ProcessSubblockDeltaPFOR_SingleValue ( uint32_t * & pRowID, int iSubblockIdInBlock );

	template <bool EQ, bool LINEAR>	int	ProcessSubblockDeltaPFOR_Values ( uint32_t * & pRowID, int iSubblockIdInBlock );

	int	ProcessSubblockDeltaPFOR_Range ( uint32_t * & pRowID, int iSubblockIdInBlock );

	template <bool EQ>				int	ProcessSubblockTable_SingleValue ( uint32_t * & pRowID, int iSubblockIdInBlock );
	template <bool EQ, bool LINEAR>	int	ProcessSubblockTable_Values ( uint32_t * & pRowID, int iSubblockIdInBlock );

	int	ProcessSubblockTable_Range ( uint32_t * & pRowID, int iSubblockIdInBlock );

	bool				MoveToBlock ( int iNextBlock ) final;
};

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::Analyzer_INT_T ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings )
	: ANALYZER ( tHeader.GetSettings().m_iSubblockSize )
	, ACCESSOR ( tHeader, pReader )
	, m_tBlockConst ( m_tRowID )
	, m_tBlockTable ( m_tRowID )
	, m_tBlockValues (m_tRowID )
	, m_tSettings ( tSettings )
{
	FixupFilterSettings();

	assert ( !tSettings.m_bExclude || ( tSettings.m_bExclude && tSettings.m_eType==FilterType_e::VALUES ) );

	m_tBlockConst.Setup(m_tSettings);
	m_tBlockTable.Setup(m_tSettings);
	m_tBlockValues.Setup(m_tSettings);

	SetupPackingFuncs();
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
void Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::SetupPackingFuncs_SingleValue()
{
	auto & dFuncs = m_dProcessingFuncs;
	if ( m_tSettings.m_bExclude )
	{
		dFuncs [ to_underlying ( IntPacking_e::TABLE ) ]		= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockTable_SingleValue<false>;
		dFuncs [ to_underlying ( IntPacking_e::DELTA_PFOR ) ]	= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockDeltaPFOR_SingleValue<false>;
		dFuncs [ to_underlying ( IntPacking_e::GENERIC_PFOR )]	= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockGenericPFOR_SingleValue<false>;
	}
	else
	{
		dFuncs [ to_underlying ( IntPacking_e::TABLE ) ]		= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockTable_SingleValue<true>;
		dFuncs [ to_underlying ( IntPacking_e::DELTA_PFOR ) ]	= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockDeltaPFOR_SingleValue<true>;
		dFuncs [ to_underlying ( IntPacking_e::GENERIC_PFOR )]	= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockGenericPFOR_SingleValue<true>;
	}
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
void Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::SetupPackingFuncs_ValuesLinear()
{
	auto & dFuncs = m_dProcessingFuncs;
	if ( m_tSettings.m_bExclude )
	{
		dFuncs [ to_underlying ( IntPacking_e::TABLE ) ]		= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockTable_Values<false,true>;
		dFuncs [ to_underlying ( IntPacking_e::DELTA_PFOR ) ]	= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockDeltaPFOR_Values<false,true>;
		dFuncs [ to_underlying ( IntPacking_e::GENERIC_PFOR ) ]	= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockGenericPFOR_Values<false,true>;
	}
	else
	{
		dFuncs [ to_underlying ( IntPacking_e::TABLE ) ]		= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockTable_Values<true,true>;
		dFuncs [ to_underlying ( IntPacking_e::DELTA_PFOR ) ]	= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockDeltaPFOR_Values<true,true>;
		dFuncs [ to_underlying ( IntPacking_e::GENERIC_PFOR ) ]	= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockGenericPFOR_Values<true,true>;
	}
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
void Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::SetupPackingFuncs_ValuesBinary()
{
	auto & dFuncs = m_dProcessingFuncs;
	if ( m_tSettings.m_bExclude )
	{
		dFuncs [ to_underlying ( IntPacking_e::TABLE ) ]		= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockTable_Values<false,false>;
		dFuncs [ to_underlying ( IntPacking_e::DELTA_PFOR ) ]	= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockDeltaPFOR_Values<false,false>;
		dFuncs [ to_underlying ( IntPacking_e::GENERIC_PFOR ) ]	= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockGenericPFOR_Values<false,false>;
	}
	else
	{
		dFuncs [ to_underlying ( IntPacking_e::TABLE ) ]		= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockTable_Values<true,false>;
		dFuncs [ to_underlying ( IntPacking_e::DELTA_PFOR ) ]	= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockDeltaPFOR_Values<true,false>;
		dFuncs [ to_underlying ( IntPacking_e::GENERIC_PFOR ) ]	= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockGenericPFOR_Values<true,false>;
	}
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
void Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::SetupPackingFuncs_Range()
{
	auto & dFuncs = m_dProcessingFuncs;
	dFuncs [ to_underlying ( IntPacking_e::TABLE ) ]		= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockTable_Range;
	dFuncs [ to_underlying ( IntPacking_e::DELTA_PFOR ) ]	= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockDeltaPFOR_Range;
	dFuncs [ to_underlying ( IntPacking_e::GENERIC_PFOR ) ]	= &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockGenericPFOR_Range;
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
void Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::SetupPackingFuncs()
{
	auto & dFuncs = m_dProcessingFuncs;
	for ( auto & i : dFuncs )
		i = nullptr;

	const int LINEAR_SEARCH_THRESH = 128;

	// doesn't depend on filter type; just fills result with rowids
	dFuncs [ to_underlying ( IntPacking_e::CONST ) ] = &Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockConst;

	switch ( m_tSettings.m_eType )
	{
	case FilterType_e::VALUES:
		if ( m_tSettings.m_dValues.size()==1 )
			SetupPackingFuncs_SingleValue();
		else if ( m_tSettings.m_dValues.size()<=LINEAR_SEARCH_THRESH )
			SetupPackingFuncs_ValuesLinear();
		else
			SetupPackingFuncs_ValuesBinary();
		break;

	case FilterType_e::RANGE:
	case FilterType_e::FLOATRANGE:
		SetupPackingFuncs_Range();
		break;

	default:
		assert ( 0 && "Unsupported filter type" );
		break;
	}
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
void Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::FixupFilterSettings()
{
	if ( ACCESSOR::m_tHeader.GetType()!=AttrType_e::FLOAT )
		return;

	// this is basically the same stuff we do when we create filters, but we don't have access to previously modified filter settings
	// that's why we need to do it all over again
	if ( m_tSettings.m_eType==FilterType_e::VALUES && m_tSettings.m_dValues.size()==1 )
	{
		m_tSettings.m_eType = FilterType_e::FLOATRANGE;
		m_tSettings.m_fMinValue = m_tSettings.m_fMaxValue = (float)m_tSettings.m_dValues[0];
	}

	if ( m_tSettings.m_eType==FilterType_e::RANGE )
	{
		m_tSettings.m_eType = FilterType_e::FLOATRANGE;
		m_tSettings.m_fMinValue = (float)m_tSettings.m_iMinValue;
		m_tSettings.m_fMaxValue = (float)m_tSettings.m_iMaxValue;
	}
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
int Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockConst ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	return m_tBlockConst.ProcessSubblock ( pRowID, StoredBlockTraits_t::GetNumSubblockValues(iSubblockIdInBlock) );
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
template <bool EQ>
int Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockGenericPFOR_SingleValue ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockPFOR.ReadSubblock_Generic ( iSubblockIdInBlock, *ACCESSOR::m_pReader );
	return m_tBlockValues.template ProcessSubblock_SingleValue<EQ> ( pRowID, ACCESSOR::m_tBlockPFOR.GetAllValues() );
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
template <bool EQ, bool LINEAR>
int Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockGenericPFOR_Values ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockPFOR.ReadSubblock_Generic ( iSubblockIdInBlock, *ACCESSOR::m_pReader );

	if ( LINEAR )
		return m_tBlockValues.template ProcessSubblock_ValuesLinear<EQ> ( pRowID, ACCESSOR::m_tBlockPFOR.GetAllValues() );

	return m_tBlockValues.template ProcessSubblock_ValuesBinary<EQ> ( pRowID, ACCESSOR::m_tBlockPFOR.GetAllValues() );
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
int Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockGenericPFOR_Range ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockPFOR.ReadSubblock_Generic ( iSubblockIdInBlock, *ACCESSOR::m_pReader );
	return m_tBlockValues.template ProcessSubblock_Range<RANGE_EVAL> ( pRowID, ACCESSOR::m_tBlockPFOR.GetAllValues() );
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
template <bool EQ>
int Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockDeltaPFOR_SingleValue ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockPFOR.ReadSubblock_Delta ( iSubblockIdInBlock, *ACCESSOR::m_pReader );
	return m_tBlockValues.template ProcessSubblock_SingleValue<EQ> ( pRowID, ACCESSOR::m_tBlockPFOR.GetAllValues() );
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
template <bool EQ, bool LINEAR>
int Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockDeltaPFOR_Values ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockPFOR.ReadSubblock_Delta ( iSubblockIdInBlock, *ACCESSOR::m_pReader );

	if ( LINEAR )
		return m_tBlockValues.template ProcessSubblock_ValuesLinear<EQ> ( pRowID, ACCESSOR::m_tBlockPFOR.GetAllValues() );

	return m_tBlockValues.template ProcessSubblock_ValuesBinary<EQ> ( pRowID, ACCESSOR::m_tBlockPFOR.GetAllValues() );
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
int Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockDeltaPFOR_Range ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockPFOR.ReadSubblock_Delta ( iSubblockIdInBlock, *ACCESSOR::m_pReader );
	return m_tBlockValues.template ProcessSubblock_Range<RANGE_EVAL> ( pRowID, ACCESSOR::m_tBlockPFOR.GetAllValues() );
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
template <bool EQ>
int Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockTable_SingleValue ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockTable.ReadSubblock ( iSubblockIdInBlock, StoredBlockTraits_t::GetNumSubblockValues(iSubblockIdInBlock), *ACCESSOR::m_pReader );
	return m_tBlockTable.template ProcessSubblock_SingleValue<EQ> ( pRowID, ACCESSOR::m_tBlockTable.GetValueIndexes() );
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
template <bool EQ, bool LINEAR>
int Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockTable_Values ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockTable.ReadSubblock ( iSubblockIdInBlock, StoredBlockTraits_t::GetNumSubblockValues(iSubblockIdInBlock), *ACCESSOR::m_pReader );

	if ( LINEAR )
		return m_tBlockTable.template ProcessSubblock_ValuesLinear<EQ> ( pRowID, ACCESSOR::m_tBlockTable.GetValueIndexes() );

	return m_tBlockTable.template ProcessSubblock_ValuesBinary<EQ> ( pRowID, ACCESSOR::m_tBlockTable.GetValueIndexes() );
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
int Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::ProcessSubblockTable_Range ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockTable.ReadSubblock ( iSubblockIdInBlock, StoredBlockTraits_t::GetNumSubblockValues(iSubblockIdInBlock), *ACCESSOR::m_pReader );
	return m_tBlockTable.ProcessSubblock_Range ( pRowID, ACCESSOR::m_tBlockTable.GetValueIndexes() );
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
bool Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	if ( m_iCurSubblock>=ANALYZER::m_iTotalSubblocks )
		return false;

	uint32_t * pRowIdStart = m_dCollected.data();
	uint32_t * pRowID = pRowIdStart;
	uint32_t * pRowIdMax = pRowIdStart + ACCESSOR::m_iSubblockSize;

	// we scan until we find at least 128 (subblock size) matches.
	// this might lead to this analyzer scanning the whole index
	// a more responsive version would return after processing each 128 docs
	// (even if it doesn't find any matches)
	while ( pRowID<pRowIdMax )
	{
		int iSubblockIdInBlock = ACCESSOR::GetSubblockIdInBlock ( m_pMatchingSubblocks->GetBlock(m_iCurSubblock) );
		assert(m_fnProcessSubblock);
		m_iNumProcessed += (*this.*m_fnProcessSubblock) ( pRowID, iSubblockIdInBlock );

		if ( !MoveToSubblock ( m_iCurSubblock+1 ) )
			break;
	}

	return CheckEmptySpan ( pRowID, pRowIdStart, dRowIdBlock );
}

template<typename VALUES, typename ACCESSOR_VALUES, typename RANGE_EVAL>
bool Analyzer_INT_T<VALUES,ACCESSOR_VALUES,RANGE_EVAL>::MoveToBlock ( int iNextBlock )
{
	int iNumMatchingSubblocks = m_pMatchingSubblocks->GetNumBlocks();

	while(true)
	{
		m_iCurBlockId = iNextBlock;
		ACCESSOR::SetCurBlock(m_iCurBlockId);

		if ( ACCESSOR::m_ePacking!=IntPacking_e::CONST && ACCESSOR::m_ePacking!=IntPacking_e::TABLE )
			break;

		if ( ACCESSOR::m_ePacking==IntPacking_e::CONST )
		{
			if ( m_tBlockConst.SetupNextBlock<ACCESSOR_VALUES,RANGE_EVAL> ( ACCESSOR::m_tBlockConst, !m_tSettings.m_bExclude ) )
				break;
		}
		else
		{
			if ( m_tBlockTable.SetupNextBlock<ACCESSOR_VALUES,RANGE_EVAL> ( ACCESSOR::m_tBlockTable, !m_tSettings.m_bExclude ) )
				break;
		}

		while ( iNextBlock==m_iCurBlockId && m_iCurSubblock<iNumMatchingSubblocks )
			iNextBlock = ACCESSOR::SubblockId2BlockId ( m_pMatchingSubblocks->GetBlock(m_iCurSubblock++) );

		if ( m_iCurSubblock>=iNumMatchingSubblocks )
			return false;
	}

	m_fnProcessSubblock = m_dProcessingFuncs [ to_underlying ( ACCESSOR::m_ePacking ) ];
	assert ( m_fnProcessSubblock );

	return true;
}

template < bool LEFT_CLOSED, bool RIGHT_CLOSED, bool LEFT_UNBOUNDED, bool RIGHT_UNBOUNDED >
struct ValueInInterval_T
{
	template<typename T=int64_t>
	static inline bool Eval ( T tValue, T tMin, T tMax )
	{
		return ValueInInterval<T, LEFT_CLOSED, RIGHT_CLOSED, LEFT_UNBOUNDED, RIGHT_UNBOUNDED> ( tValue, tMin, tMax );
	}
};

//////////////////////////////////////////////////////////////////////////

Iterator_i * CreateIteratorUint32 ( const AttributeHeader_i & tHeader, FileReader_c * pReader )
{
	return ::new Iterator_INT_T<uint32_t> ( tHeader, pReader );
}


Iterator_i * CreateIteratorUint64 ( const AttributeHeader_i & tHeader, FileReader_c * pReader )
{
	return ::new Iterator_INT_T<uint64_t> ( tHeader, pReader );
}

//////////////////////////////////////////////////////////////////////////

template <typename RANGE_EVAL>
static Analyzer_i * CreateAnalyzerInt ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings )
{
	switch ( tHeader.GetType() )
	{
	case AttrType_e::UINT32:
	case AttrType_e::TIMESTAMP:
		return ::new Analyzer_INT_T<uint32_t, uint32_t, RANGE_EVAL> ( tHeader, pReader, tSettings );

	case AttrType_e::INT64:
		return ::new Analyzer_INT_T<int64_t, uint64_t, RANGE_EVAL> ( tHeader, pReader, tSettings );

	case AttrType_e::FLOAT:
		return ::new Analyzer_INT_T<float, uint32_t, RANGE_EVAL> ( tHeader, pReader, tSettings );

	default:
		assert ( 0 && "Unknown int analyzer" );
		return nullptr;
	}
}


Analyzer_i * CreateAnalyzerInt ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings  )
{
	if ( tSettings.m_eType!=FilterType_e::VALUES && tSettings.m_eType!=FilterType_e::RANGE && tSettings.m_eType!=FilterType_e::FLOATRANGE )
		return nullptr;

	int iIndex = tSettings.m_bLeftClosed*8 + tSettings.m_bRightClosed*4 + tSettings.m_bLeftUnbounded*2 + tSettings.m_bRightUnbounded;
	switch ( iIndex )
	{
	case 0:		return CreateAnalyzerInt<ValueInInterval_T<false, false, false, false>> ( tHeader, pReader, tSettings );
	case 1:		return CreateAnalyzerInt<ValueInInterval_T<false, false, false, true>>  ( tHeader, pReader, tSettings );
	case 2:		return CreateAnalyzerInt<ValueInInterval_T<false, false, true,  false>> ( tHeader, pReader, tSettings );
	case 3:		return CreateAnalyzerInt<ValueInInterval_T<false, false, true,  true>>  ( tHeader, pReader, tSettings );
	case 4:		return CreateAnalyzerInt<ValueInInterval_T<false, true,  false, false>> ( tHeader, pReader, tSettings );
	case 5:		return CreateAnalyzerInt<ValueInInterval_T<false, true,  false, true>>  ( tHeader, pReader, tSettings );
	case 6:		return CreateAnalyzerInt<ValueInInterval_T<false, true,  true,  false>> ( tHeader, pReader, tSettings );
	case 7:		return CreateAnalyzerInt<ValueInInterval_T<false, true,  true,  true>>  ( tHeader, pReader, tSettings );
	case 8:		return CreateAnalyzerInt<ValueInInterval_T<true,  false, false, false>> ( tHeader, pReader, tSettings );
	case 9:		return CreateAnalyzerInt<ValueInInterval_T<true,  false, false, true>>  ( tHeader, pReader, tSettings );
	case 10:	return CreateAnalyzerInt<ValueInInterval_T<true,  false, true,  false>> ( tHeader, pReader, tSettings );
	case 11:	return CreateAnalyzerInt<ValueInInterval_T<true,  false, true,  true>>  ( tHeader, pReader, tSettings );
	case 12:	return CreateAnalyzerInt<ValueInInterval_T<true,  true,  false, false>> ( tHeader, pReader, tSettings );
	case 13:	return CreateAnalyzerInt<ValueInInterval_T<true,  true,  false, true>>  ( tHeader, pReader, tSettings );
	case 14:	return CreateAnalyzerInt<ValueInInterval_T<true,  true,  true,  false>> ( tHeader, pReader, tSettings );
	case 15:	return CreateAnalyzerInt<ValueInInterval_T<true,  true,  true,  true>>  ( tHeader, pReader, tSettings );
	default:	return nullptr;
	}
}

} // namespace columnar

