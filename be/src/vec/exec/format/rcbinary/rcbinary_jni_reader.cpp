// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "rcbinary_jni_reader.h"

#include <map>
#include <ostream>

#include "runtime/descriptors.h"
#include "runtime/types.h"
#include "common/logging.h"

namespace doris::vectorized {

RCBinaryJNIReader::RCBinaryJNIReader(RuntimeState* state, RuntimeProfile* profile,
                             const TFileScanRangeParams& params,
                             const std::vector<SlotDescriptor*>& file_slot_descs,
                             const TFileRangeDesc& range)
        : JniReader(file_slot_descs, state, profile), _params(params), _range(range) {
            // TODO: process _column_names & _column_types
        }

RCBinaryJNIReader::RCBinaryJNIReader(RuntimeProfile* profile, const TFileScanRangeParams& params,
              const TFileRangeDesc& range, const std::vector<SlotDescriptor*>& file_slot_descs)
    : JniReader(file_slot_descs, nullptr, profile), _params(params), _range(range) {
            // TODO: process _column_names & _column_types
    }

RCBinaryJNIReader::~RCBinaryJNIReader() = default;

TFileType::type RCBinaryJNIReader::get_file_type() {
    TFileType::type type;
    if (_range.__isset.file_type) {
        type = _range.file_type;
    } else {
        type = _params.file_type;
    }
    return type;
}

Status RCBinaryJNIReader::init_fetch_table_reader(
            std::unordered_map<std::string, ColumnValueRangeType>* colname_to_value_range)
{
    _colname_to_value_range = colname_to_value_range;
    std::ostringstream required_fields;
    std::ostringstream columns_types;
    std::vector<std::string> column_names;
    int index = 0;
    for (auto& desc : _file_slot_descs) {
        std::string field = desc->col_name();
        column_names.emplace_back(field);
        std::string type = JniConnector::get_jni_type(desc->type());
        if(index == 0) {
            required_fields << field;
            columns_types << type;
        } else {
            required_fields << "," << field;
            columns_types << "#" << type;
        }
        index++;
    }
    TFileType::type type = get_file_type();
    std::map<String, String> required_params = {
            {"uri", _range.path},
            {"required_fields", "col_tinyint"}, 
            {"columns_types", columns_types.str()},
            {"file_type", std::to_string(type)},
            {"is_get_table_schema", "false"},
    };
    if (type == TFileType::FILE_S3) {
        required_params.insert(_params.properties.begin(), _params.properties.end());
    }
    _jni_connector = std::make_unique<JniConnector>("org/apache/doris/hive/HiveJNIScanner",
                                                    required_params, column_names);
    RETURN_IF_ERROR(_jni_connector->init(_colname_to_value_range));
    return _jni_connector->open(_state, _profile);
}

Status RCBinaryJNIReader::init_fetch_table_schema_reader() {
    std::map<String, String> required_params = {
        {"uri", _range.path},
        {"file_type", std::to_string(get_file_type())},
        {"is_get_table_schema", "true"}
    };
    required_params.insert(_params.properties.begin(), _params.properties.end());
    _jni_connector = std::make_unique<JniConnector>("org/apache/doris/hive/HiveJNIScanner",
                                                    required_params);
    return _jni_connector->open(nullptr, _profile);                                                    
}

Status RCBinaryJNIReader::get_next_block(Block* block, size_t* read_rows, bool* eof) {
    LOG(WARNING) << "rcbinary start get next block";
    RETURN_IF_ERROR(_jni_connector->get_next_block(block, read_rows, eof));
    LOG(WARNING) << "success get next block";
    if (*eof) {
        LOG(WARNING) << "start close jni connector";
        RETURN_IF_ERROR(_jni_connector->close());
        LOG(WARNING) << "end close jni connector";
    }
    return Status::OK();
}

Status RCBinaryJNIReader::get_columns(std::unordered_map<std::string, TypeDescriptor>* name_to_type,
                                  std::unordered_set<std::string>* missing_cols) {
    for (auto& desc : _file_slot_descs) {
        name_to_type->emplace(desc->col_name(), desc->type());
        LOG(WARNING) << "RCBINARY slot name : " << desc->col_name();
    }
    return Status::OK();
}

Status RCBinaryJNIReader::get_parsed_schema(std::vector<std::string>* col_names,
                                            std::vector<TypeDescriptor>* col_types) {
    std::string table_schema_str;
    RETURN_IF_ERROR(_jni_connector->get_table_schema(table_schema_str));
    rapidjson::Document document;
    document.Parse(table_schema_str.c_str());
    if (document.IsArray()) {
        for (int i = 0; i < document.Size(); ++i) {
            rapidjson::Value& column_schema = document[i];
            col_names->push_back(column_schema["name"].GetString());
            col_types->push_back(convert_to_doris_type(column_schema));
        }
    }
    return _jni_connector->close();
}

TypeDescriptor RCBinaryJNIReader::convert_to_doris_type(const rapidjson::Value& column_schema) {
    auto schema_type = static_cast< ::doris::TPrimitiveType::type>(column_schema["type"].GetInt());
    switch (schema_type) {
    case TPrimitiveType::INT:
    case TPrimitiveType::STRING:
    case TPrimitiveType::BIGINT:
    case TPrimitiveType::BOOLEAN:
    case TPrimitiveType::DOUBLE:
    case TPrimitiveType::FLOAT:
    case TPrimitiveType::BINARY:
        return {thrift_to_type(schema_type)};
    case TPrimitiveType::ARRAY: {
        TypeDescriptor list_type(PrimitiveType::TYPE_ARRAY);
        const rapidjson::Value& childColumns = column_schema["childColumns"];
        list_type.add_sub_type(convert_to_doris_type(childColumns[0]));
        return list_type;
    }
    case TPrimitiveType::MAP: {
        TypeDescriptor map_type(PrimitiveType::TYPE_MAP);
        const rapidjson::Value& childColumns = column_schema["childColumns"];
        // The default type of AVRO MAP structure key is STRING
        map_type.add_sub_type(PrimitiveType::TYPE_STRING);
        map_type.add_sub_type(convert_to_doris_type(childColumns[1]));
        return map_type;
    }
    case TPrimitiveType::STRUCT: {
        TypeDescriptor struct_type(PrimitiveType::TYPE_STRUCT);
        const rapidjson::Value& childColumns = column_schema["childColumns"];
        for (auto i = 0; i < childColumns.Size(); i++) {
            const rapidjson::Value& child = childColumns[i];
            struct_type.add_sub_type(convert_to_doris_type(childColumns[i]),
                                     std::string(child["name"].GetString()));
        }
        return struct_type;
    }
    default:
        return {PrimitiveType::INVALID_TYPE};
    }
}

}