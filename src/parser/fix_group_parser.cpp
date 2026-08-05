#include "fix_group_parser.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types.hpp"
#include <algorithm>

namespace duckdb {

namespace {
// Sanity bound on repeating group counts - protects against corrupted/malicious
// NoXXX count fields causing runaway parsing.
constexpr int kMaxGroupCount = 100;
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
                                  int count_tag, std::vector<std::string> &errors) {
	auto tag_it = other_tags.find(count_tag);
	if (tag_it == other_tags.end()) {
		return 0; // Group not present
	}

	std::string count_str(tag_it->second.data, tag_it->second.len);
	try {
		int count = std::stoi(count_str);

		if (count <= 0) {
			return 0;
		}
		if (count > kMaxGroupCount) {
			errors.push_back("Group tag " + std::to_string(count_tag) + " count " + std::to_string(count) +
			                 " exceeds maximum of " + std::to_string(kMaxGroupCount) + "; group dropped");
			return 0;
		}

		return count;
	} catch (...) {
		errors.push_back("Group tag " + std::to_string(count_tag) + " has invalid count value: '" + count_str + "'");
		return 0; // Invalid count
	}
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

size_t FixGroupParser::SkipGroupSpan(const std::vector<std::pair<int, ParsedFixMessage::TagValue>> &ordered_tags,
                                     size_t pos, const FixGroupDef &group_def) {
	// pos points at the group's count tag. Its value must be read directly from this
	// occurrence in the ordered stream, not via other_tags, since a tag number repeated
	// across instances only keeps its last value in that map.
	int count = 0;
	if (pos < ordered_tags.size()) {
		try {
			const auto &count_value = ordered_tags[pos].second;
			count = std::stoi(std::string(count_value.data, count_value.len));
		} catch (...) {
			count = 0;
		}
	}
	pos++; // move past the count tag itself

	if (count <= 0 || count > kMaxGroupCount) {
		return pos;
	}

	for (int instance = 0; instance < count && pos < ordered_tags.size(); instance++) {
		bool consumed_any = false;

		while (pos < ordered_tags.size()) {
			int tag = ordered_tags[pos].first;

			auto sub_it = group_def.subgroups.find(tag);
			if (sub_it != group_def.subgroups.end()) {
				pos = SkipGroupSpan(ordered_tags, pos, *sub_it->second);
				consumed_any = true;
				continue;
			}

			if (!IsGroupField(tag, group_def.field_tags)) {
				break;
			}

			pos++;
			consumed_any = true;

			if (pos < ordered_tags.size() && !group_def.field_tags.empty() &&
			    ordered_tags[pos].first == group_def.field_tags[0]) {
				break;
			}
		}

		if (!consumed_any) {
			// Nothing recognized for this instance; avoid spinning without progress.
			break;
		}
	}

	return pos;
}

vector<Value>
FixGroupParser::ParseGroupInstances(const std::vector<std::pair<int, ParsedFixMessage::TagValue>> &ordered_tags,
                                    size_t start_pos, int group_count, const FixGroupDef &group_def) {
	vector<Value> group_instances;
	size_t pos = start_pos;
	const auto &group_field_tags = group_def.field_tags;

	for (int instance = 0; instance < group_count && pos < ordered_tags.size(); instance++) {
		// Parse one group instance
		vector<Value> instance_map_entries;

		// Collect tags that belong to this group instance
		while (pos < ordered_tags.size()) {
			int tag = ordered_tags[pos].first;

			// A nested subgroup within this instance - its content isn't exposed in the
			// `groups` column yet, but its span must still be skipped so later fields and
			// instances of the outer group don't get misaligned.
			auto sub_it = group_def.subgroups.find(tag);
			if (sub_it != group_def.subgroups.end()) {
				pos = SkipGroupSpan(ordered_tags, pos, *sub_it->second);
				continue;
			}

			// Check if this tag belongs to the current group
			if (!IsGroupField(tag, group_field_tags)) {
				// Not a group field - either another group starts or non-group tag
				break;
			}

			// Add this tag to the instance
			auto &tag_value = ordered_tags[pos].second;
			child_list_t<Value> map_entry;
			map_entry.push_back(make_pair("key", Value::INTEGER(tag)));
			map_entry.push_back(make_pair("value", Value(std::string(tag_value.data, tag_value.len))));
			instance_map_entries.push_back(Value::STRUCT(map_entry));

			pos++;

			// Check if we've seen the first field again (marks next instance)
			if (pos < ordered_tags.size() && !group_field_tags.empty() &&
			    ordered_tags[pos].first == group_field_tags[0]) {
				break;
			}
		}

		if (!instance_map_entries.empty()) {
			// Create MAP for this instance
			auto instance_map_type = LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR);
			auto instance_child_type = ListType::GetChildType(instance_map_type);
			group_instances.push_back(Value::MAP(instance_child_type, instance_map_entries));
		}
	}

	return group_instances;
}

Value FixGroupParser::ParseGroups(const ParsedFixMessage &parsed, const FixDictionary &dict, bool needs_groups,
                                  std::vector<std::string> &errors) {
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
		int group_count = GetGroupCount(parsed.other_tags, count_tag, errors);
		if (group_count == 0) {
			continue; // Group not present or invalid count
		}

		if (group_def->field_tags.empty()) {
			continue; // No fields defined for this group
		}

		// Find the position of the count tag in ordered list
		size_t count_tag_pos = FindCountTagPosition(parsed.all_tags_ordered, count_tag);
		if (count_tag_pos >= parsed.all_tags_ordered.size()) {
			continue; // Count tag not found in ordered list
		}

		// Parse group instances from ordered tags starting after count tag
		auto group_instances =
		    ParseGroupInstances(parsed.all_tags_ordered, count_tag_pos + 1, group_count, *group_def);

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

} // namespace duckdb
