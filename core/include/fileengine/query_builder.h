// Copyright (C) 2026 James Hickman
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <string>
#include <vector>
#include <map>
#include <sstream>

namespace fileengine {

class QueryBuilder {
public:
    enum class Operation {
        SELECT,
        INSERT,
        UPDATE,
        DELETE
    };
    
    enum class ConditionType {
        EQUAL,
        NOT_EQUAL,
        GREATER_THAN,
        LESS_THAN,
        GREATER_EQUAL,
        LESS_EQUAL,
        LIKE,
        IN
    };

    QueryBuilder& select(const std::string& columns);
    QueryBuilder& select(const std::vector<std::string>& columns);
    QueryBuilder& from(const std::string& table);
    QueryBuilder& insert_into(const std::string& table);
    QueryBuilder& insert_columns(const std::vector<std::string>& columns);
    QueryBuilder& values(const std::vector<std::string>& values);
    QueryBuilder& update(const std::string& table);
    QueryBuilder& set(const std::string& column, const std::string& value);
    QueryBuilder& set(const std::map<std::string, std::string>& values);
    QueryBuilder& where(const std::string& column, const std::string& value, ConditionType type = ConditionType::EQUAL);
    QueryBuilder& and_where(const std::string& column, const std::string& value, ConditionType type = ConditionType::EQUAL);
    QueryBuilder& or_where(const std::string& column, const std::string& value, ConditionType type = ConditionType::EQUAL);
    QueryBuilder& order_by(const std::string& column, bool ascending = true);
    QueryBuilder& limit(int count);
    QueryBuilder& offset(int count);
    
    std::string build() const;
    std::vector<std::string> get_params() const;
    std::string build_with_params() const; // Builds query with placeholders for prepared statements

private:
    Operation operation_;
    std::vector<std::string> columns_;
    std::string table_;
    std::map<std::string, std::string> set_values_;
    std::vector<std::string> insert_columns_;
    std::vector<std::string> insert_values_;
    struct Condition {
        std::string column;
        std::string value;
        ConditionType type;
        std::string logical_op; // "AND" or "OR"
    };
    std::vector<Condition> conditions_;
    std::string order_column_;
    bool order_ascending_;
    int limit_;
    int offset_;
    bool has_limit_;
    bool has_offset_;
    bool has_order_;
    
    std::string condition_type_to_string(ConditionType type) const;
    std::string sanitize_value(const std::string& value) const;
    std::string escape_identifier(const std::string& identifier) const;
};

} // namespace fileengine