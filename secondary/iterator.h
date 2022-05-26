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

#include "util/reader.h"
#include "util/codec.h"
#include "util/filter.h"
#include "util/blockiterator.h"
#include "common.h"
#include "builder.h"
#include <memory>

namespace SI
{
	common::BlockIterator_i * CreateRowidIterator ( Packing_e eType, uint64_t uRowStart, std::shared_ptr<util::FileReader_c> & pReader, std::shared_ptr<util::IntCodec_i> & pCodec, const common::RowidRange_t * pBounds, bool bCreateReader, std::string & sError );
}
