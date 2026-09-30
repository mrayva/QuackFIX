#include "fix_group_parser.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types.hpp"
#include <algorithm>
#include <limits>
#include <unordered_set>
#include <string>

namespace duckdb {

namespace {
std::string JsonEscape(const std::string &input) {
	std::string output;
	output.reserve(input.size() + 2);
	for (unsigned char c : input) {
		switch (c) {
		case '"':
			output += "\\\"";
			break;
		case '\\':
			output += "\\\\";
			break;
		case '\b':
			output += "\\b";
			break;
		case '\f':
			output += "\\f";
			break;
		case '\n':
			output += "\\n";
			break;
		case '\r':
			output += "\\r";
			break;
		case '\t':
			output += "\\t";
			break;
		default:
			if (c < 0x20) {
				static const char hex[] = "0123456789abcdef";
				output += "\\u00";
				output += hex[c >> 4];
				output += hex[c & 0x0f];
			} else {
				output += static_cast<char>(c);
			}
		}
	}
	return output;
}

bool TryParseCount(const ParsedFixMessage::TagValue &value, int &count) {
	if (!value.data || value.len == 0) {
		return false;
	}
	count = 0;
	for (size_t i = 0; i < value.len; i++) {
		char c = value.data[i];
		if (c < '0' || c > '9' || count > (std::numeric_limits<int>::max() - (c - '0')) / 10) {
			return false;
		}
		count = count * 10 + (c - '0');
	}
	return true;
}

int ParseCount(const ParsedFixMessage::TagValue &value) {
	int count = 0;
	return TryParseCount(value, count) ? count : 0;
}

bool ValidateGroupInstances(const std::vector<std::pair<int, ParsedFixMessage::TagValue>> &tags, size_t &pos,
                            int count, const FixGroupDef &definition, unsigned depth, std::string &error) {
	if (depth > 32) {
		error = "Repeating groups exceed the maximum nesting depth";
		return false;
	}
	if (count > 0 && definition.field_tags.empty()) {
		error = "Repeating group has no fields in its dictionary definition";
		return false;
	}
	const int delimiter_tag = definition.field_tags.empty() ? 0 : definition.field_tags.front();
	for (int instance = 0; instance < count; instance++) {
		if (pos >= tags.size() || tags[pos].first != delimiter_tag) {
			error = "Repeating group count tag " + std::to_string(definition.count_tag) + " declares " +
			        std::to_string(count) + " instances, but delimiter tag " + std::to_string(delimiter_tag) +
			        " is missing for instance " + std::to_string(instance + 1);
			return false;
		}
		bool consumed_field = false;
		while (pos < tags.size()) {
			const int tag = tags[pos].first;
			if (consumed_field && tag == delimiter_tag) {
				break;
			}
			auto subgroup = definition.subgroups.find(tag);
			if (subgroup != definition.subgroups.end()) {
				int subgroup_count = 0;
				if (!TryParseCount(tags[pos].second, subgroup_count)) {
					error = "Invalid repeating group count in tag " + std::to_string(tag) +
					        ": expected a non-negative integer";
					return false;
				}
				pos++;
				if (!ValidateGroupInstances(tags, pos, subgroup_count, *subgroup->second, depth + 1, error)) {
					return false;
				}
				consumed_field = true;
				continue;
			}
			if (std::find(definition.field_tags.begin(), definition.field_tags.end(), tag) == definition.field_tags.end()) {
				break;
			}
			pos++;
			consumed_field = true;
		}
	}
	if (count == 0 && pos < tags.size() && tags[pos].first == delimiter_tag) {
		error = "Repeating group count tag " + std::to_string(definition.count_tag) +
		        " declares zero instances, but delimiter tag " + std::to_string(delimiter_tag) + " is present";
		return false;
	}
	if (count > 0 && pos < tags.size() && tags[pos].first == delimiter_tag) {
		error = "Repeating group count tag " + std::to_string(definition.count_tag) + " declares " +
		        std::to_string(count) + " instances, but more instances start with delimiter tag " +
		        std::to_string(delimiter_tag);
		return false;
	}
	return true;
}

std::string ParseJsonInstances(const std::vector<std::pair<int, ParsedFixMessage::TagValue>> &tags, size_t &pos,
                               int count, const FixGroupDef &definition, unsigned depth) {
	if (depth > 32 || definition.field_tags.empty()) {
		return "[]";
	}
	std::string instances = "[";
	bool first_instance = true;
	for (int instance = 0; instance < count && pos < tags.size(); instance++) {
		if (tags[pos].first != definition.field_tags.front()) {
			break;
		}
		if (!first_instance) {
			instances += ',';
		}
		first_instance = false;
		std::string fields = "{";
		std::string groups = "{";
		bool first_field = true;
		bool first_group = true;
		bool have_content = false;
		while (pos < tags.size()) {
			const int tag = tags[pos].first;
			if (have_content && tag == definition.field_tags.front()) {
				break;
			}
			auto subgroup = definition.subgroups.find(tag);
			if (subgroup != definition.subgroups.end()) {
				const int subgroup_count = ParseCount(tags[pos].second);
				pos++;
				std::string subgroup_instances = ParseJsonInstances(tags, pos, subgroup_count, *subgroup->second, depth + 1);
				if (!first_group) {
					groups += ',';
				}
				first_group = false;
				groups += "\"" + std::to_string(tag) + "\":" + subgroup_instances;
				have_content = true;
				continue;
			}
			if (std::find(definition.field_tags.begin(), definition.field_tags.end(), tag) == definition.field_tags.end()) {
				break;
			}
			if (!first_field) {
				fields += ',';
			}
			first_field = false;
			const auto &value = tags[pos].second;
			fields += "\"" + std::to_string(tag) + "\":\"" +
			          JsonEscape(std::string(value.data, value.len)) + "\"";
			pos++;
			have_content = true;
		}
		fields += '}';
		groups += '}';
		instances += "{\"fields\":" + fields + ",\"groups\":" + groups + "}";
	}
	instances += ']';
	return instances;
}
} // namespace

bool FixGroupParser::IsGroupField(int tag, const std::vector<int> &group_field_tags) {
	for (int gf : group_field_tags) {
		if (tag == gf) {
			return true;
		}
	}
	return false;
}

int FixGroupParser::GetGroupCount(const std::unordered_map<int, ParsedFixMessage::TagValue> &other_tags,
                                  int count_tag) {
	auto tag_it = other_tags.find(count_tag);
	if (tag_it == other_tags.end()) {
		return 0; // Group not present
	}
	return ParseCount(tag_it->second);
}

size_t FixGroupParser::FindCountTagPosition(const std::vector<std::pair<int, ParsedFixMessage::TagValue>> &ordered_tags,
                                            int count_tag) {
	for (size_t i = 0; i < ordered_tags.size(); i++) {
		if (ordered_tags[i].first == count_tag) {
			return i;
		}
	}
	return ordered_tags.size(); // Not found
}

vector<Value>
FixGroupParser::ParseGroupInstances(const std::vector<std::pair<int, ParsedFixMessage::TagValue>> &ordered_tags,
                                    size_t start_pos, int group_count, const std::vector<int> &group_field_tags) {
	vector<Value> group_instances;
	size_t pos = start_pos;

	for (int instance = 0; instance < group_count && pos < ordered_tags.size(); instance++) {
		// Parse one group instance
		vector<Value> instance_map_entries;

		// Collect tags that belong to this group instance
		while (pos < ordered_tags.size()) {
			int tag = ordered_tags[pos].first;

			// The first declared field is the delimiter that starts every instance.
			if (!instance_map_entries.empty() && tag == group_field_tags.front()) {
				break;
			}
			if (!IsGroupField(tag, group_field_tags)) {
				break;
			}

			// Add this tag to the instance
			auto &tag_value = ordered_tags[pos].second;
			child_list_t<Value> map_entry;
			map_entry.push_back(make_pair("key", Value::INTEGER(tag)));
			map_entry.push_back(make_pair("value", Value(std::string(tag_value.data, tag_value.len))));
			instance_map_entries.push_back(Value::STRUCT(map_entry));

			pos++;
		}

		if (instance_map_entries.empty()) {
			break;
		}
		// Create MAP for this instance
		auto instance_map_type = LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR);
		auto instance_child_type = ListType::GetChildType(instance_map_type);
		group_instances.push_back(Value::MAP(instance_child_type, instance_map_entries));
	}

	return group_instances;
}

Value FixGroupParser::ParseGroups(const ParsedFixMessage &parsed, const FixDictionary &dict, bool needs_groups) {
	// Early exit optimization - groups not requested
	if (!needs_groups) {
		return Value(); // NULL
	}

	// Validate prerequisites
	if (parsed.all_tags_ordered.empty() || parsed.msg_type == nullptr || parsed.msg_type_len == 0) {
		return Value(); // NULL
	}

	// Look up message type in dictionary
	std::string msg_type_str(parsed.msg_type, parsed.msg_type_len);
	auto msg_it = dict.messages.find(msg_type_str);

	if (msg_it == dict.messages.end()) {
		// Message type not in dictionary - no groups to parse
		return Value();
	}

	const auto &message_def = msg_it->second;
	vector<Value> outer_map_entries;

	// Iterate through all groups defined for this message type
	for (const auto &[count_tag, group_def] : message_def.groups) {
		// Check if this group exists in the message
		int group_count = GetGroupCount(parsed.other_tags, count_tag);
		if (group_count == 0) {
			continue; // Group not present or invalid count
		}

		// Get field tags from dictionary (dereference shared_ptr)
		std::vector<int> group_field_tags;
		std::unordered_set<int> seen_fields;
		// The current SQL value stores each group instance as MAP(INTEGER, VARCHAR),
		// so collect descendant tags too; their values remain available in that flat map.
		auto collect_fields = [&](auto &&self, const FixGroupDef &definition) -> void {
			for (int tag : definition.field_tags) {
				if (seen_fields.insert(tag).second) {
					group_field_tags.push_back(tag);
				}
			}
			for (const auto &subgroup : definition.subgroups) {
				if (seen_fields.insert(subgroup.first).second) {
					group_field_tags.push_back(subgroup.first);
				}
				self(self, *subgroup.second);
			}
		};
		collect_fields(collect_fields, *group_def);
		if (group_field_tags.empty()) {
			continue; // No fields defined for this group
		}

		// Find the position of the count tag in ordered list
		size_t count_tag_pos = FindCountTagPosition(parsed.all_tags_ordered, count_tag);
		if (count_tag_pos >= parsed.all_tags_ordered.size()) {
			continue; // Count tag not found in ordered list
		}

		// Parse group instances from ordered tags starting after count tag
		auto group_instances =
		    ParseGroupInstances(parsed.all_tags_ordered, count_tag_pos + 1, group_count, group_field_tags);

		if (!group_instances.empty()) {
			// Create outer map entry for this group
			child_list_t<Value> outer_struct;
			outer_struct.push_back(make_pair("key", Value::INTEGER(count_tag)));

			// Create LIST value with the correct child type
			auto instance_map_type = LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR);
			outer_struct.push_back(make_pair("value", Value::LIST(instance_map_type, group_instances)));
			outer_map_entries.push_back(Value::STRUCT(outer_struct));
		}
	}

	if (outer_map_entries.empty()) {
		return Value(); // NULL if no groups found
	}

	// Create final nested MAP
	auto outer_map_type = LogicalType::MAP(
	    LogicalType::INTEGER, LogicalType::LIST(LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR)));
	auto outer_child_type = ListType::GetChildType(outer_map_type);
	return Value::MAP(outer_child_type, outer_map_entries);
}

Value FixGroupParser::ParseGroupsJson(const ParsedFixMessage &parsed, const FixDictionary &dict) {
	if (parsed.all_tags_ordered.empty() || parsed.msg_type == nullptr || parsed.msg_type_len == 0) {
		return Value();
	}
	auto message = dict.messages.find(std::string(parsed.msg_type, parsed.msg_type_len));
	if (message == dict.messages.end()) {
		return Value();
	}
	std::vector<std::pair<int, std::shared_ptr<FixGroupDef>>> definitions(message->second.groups.begin(),
	                                                                     message->second.groups.end());
	std::sort(definitions.begin(), definitions.end(), [](const auto &left, const auto &right) {
		return left.first < right.first;
	});
	std::string output = "{";
	bool first_group = true;
	for (const auto &entry : definitions) {
		int count = GetGroupCount(parsed.other_tags, entry.first);
		if (count <= 0) {
			continue;
		}
		size_t count_position = FindCountTagPosition(parsed.all_tags_ordered, entry.first);
		if (count_position >= parsed.all_tags_ordered.size()) {
			continue;
		}
		size_t pos = count_position + 1;
		std::string instances = ParseJsonInstances(parsed.all_tags_ordered, pos, count, *entry.second, 0);
		if (instances == "[]") {
			continue;
		}
		if (!first_group) {
			output += ',';
		}
		first_group = false;
		output += "\"" + std::to_string(entry.first) + "\":" + instances;
	}
	output += '}';
	if (first_group) {
		return Value();
	}
	return Value(output);
}

bool FixGroupParser::ValidateCounts(const ParsedFixMessage &parsed, const FixDictionary &dict, std::string &error) {
	if (!parsed.msg_type || parsed.msg_type_len == 0) {
		return true;
	}
	auto message = dict.messages.find(std::string(parsed.msg_type, parsed.msg_type_len));
	if (message == dict.messages.end()) {
		return true;
	}
	std::vector<std::pair<int, std::shared_ptr<FixGroupDef>>> definitions(message->second.groups.begin(),
	                                                                     message->second.groups.end());
	std::sort(definitions.begin(), definitions.end(), [](const auto &left, const auto &right) {
		return left.first < right.first;
	});
	for (const auto &group : definitions) {
		size_t count_position = FindCountTagPosition(parsed.all_tags_ordered, group.first);
		if (count_position >= parsed.all_tags_ordered.size()) {
			continue;
		}
		int count = 0;
		if (!TryParseCount(parsed.all_tags_ordered[count_position].second, count)) {
			error = "Invalid repeating group count in tag " + std::to_string(group.first) +
			        ": expected a non-negative integer";
			return false;
		}
		auto duplicate_count = std::find_if(parsed.all_tags_ordered.begin() + count_position + 1,
		                                   parsed.all_tags_ordered.end(),
		                                   [&](const auto &tag) { return tag.first == group.first; });
		if (duplicate_count != parsed.all_tags_ordered.end()) {
			error = "Duplicate top-level repeating group count tag " + std::to_string(group.first);
			return false;
		}
		size_t pos = count_position + 1;
		if (!ValidateGroupInstances(parsed.all_tags_ordered, pos, count, *group.second, 0, error)) {
			return false;
		}
	}
	return true;
}

} // namespace duckdb
