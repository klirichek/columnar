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

#include "accessorstr.h"
#include "accessortraits.h"
#include "builderstr.h"
#include "reader.h"

namespace columnar
{


class StrHashReader_c
{
public:
	FORCE_INLINE void		ReadHashesWithNullMap ( FileReader_c & tReader, int iValues, int iNumHashes );
	FORCE_INLINE void		ReadHashes ( FileReader_c & tReader, int iValues, bool bNeedHashes );
	FORCE_INLINE uint64_t	GetHash ( int iId ) const { return m_dHashes[iId]; }

private:
	SpanResizeable_T<uint32_t>	m_dNullMap;
	SpanResizeable_T<uint64_t>	m_dHashes;
	SpanResizeable_T<uint32_t>	m_dTmp;
};


void StrHashReader_c::ReadHashesWithNullMap ( FileReader_c & tReader, int iValues, int iNumHashes )
{
	assert ( iValues==128 );
	m_dTmp.resize ( iValues >> 5 );
	m_dNullMap.resize(iValues);
	tReader.Read ( (uint8_t*)m_dTmp.data(), m_dTmp.size()*sizeof(m_dTmp[0]) );
	BitUnpack128 ( m_dTmp, m_dNullMap, 1 );
	tReader.Read ( (uint8_t*)m_dHashes.data(), iNumHashes*sizeof(uint64_t) );

	const uint32_t * pNullMap = m_dNullMap.end()-1;
	uint64_t * pDstMin = m_dHashes.data();
	uint64_t * pDst = pDstMin+iValues-1;
	uint64_t * pSrc = pDstMin+iNumHashes-1;
	while ( pDst>=pDstMin )
	{
		if ( *pNullMap )
			*pDst = *pSrc--;
		else
			*pDst = 0;

		pNullMap--;
		pDst--;
	}
}


void StrHashReader_c::ReadHashes ( FileReader_c & tReader, int iValues, bool bNeedHashes )
{
	int iNumHashes = tReader.Read_uint8();
	bool bHaveNullMap = iValues!=iNumHashes;
	size_t tTotalHashSize = iNumHashes*sizeof(uint64_t);

	if ( !bNeedHashes )
	{
		size_t tOffsetToData = tTotalHashSize + ( bHaveNullMap ? ( iValues>>3 ) : 0 );
		tReader.Seek ( tReader.GetPos() + tOffsetToData );
		return;
	}

	m_dHashes.resize(iValues);
	if ( bHaveNullMap )
		ReadHashesWithNullMap ( tReader, iValues, iNumHashes );
	else
		tReader.Read ( (uint8_t*)m_dHashes.data(), tTotalHashSize );
}

//////////////////////////////////////////////////////////////////////////

class StoredBlock_StrConst_c : public StrHashReader_c
{
	using BASE = StrHashReader_c;

public:
	FORCE_INLINE void		ReadHeader ( FileReader_c & tReader, bool bHaveHashes, bool bNeedHashes );
	template <bool PACK> FORCE_INLINE int GetValue ( uint8_t * & pValue );
	FORCE_INLINE int		GetValueLength() const { return (int)m_dValue.size(); }
	FORCE_INLINE uint64_t	GetHash() const { return BASE::GetHash(0); }

private:
	std::vector<uint8_t>	m_dValue;
	std::vector<uint8_t>	m_dValuePacked;
};


void StoredBlock_StrConst_c::ReadHeader ( FileReader_c & tReader, bool bHaveHashes, bool bNeedHashes )
{
	if ( bHaveHashes )
		ReadHashes ( tReader, 1, bNeedHashes );

	int iLength = tReader.Unpack_uint32();
	m_dValue.resize(iLength);
	tReader.Read ( m_dValue.data(), iLength );

	ByteCodec_c::PackData ( m_dValuePacked, Span_T<uint8_t>(m_dValue) );
}

template <bool PACK>
int StoredBlock_StrConst_c::GetValue ( uint8_t * & pValue )
{
	if ( PACK )
	{
		uint8_t * pData = new uint8_t [ m_dValuePacked.size() ];
		memcpy ( pData, m_dValuePacked.data(), m_dValuePacked.size() );
		pValue = pData;
	}
	else
		pValue = m_dValue.data();

	return (int)m_dValue.size();

}

//////////////////////////////////////////////////////////////////////////

class StoredBlock_StrConstLen_c
{
public:
	FORCE_INLINE void		ReadHeader ( FileReader_c & tReader, int iValues, bool bHaveStringHashes );
	template <bool PACK> FORCE_INLINE int ReadValue ( uint8_t * & pValue, FileReader_c & tReader, int iIdInBlock );
	FORCE_INLINE int		GetValueLength() const { return (int)m_tValuesOffset; }
	FORCE_INLINE uint64_t	GetHash ( FileReader_c & tReader, int iIdInBlock );

private:
	int64_t					m_tHashOffset = 0;
	int64_t					m_tValuesOffset = 0;
	size_t					m_tValueLength = 0;
	int						m_iLastReadId = -1;
	std::vector<uint8_t>	m_dValue;
};


void StoredBlock_StrConstLen_c::ReadHeader ( FileReader_c & tReader, int iValues, bool bHaveStringHashes )
{
	size_t tLength = tReader.Unpack_uint64();

	if ( bHaveStringHashes )
	{
		m_tHashOffset = tReader.GetPos();
		m_tValuesOffset = m_tHashOffset + iValues*sizeof(uint64_t);
	}
	else
		m_tValuesOffset = tReader.GetPos();

	m_tValueLength = tLength;
	m_dValue.resize(m_tValueLength);

	m_iLastReadId = -1;
}

template <bool PACK>
int StoredBlock_StrConstLen_c::ReadValue ( uint8_t * & pValue, FileReader_c & tReader, int iIdInBlock )
{
	// non-sequental read or first read?
	if ( m_iLastReadId==-1 || m_iLastReadId+1!=iIdInBlock )
	{
		int64_t tOffset = m_tValuesOffset + int64_t(iIdInBlock)*m_tValueLength;
		tReader.Seek ( tOffset );
	}

	m_iLastReadId = iIdInBlock;

	if ( PACK )
	{
		uint8_t * pData = nullptr;
		std::tie ( pValue, pData ) = ByteCodec_c::PackData(m_tValueLength);
		tReader.Read ( pData, m_tValueLength );
	}
	else
	{
		// try to read without copying first
		if ( !tReader.ReadFromBuffer ( pValue, m_tValueLength ) )
		{
			// can't read directly from reader's buffer? read to a temp buffer then
			m_dValue.resize(m_tValueLength);
			tReader.Read ( m_dValue.data(), m_tValueLength );
			pValue = m_dValue.data();
		}
	}

	return (int)m_tValueLength;
}


uint64_t StoredBlock_StrConstLen_c::GetHash ( FileReader_c & tReader, int iIdInBlock )
{
	// we assume that we are reading either hashes or values but not both at the same time
	if ( m_iLastReadId==-1 || m_iLastReadId+1!=iIdInBlock )
	{
		int64_t tOffset = m_tHashOffset + sizeof(uint64_t)*iIdInBlock;
		tReader.Seek(tOffset);
	}

	m_iLastReadId = iIdInBlock;

	return tReader.Read_uint64();
}

//////////////////////////////////////////////////////////////////////////

class StoredBlock_StrTable_c : public StrHashReader_c
{
	using BASE = StrHashReader_c;

public:
							StoredBlock_StrTable_c ( const std::string & sCodec32, const std::string & sCodec64, int iSubblockSize );

	FORCE_INLINE void		ReadHeader ( FileReader_c & tReader, int iValues, bool bHaveHashes, bool bNeedHashes );
	FORCE_INLINE void		ReadSubblock ( int iSubblockId, int iNumValues, FileReader_c & tReader );
	FORCE_INLINE int		GetValueLength ( int iIdInSubblock ) const			{ return m_dTableValueLengths[m_dValueIndexes[iIdInSubblock]]; }
	template <bool PACK>
	FORCE_INLINE int		GetValue ( uint8_t * & pValue, int iIdInSubblock )	{ return PackValue<uint8_t,PACK> ( m_dTableValues [ m_dValueIndexes[iIdInSubblock] ], pValue ); } 
	FORCE_INLINE uint64_t	GetHash ( int iIdInSubblock ) const					{ return BASE::GetHash ( m_dValueIndexes[iIdInSubblock] ); }

private:
	std::vector<std::vector<uint8_t>>	m_dTableValues;
	SpanResizeable_T<uint32_t>			m_dTableValueLengths;
	SpanResizeable_T<uint32_t> 			m_dTmp;
	std::vector<uint32_t>				m_dValueIndexes;
	std::vector<uint32_t>				m_dEncoded;
	std::unique_ptr<IntCodec_i>			m_pCodec;

	int64_t		m_iValuesOffset = 0;
	int			m_iSubblockId = -1;
	int			m_iBits = 0;
};


StoredBlock_StrTable_c::StoredBlock_StrTable_c ( const std::string & sCodec32, const std::string & sCodec64, int iSubblockSize )
	: m_pCodec ( CreateIntCodec ( sCodec32, sCodec64 ) ) 
{
	m_dValueIndexes.resize(iSubblockSize);
}


void StoredBlock_StrTable_c::ReadHeader ( FileReader_c & tReader, int iValues, bool bHaveHashes, bool bNeedHashes )
{
	m_dTableValues.resize ( tReader.Read_uint8() );

	if ( bHaveHashes )
		ReadHashes ( tReader, (int)m_dTableValues.size(), bNeedHashes ); // read or skip hashes depending on bNeedHashes

	uint32_t uTotalSize = tReader.Unpack_uint32();
	DecodeValues_Delta_PFOR ( m_dTableValueLengths, tReader, *m_pCodec, m_dTmp, uTotalSize, false );

	for ( size_t i = 0; i < m_dTableValues.size(); i++ )
	{
		auto & tValue = m_dTableValues[i];
		tValue.resize ( m_dTableValueLengths[i] );
		tReader.Read ( tValue.data(), tValue.size() );
	}

	m_iBits = CalcNumBits ( m_dTableValues.size() );
	m_dEncoded.resize ( ( m_dValueIndexes.size() >> 5 ) * m_iBits );

	m_iValuesOffset = tReader.GetPos();
	m_iSubblockId = -1;
}


void StoredBlock_StrTable_c::ReadSubblock ( int iSubblockId, int iNumValues, FileReader_c & tReader )
{
	if ( m_iSubblockId==iSubblockId )
		return;

	m_iSubblockId = iSubblockId;

	size_t uPackedSize = m_dEncoded.size()*sizeof ( m_dEncoded[0] );
	tReader.Seek ( m_iValuesOffset + uPackedSize*iSubblockId );
	tReader.Read ( (uint8_t*)m_dEncoded.data(), uPackedSize );
	BitUnpack128 ( m_dEncoded, m_dValueIndexes, m_iBits );
}

//////////////////////////////////////////////////////////////////////////

class StoredBlock_StrGeneric_c : public StrHashReader_c
{
public:
						StoredBlock_StrGeneric_c ( const std::string & sCodec32, const std::string & sCodec64 ) : m_pCodec ( CreateIntCodec ( sCodec32, sCodec64 ) ) {}

	FORCE_INLINE void	ReadHeader ( FileReader_c & tReader, bool bHaveHashes, bool bNeedHashes );
	FORCE_INLINE void	ReadSubblock ( int iSubblockId, int iSubblockValues, FileReader_c & tReader );
	template <bool PACK> FORCE_INLINE int ReadValue ( uint8_t * & pValue, int iIdInSubblock, FileReader_c & tReader );
	FORCE_INLINE int	GetValueLength ( int iIdInSubblock ) const;

private:
	std::unique_ptr<IntCodec_i>	m_pCodec;
	SpanResizeable_T<uint32_t>	m_dTmp;
	SpanResizeable_T<uint64_t>	m_dOffsets;
	SpanResizeable_T<uint64_t>	m_dCumulativeLengths;
	SpanResizeable_T<uint8_t>	m_dValue;

	int		m_iSubblockId = -1;
	int64_t	m_tValuesOffset = 0;
	bool	m_bHaveHashes = false;
	bool	m_bNeedHashes = false;
	int64_t	m_iFirstValueOffset = 0;
	int		m_iLastReadId = -1;
};


void StoredBlock_StrGeneric_c::ReadHeader ( FileReader_c & tReader, bool bHaveHashes, bool bNeedHashes )
{
	uint32_t uSubblockSize = tReader.Unpack_uint32();
	DecodeValues_Delta_PFOR ( m_dOffsets, tReader, *m_pCodec, m_dTmp, uSubblockSize, false );

	m_tValuesOffset = tReader.GetPos();
	m_bHaveHashes = bHaveHashes;
	m_bNeedHashes = bHaveHashes && bNeedHashes;
}


void StoredBlock_StrGeneric_c::ReadSubblock ( int iSubblockId, int iSubblockValues, FileReader_c & tReader )
{
	if ( m_iSubblockId==iSubblockId )
		return;

	m_iSubblockId = iSubblockId;
	tReader.Seek ( m_tValuesOffset+m_dOffsets[iSubblockId] );

	if ( m_bHaveHashes )
		ReadHashes ( tReader, iSubblockValues, m_bNeedHashes ); // read or skip hashes depending on m_bNeedHashes

	uint32_t uSubblockSize = (uint32_t)tReader.Unpack_uint64();

	// the logic is that if we need hashes, we don't need string lengths/values
	// maybe there's a case when we need both? we'll need separate flags in that case
	if ( !m_bNeedHashes )
		DecodeValues_Delta_PFOR ( m_dCumulativeLengths, tReader, *m_pCodec, m_dTmp, uSubblockSize, false );

	m_iFirstValueOffset = tReader.GetPos();
	m_iLastReadId = -1;
}


int StoredBlock_StrGeneric_c::GetValueLength ( int iIdInSubblock ) const
{
	uint64_t uLength = m_dCumulativeLengths[iIdInSubblock];
	if ( iIdInSubblock>0 )
		uLength -= m_dCumulativeLengths[iIdInSubblock-1];

	return (int)uLength;
}

template <bool PACK>
int StoredBlock_StrGeneric_c::ReadValue ( uint8_t * & pValue, int iIdInSubblock, FileReader_c & tReader )
{
	int iLength = GetValueLength(iIdInSubblock);

	int64_t iOffset = m_iFirstValueOffset;
	if ( iIdInSubblock>0 )
		iOffset += m_dCumulativeLengths[iIdInSubblock-1];

	if ( m_iLastReadId==-1 || m_iLastReadId+1!=iIdInSubblock )
		tReader.Seek(iOffset);

	m_iLastReadId = iIdInSubblock;

	if ( PACK )
	{
		uint8_t * pData = nullptr;
		std::tie ( pValue, pData ) = ByteCodec_c::PackData ( (size_t)iLength );
		tReader.Read ( pData, iLength );
	}
	else
	{
		// try to read without copying first
		if ( !tReader.ReadFromBuffer ( pValue, iLength ) )
		{
			// can't read directly from reader's buffer? read to a temp buffer then
			m_dValue.resize(iLength);
			tReader.Read ( m_dValue.data(), iLength );
			pValue = m_dValue.data();
		}
	}

	return iLength;
}

//////////////////////////////////////////////////////////////////////////

class Iterator_String_c : public Iterator_i, public StoredBlockTraits_t
{
public:
				Iterator_String_c ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const IteratorHints_t & tHints );

	uint32_t	AdvanceTo ( uint32_t tRowID ) final;

	int64_t		Get() final						{ assert ( 0 && "INTERNAL ERROR: requesting int from string iterator" ); return 0; }

	int			Get ( const uint8_t * & pData ) final;
	uint8_t *	GetPacked() final;
	int			GetLength() final;

	uint64_t	GetStringHash() final;
	bool		HaveStringHashes() const final	{ return m_tHeader.HaveStringHashes(); }

private:
	const AttributeHeader_i &		m_tHeader;
	IteratorHints_t					m_tHints;
	std::unique_ptr<FileReader_c>	m_pReader;
	StrPacking_e					m_ePacking = StrPacking_e::CONSTLEN;
	StoredBlock_StrConst_c			m_tBlockConst;
	StoredBlock_StrConstLen_c		m_tBlockConstLen;
	StoredBlock_StrTable_c			m_tBlockTable;
	StoredBlock_StrGeneric_c		m_tBlockGeneric;

	uint8_t	*						m_pResult = nullptr;
	size_t							m_tValueLength = 0;

	void (Iterator_String_c::*m_fnReadValue)() = nullptr;
	void (Iterator_String_c::*m_fnReadValuePacked)() = nullptr;
	int (Iterator_String_c::*m_fnGetValueLength)() = nullptr;
	uint64_t (Iterator_String_c::*m_fnGetHash)() = nullptr;

	inline void	SetCurBlock ( uint32_t uBlockId );

	template <bool PACK> void ReadValue_Const()		{ m_tValueLength = m_tBlockConst.GetValue<PACK>(m_pResult); }
	int			GetValueLen_Const()					{ return m_tBlockConst.GetValueLength(); }
	uint64_t	GetHash_Const()						{ return m_tBlockConst.GetHash(); }

	template <bool PACK> void ReadValue_ConstLen()	{ m_tValueLength = m_tBlockConstLen.ReadValue<PACK> ( m_pResult, *m_pReader, m_tRequestedRowID-m_tStartBlockRowId ); }
	int			GetValueLen_ConstLen()				{ return m_tBlockConstLen.GetValueLength(); }
	uint64_t	GetHash_ConstLen()					{ return m_tBlockConstLen.GetHash ( *m_pReader, m_tRequestedRowID-m_tStartBlockRowId ); }

	template <bool PACK> void ReadValue_Table()		{ m_tValueLength = m_tBlockTable.template GetValue<PACK>( m_pResult, ReadSubblock(m_tBlockTable) ); }
	int			GetValueLen_Table()					{ return m_tBlockTable.GetValueLength ( ReadSubblock(m_tBlockTable) ); }
	uint64_t	GetHash_Table()						{ return m_tBlockTable.GetHash ( ReadSubblock(m_tBlockTable) ); }

	template <bool PACK> void ReadValue_Generic()	{ m_tValueLength = m_tBlockGeneric.template ReadValue<PACK>( m_pResult, ReadSubblock(m_tBlockGeneric), *m_pReader ); }
	int			GetValueLen_Generic()				{ return m_tBlockGeneric.GetValueLength ( ReadSubblock(m_tBlockGeneric) ); }
	uint64_t	GetHash_Generic()					{ return m_tBlockGeneric.GetHash ( ReadSubblock(m_tBlockGeneric) ); }

	template <typename T>
	FORCE_INLINE int ReadSubblock ( T & tSubblock );
};


Iterator_String_c::Iterator_String_c ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const IteratorHints_t & tHints )
	: StoredBlockTraits_t ( tHeader.GetSettings().m_iSubblockSize )
	, m_tHeader ( tHeader )
	, m_tHints ( tHints )
	, m_pReader ( pReader )
	, m_tBlockTable ( tHeader.GetSettings().m_sCompressionUINT32, tHeader.GetSettings().m_sCompressionUINT64, tHeader.GetSettings().m_iSubblockSize )
	, m_tBlockGeneric ( tHeader.GetSettings().m_sCompressionUINT32, tHeader.GetSettings().m_sCompressionUINT64 )
{
	assert(pReader);
}


void Iterator_String_c::SetCurBlock ( uint32_t uBlockId )
{
	m_pReader->Seek ( m_tHeader.GetBlockOffset(uBlockId) );
	m_ePacking = (StrPacking_e)m_pReader->Unpack_uint32();

	switch ( m_ePacking )
	{
	case StrPacking_e::CONST:
		m_fnReadValue			= &Iterator_String_c::ReadValue_Const<false>;
		m_fnReadValuePacked		= &Iterator_String_c::ReadValue_Const<true>;
		m_fnGetValueLength		= &Iterator_String_c::GetValueLen_Const;
		m_fnGetHash				= &Iterator_String_c::GetHash_Const;
		m_tBlockConst.ReadHeader ( *m_pReader, m_tHeader.HaveStringHashes(), m_tHints.m_bNeedStringHashes );
		break;

	case StrPacking_e::CONSTLEN:
		m_fnReadValue			= &Iterator_String_c::ReadValue_ConstLen<false>;
		m_fnReadValuePacked		= &Iterator_String_c::ReadValue_ConstLen<true>;
		m_fnGetValueLength		= &Iterator_String_c::GetValueLen_ConstLen;
		m_fnGetHash				= &Iterator_String_c::GetHash_ConstLen;
		m_tBlockConstLen.ReadHeader ( *m_pReader, m_tHeader.GetNumDocs(uBlockId), m_tHeader.HaveStringHashes() );
		break;

	case StrPacking_e::TABLE:
		m_fnReadValue			= &Iterator_String_c::ReadValue_Table<false>;
		m_fnReadValuePacked		= &Iterator_String_c::ReadValue_Table<true>;
		m_fnGetValueLength		= &Iterator_String_c::GetValueLen_Table;
		m_fnGetHash				= &Iterator_String_c::GetHash_Table;
		m_tBlockTable.ReadHeader ( *m_pReader, m_tHeader.GetNumDocs(uBlockId), m_tHeader.HaveStringHashes(), m_tHints.m_bNeedStringHashes );
		break;

	case StrPacking_e::GENERIC:
		m_fnReadValue			= &Iterator_String_c::ReadValue_Generic<false>;
		m_fnReadValuePacked		= &Iterator_String_c::ReadValue_Generic<true>;
		m_fnGetValueLength		= &Iterator_String_c::GetValueLen_Generic;
		m_fnGetHash				= &Iterator_String_c::GetHash_Generic;
		m_tBlockGeneric.ReadHeader ( *m_pReader, m_tHeader.HaveStringHashes(), m_tHints.m_bNeedStringHashes );
		break;

	default:
		assert ( 0 && "Packing not implemented yet" );
		break;
	}

	m_tRequestedRowID = INVALID_ROW_ID;
	m_pResult = nullptr;

	SetBlockId ( uBlockId, m_tHeader.GetNumDocs(uBlockId) );
}


uint32_t Iterator_String_c::AdvanceTo ( uint32_t tRowID )
{
	if ( m_tRequestedRowID==tRowID ) // might happen on GetLength/Get calls
		return tRowID;

	uint32_t uBlockId = RowId2BlockId(tRowID);
	if ( uBlockId!=m_uBlockId )
		SetCurBlock(uBlockId);

	m_tRequestedRowID = tRowID;

	return tRowID;
}


int Iterator_String_c::Get ( const uint8_t * & pData )
{
	assert(m_fnReadValue);
	(*this.*m_fnReadValue)();

	pData = m_pResult;
	m_pResult = nullptr;

	return (int)m_tValueLength;
}


uint8_t * Iterator_String_c::GetPacked()
{
	assert(m_fnReadValuePacked);
	(*this.*m_fnReadValuePacked)();

	uint8_t * pData = m_pResult;
	m_pResult = nullptr;
	return pData;
}


int Iterator_String_c::GetLength()
{
	assert(m_fnGetValueLength);
	return (*this.*m_fnGetValueLength)();
}


uint64_t Iterator_String_c::GetStringHash()
{
	assert(m_fnGetHash);
	return (*this.*m_fnGetHash)();
}

template <typename T>
int Iterator_String_c::ReadSubblock ( T & tSubblock )
{
	uint32_t uIdInBlock = m_tRequestedRowID - m_tStartBlockRowId;
	int iSubblockId = StoredBlockTraits_t::GetSubblockId(uIdInBlock);
	tSubblock.ReadSubblock ( iSubblockId, StoredBlockTraits_t::GetNumSubblockValues(iSubblockId), *m_pReader );
	return GetValueIdInSubblock(uIdInBlock);
}

//////////////////////////////////////////////////////////////////////////

Iterator_i * CreateIteratorStr ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const IteratorHints_t & tHints )
{
	return new Iterator_String_c ( tHeader, pReader, tHints );
}

} // namespace columnar