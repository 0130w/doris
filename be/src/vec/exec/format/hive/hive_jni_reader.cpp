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

#include "hive_jni_reader.h"

#include <map>
#include <ostream>

#include "runtime/descriptors.h"
#include "runtime/types.h"
#include "common/logging.h"

namespace doris::vectorized {

HiveJNIReader::HiveJNIReader(RuntimeState* state, RuntimeProfile* profile,
                             const TFileScanRangeParams& params,
                             const std::vector<SlotDescriptor*>& file_slot_descs,
                             const TFileRangeDesc& range)
        : JniReader(file_slot_descs, state, profile), _params(params), _range(range) {
        }

HiveJNIReader::HiveJNIReader(RuntimeProfile* profile, const TFileScanRangeParams& params,
              const TFileRangeDesc& range, const std::vector<SlotDescriptor*>& file_slot_descs)
    : JniReader(file_slot_descs, nullptr, profile), _params(params), _range(range) {
    }

HiveJNIReader::~HiveJNIReader() = default;

TFileType::type HiveJNIReader::get_file_type() {
    TFileType::type type;
    if (_range.__isset.file_type) {
        type = _range.file_type;
    } else {
        type = _params.file_type;
    }
    return type;
}

Status HiveJNIReader::init_fetch_table_reader(
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
            {"file_type", std::to_string(type)},
            {"is_get_table_schema", "false"},
            {"file_format", std::to_string(_params.format_type)},
            {"columns_names", "col_tinyint,col_smallint,col_int,col_bigint,col_float,col_double,col_decimal,col_string,col_char,col_varchar,col_boolean,col_timestamp,col_date,col_array,col_map,col_struct"},
            {"columns_types", "tinyint#smallint#int#bigint#float#double#decimal(10,2)#string#char(10)#varchar(20)#boolean#timestamp#date#array<string>#map<string,int>#struct<name:string,age:int>"},
            {"required_fields", "col_tinyint,col_smallint,col_int,col_bigint,col_float,col_double,col_decimal,col_string,col_char,col_varchar,col_boolean,col_timestamp,col_date,col_array,col_map,col_struct"},
            {"split_start_offset", std::to_string(_range.start_offset)},
            {"split_size", std::to_string(_range.size)}
    };
    if (type == TFileType::FILE_S3) {
        required_params.insert(_params.properties.begin(), _params.properties.end());
    }
    _jni_connector = std::make_unique<JniConnector>("org/apache/doris/hive/HiveJNIScanner",
                                                    required_params, column_names);
    RETURN_IF_ERROR(_jni_connector->init(_colname_to_value_range));
    return _jni_connector->open(_state, _profile);
}

Status HiveJNIReader::init_fetch_table_schema_reader() {
    std::map<String, String> required_params = {
        {"uri", _range.path},
        {"file_type", std::to_string(get_file_type())},
        {"file_format", std::to_string(_params.format_type)},
        {"is_get_table_schema", "true"},
        {"columns_names", "col_tinyint,col_smallint,col_int,col_bigint,col_float,col_double,col_decimal,col_string,col_char,col_varchar,col_boolean,col_timestamp,col_date,col_array,col_map,col_struct"},
        {"columns_types", "tinyint#smallint#int#bigint#float#double#decimal(10,2)#string#char(10)#varchar(20)#boolean#timestamp#date#array<string>#map<string,int>#struct<name:string,age:int>"},
        {"required_fields", "col_tinyint,col_smallint,col_int,col_bigint,col_float,col_double,col_decimal,col_string,col_char,col_varchar,col_boolean,col_timestamp,col_date,col_array,col_map,col_struct"},
    };
    required_params.insert(_params.properties.begin(), _params.properties.end());
    _jni_connector = std::make_unique<JniConnector>("org/apache/doris/hive/HiveJNIScanner",
                                                    required_params);
    return _jni_connector->open(nullptr, _profile);                                                    
}

Status HiveJNIReader::get_next_block(Block* block, size_t* read_rows, bool* eof) {
    RETURN_IF_ERROR(_jni_connector->get_next_block(block, read_rows, eof));
    if (*eof) {
        RETURN_IF_ERROR(_jni_connector->close());
    }
    return Status::OK();
}

Status HiveJNIReader::get_columns(std::unordered_map<std::string, TypeDescriptor>* name_to_type,
                                  std::unordered_set<std::string>* missing_cols) {
    for (auto& desc : _file_slot_descs) {
        name_to_type->emplace(desc->col_name(), desc->type());
    }
    return Status::OK();
}

Status HiveJNIReader::get_parsed_schema(std::vector<std::string>* col_names,
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

 TypeDescriptor HiveJNIReader::convert_to_doris_type(const rapidjson::Value& column_schema) {
    auto schema_type = static_cast< ::doris::TPrimitiveType::type>(column_schema["type"].GetInt());
    auto precision = column_schema["precision"].GetInt();
    auto scale = column_schema["scale"].GetInt();
    switch (schema_type) {
    case TPrimitiveType::TINYINT:
    case TPrimitiveType::INT:
    case TPrimitiveType::STRING:
    case TPrimitiveType::BIGINT:
    case TPrimitiveType::SMALLINT:
    case TPrimitiveType::LARGEINT:
    case TPrimitiveType::BOOLEAN:
    case TPrimitiveType::DOUBLE:
    case TPrimitiveType::FLOAT:
    case TPrimitiveType::BINARY:
    case TPrimitiveType::DATE:
    case TPrimitiveType::DATEV2:
    case TPrimitiveType::DATETIME:
    case TPrimitiveType::DATETIMEV2:
    case TPrimitiveType::CHAR:
    case TPrimitiveType::VARCHAR:
        return {thrift_to_type(schema_type)};
    case TPrimitiveType::DECIMALV2:
    case TPrimitiveType::DECIMAL32:
    case TPrimitiveType::DECIMAL64:
    case TPrimitiveType::DECIMAL128I:
        return TypeDescriptor::create_decimalv3_type(precision, scale);
    case TPrimitiveType::ARRAY: {
        TypeDescriptor list_type(PrimitiveType::TYPE_ARRAY);
        const rapidjson::Value& childColumns = column_schema["childColumns"];
        list_type.add_sub_type(convert_to_doris_type(childColumns[0]));
        return list_type;
    }
    case TPrimitiveType::MAP: {
        TypeDescriptor map_type(PrimitiveType::TYPE_MAP);
        const rapidjson::Value& childColumns = column_schema["childColumns"];
        // The default type of MAP structure key is STRING
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