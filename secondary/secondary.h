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

#pragma once

#include <limits>
#include "util/blockiterator.h"
#include "common.h"

namespace SI
{
	struct ColumnInfo_t;

	struct SourceAttrTrait_t
	{
		AttrType_e	m_eType { AttrType_e::NONE };
		int			m_iAttr { -1 };
		std::string m_sName;
	};

	struct FilterRange_t
	{
		int64_t m_iMin { std::numeric_limits<int64_t>::min() };
		int64_t m_iMax { std::numeric_limits<int64_t>::max() };
		float m_fMin { std::numeric_limits<float>::min() };
		float m_fMax { std::numeric_limits<float>::max() };
		bool m_bHasEqualMin { true };
		bool m_bHasEqualMax { true };
		bool m_bOpenLeft { false };
		bool m_bOpenRight { false };
	};

	class FilterContext_i
	{
	public:
		FilterContext_i() = default;
		virtual ~FilterContext_i() {};
	};

	struct RowidRange_t
	{
		bool m_bHasRange = false;
		uint32_t m_uMin { std::numeric_limits<uint32_t>::min() };
		uint32_t m_uMax{ std::numeric_limits<uint32_t>::max() };
	};

	struct FilterArgs_t
	{
		ColumnInfo_t m_tCol;

		columnar::Span_T<uint64_t> m_dVals;
		FilterRange_t m_tRange;

		RowidRange_t m_tBounds;
	};

	class Index_i
	{
	public:
		Index_i() = default;
		virtual ~Index_i() = default;

		virtual ColumnInfo_t GetColumn ( const char * sName ) const = 0;
		virtual bool GetValsRows ( const FilterArgs_t & tArgs, std::string & sError, FilterContext_i * pCtx, std::vector<columnar::BlockIterator_i *> & dRes ) const = 0;
		virtual bool GetRangeRows ( const FilterArgs_t & tArgs, std::string & sError, FilterContext_i * pCtx, std::vector<columnar::BlockIterator_i *> & dRes ) const = 0;

		virtual bool SaveMeta ( std::string & sError ) = 0;
		virtual void ColumnUpdated ( const char * sName ) = 0;

		virtual FilterContext_i * CreateFilterContext () const = 0;

		virtual Collation_e GetCollation() const = 0;
		virtual uint64_t GetHash ( const char * sVal ) const = 0;
	};

	class Builder_i;

} // namespace SI

extern "C"
{
	DLLEXPORT SI::Index_i *		CreateSecondaryIndex ( const char * sFile, std::string & sError );
	DLLEXPORT SI::Builder_i *	CreateBuilder ( const std::vector<SI::SourceAttrTrait_t> & dSrcAttrs, int iMemoryLimit, SI::Collation_e eCollation, const char * sFile, std::string & sError );

	DLLEXPORT void CollationInit ( const std::array<SI::StrHash_fn, (size_t)SI::Collation_e::TOTAL> & dCollations );

	DLLEXPORT int				GetSecondaryLibVersion();
	DLLEXPORT const char *		GetSecondaryLibVersionStr();
	DLLEXPORT int				GetSecondaryStorageVersion();
}