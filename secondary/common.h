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

#include <string>
#include <array>

#include "common/schema.h"

namespace SI
{
	static const int LIB_VERSION = 2;
	static const uint32_t STORAGE_VERSION = 1;

	struct ColumnInfo_t
	{
		common::AttrType_e m_eType = common::AttrType_e::NONE;
		std::string m_sName;
		bool		m_bEnabled { true };
	};


} // namespace SI
