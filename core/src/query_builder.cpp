#include "fileengine/query_builder.h"
#include <algorithm>
#include <cctype>

namespace fileengine {

QueryBuilder& QueryBuilder::select(const std::string& columns) {
    operation_ = Operation::SELECT;
    columns_.clear();
    columns_.push_back(columns);
    return *this;
}

QueryBuilder& QueryBuilder::select(const std::vector<std::string>& columns) {
    operation_ = Operation::SELECT;
    columns_ = columns;
    return *this;
}

QueryBuilder& QueryBuilder::from(const std::string& table) {
    table_ = escape_identifier(table);
    return *this;
}

QueryBuilder& QueryBuilder::insert_into(const std::string& table) {
    operation_ = Operation::INSERT;
    table_ = escape_identifier(table);
    return *this;
}

QueryBuilder& QueryBuilder::insert_columns(const std::vector<std::string>& columns) {
    insert_columns_ = columns;
    for (auto& col : insert_columns_) {
        col = escape_identifier(col);
    }
    return *this;
}

QueryBuilder& QueryBuilder::values(const std::vector<std::string>& values) {
    insert_values_ = values;
    return *this;
}

QueryBuilder& QueryBuilder::update(const std::string& table) {
    operation_ = Operation::UPDATE;
    table_ = escape_identifier(table);
    return *this;
}

QueryBuilder& QueryBuilder::set(const std::string& column, const std::string& value) {
    set_values_[escape_identifier(column)] = value;
    return *this;
}

QueryBuilder& QueryBuilder::set(const std::map<std::string, std::string>& values) {
    for (const auto& pair : values) {
        set_values_[escape_identifier(pair.first)] = pair.second;
    }
    return *this;
}

QueryBuilder& QueryBuilder::where(const std::string& column, const std::string& value, ConditionType type) {
    Condition cond;
    cond.column = escape_identifier(column);
    cond.value = sanitize_value(value);
    cond.type = type;
    cond.logical_op = conditions_.empty() ? "" : "AND";
    conditions_.push_back(cond);
    return *this;
}

QueryBuilder& QueryBuilder::and_where(const std::string& column, const std::string& value, ConditionType type) {
    Condition cond;
    cond.column = escape_identifier(column);
    cond.value = sanitize_value(value);
    cond.type = type;
    cond.logical_op = "AND";
    conditions_.push_back(cond);
    return *this;
}

QueryBuilder& QueryBuilder::or_where(const std::string& column, const std::string& value, ConditionType type) {
    Condition cond;
    cond.column = escape_identifier(column);
    cond.value = sanitize_value(value);
    cond.type = type;
    cond.logical_op = "OR";
    conditions_.push_back(cond);
    return *this;
}

QueryBuilder& QueryBuilder::order_by(const std::string& column, bool ascending) {
    order_column_ = escape_identifier(column);
    order_ascending_ = ascending;
    has_order_ = true;
    return *this;
}

QueryBuilder& QueryBuilder::limit(int count) {
    limit_ = count;
    has_limit_ = true;
    return *this;
}

QueryBuilder& QueryBuilder::offset(int count) {
    offset_ = count;
    has_offset_ = true;
    return *this;
}

std::string QueryBuilder::build() const {
    std::ostringstream query;
    
    switch (operation_) {
        case Operation::SELECT: {
            query << "SELECT ";
            if (columns_.empty()) {
                query << "*";
            } else {
                for (size_t i = 0; i < columns_.size(); ++i) {
                    if (i > 0) query << ", ";
                    query << escape_identifier(columns_[i]);
                }
            }
            query << " FROM " << table_;
            break;
        }
        case Operation::INSERT: {
            query << "INSERT INTO " << table_ << " (";
            for (size_t i = 0; i < insert_columns_.size(); ++i) {
                if (i > 0) query << ", ";
                query << insert_columns_[i];
            }
            query << ") VALUES (";
            for (size_t i = 0; i < insert_values_.size(); ++i) {
                if (i > 0) query << ", ";
                query << "'" << sanitize_value(insert_values_[i]) << "'";
            }
            query << ")";
            break;
        }
        case Operation::UPDATE: {
            query << "UPDATE " << table_ << " SET ";
            size_t i = 0;
            for (const auto& pair : set_values_) {
                if (i > 0) query << ", ";
                query << pair.first << " = '" << sanitize_value(pair.second) << "'";
                ++i;
            }
            break;
        }
        case Operation::DELETE: {
            query << "DELETE FROM " << table_;
            break;
        }
    }
    
    // Add WHERE clause if there are conditions
    if (!conditions_.empty()) {
        query << " WHERE ";
        for (size_t i = 0; i < conditions_.size(); ++i) {
            const auto& cond = conditions_[i];
            if (i > 0) {
                query << " " << cond.logical_op << " ";
            }
            query << cond.column << " " << condition_type_to_string(cond.type) << " '" << cond.value << "'";
        }
    }
    
    // Add ORDER BY clause
    if (has_order_) {
        query << " ORDER BY " << order_column_ << " " << (order_ascending_ ? "ASC" : "DESC");
    }
    
    // Add LIMIT and OFFSET clauses
    if (has_limit_) {
        query << " LIMIT " << limit_;
        if (has_offset_) {
            query << " OFFSET " << offset_;
        }
    }
    
    return query.str();
}

std::vector<std::string> QueryBuilder::get_params() const {
    std::vector<std::string> params;
    
    // Extract parameters from conditions
    for (const auto& cond : conditions_) {
        params.push_back(cond.value);
    }
    
    // Extract parameters from INSERT values
    for (const auto& value : insert_values_) {
        params.push_back(sanitize_value(value));
    }
    
    // Extract parameters from SET values
    for (const auto& pair : set_values_) {
        params.push_back(sanitize_value(pair.second));
    }
    
    return params;
}

std::string QueryBuilder::build_with_params() const {
    std::ostringstream query;
    int param_count = 1; // PostgreSQL uses $1, $2, etc. for parameterized queries
    
    switch (operation_) {
        case Operation::SELECT: {
            query << "SELECT ";
            if (columns_.empty()) {
                query << "*";
            } else {
                for (size_t i = 0; i < columns_.size(); ++i) {
                    if (i > 0) query << ", ";
                    query << escape_identifier(columns_[i]);
                }
            }
            query << " FROM " << table_;
            break;
        }
        case Operation::INSERT: {
            query << "INSERT INTO " << table_ << " (";
            for (size_t i = 0; i < insert_columns_.size(); ++i) {
                if (i > 0) query << ", ";
                query << insert_columns_[i];
            }
            query << ") VALUES (";
            for (size_t i = 0; i < insert_values_.size(); ++i) {
                if (i > 0) query << ", ";
                query << "$" << param_count++;
            }
            query << ")";
            break;
        }
        case Operation::UPDATE: {
            query << "UPDATE " << table_ << " SET ";
            size_t i = 0;
            for (const auto& pair : set_values_) {
                if (i > 0) query << ", ";
                query << pair.first << " = $" << param_count++;
                ++i;
            }
            break;
        }
        case Operation::DELETE: {
            query << "DELETE FROM " << table_;
            break;
        }
    }
    
    // Add WHERE clause if there are conditions
    if (!conditions_.empty()) {
        query << " WHERE ";
        for (size_t i = 0; i < conditions_.size(); ++i) {
            const auto& cond = conditions_[i];
            if (i > 0) {
                query << " " << cond.logical_op << " ";
            }
            query << cond.column << " " << condition_type_to_string(cond.type) << " $" << param_count++;
        }
    }
    
    // Add ORDER BY clause
    if (has_order_) {
        query << " ORDER BY " << order_column_ << " " << (order_ascending_ ? "ASC" : "DESC");
    }
    
    // Add LIMIT and OFFSET clauses
    if (has_limit_) {
        query << " LIMIT " << limit_;
        if (has_offset_) {
            query << " OFFSET " << offset_;
        }
    }
    
    return query.str();
}

std::string QueryBuilder::condition_type_to_string(ConditionType type) const {
    switch (type) {
        case ConditionType::EQUAL: return "=";
        case ConditionType::NOT_EQUAL: return "!=";
        case ConditionType::GREATER_THAN: return ">";
        case ConditionType::LESS_THAN: return "<";
        case ConditionType::GREATER_EQUAL: return ">=";
        case ConditionType::LESS_EQUAL: return "<=";
        case ConditionType::LIKE: return "LIKE";
        case ConditionType::IN: return "IN";
    }
    return "=";
}

std::string QueryBuilder::sanitize_value(const std::string& value) const {
    // Remove potentially dangerous characters/sequences
    std::string sanitized = value;
    
    // Replace single quotes with two single quotes (PostgreSQL escaping)
    size_t pos = 0;
    while ((pos = sanitized.find("'", pos)) != std::string::npos) {
        sanitized.replace(pos, 1, "''");
        pos += 2; // Move past the replacement
    }
    
    // Additional sanitization could go here
    return sanitized;
}

std::string QueryBuilder::escape_identifier(const std::string& identifier) const {
    // For PostgreSQL, we use double quotes for identifiers
    std::string escaped = identifier;
    
    // First, replace any existing double quotes with escaped double quotes
    size_t pos = 0;
    while ((pos = escaped.find("\"", pos)) != std::string::npos) {
        escaped.replace(pos, 1, "\"\"");
        pos += 2;
    }
    
    // Wrap in double quotes
    return "\"" + escaped + "\"";
}

} // namespace fileengine