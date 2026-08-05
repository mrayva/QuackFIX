#pragma once

#include "duckdb/common/types/value.hpp"
#include "parser/fix_message.hpp"
#include "dictionary/fix_dictionary.hpp"
#include <string>

namespace duckdb {

// Parser for FIX repeating groups
// Extracts repeating group instances from ordered tag list using dictionary definitions
class FixGroupParser {
public:
	// Parse all groups for a message and return MAP value
	// Returns NULL if no groups found or groups not needed
	// Any truncated/invalid group counts are appended to `errors` rather than silently dropped
	static Value ParseGroups(const ParsedFixMessage &parsed, const FixDictionary &dict, bool needs_groups,
	                         std::vector<std::string> &errors);

private:
	// Parse instances of a single group
	static vector<Value> ParseGroupInstances(const std::vector<std::pair<int, ParsedFixMessage::TagValue>> &ordered_tags,
	                                         size_t start_pos, int group_count, const FixGroupDef &group_def);

	// Skip over one occurrence of a (possibly nested) group's entire tag span, recursing into
	// any subgroups it contains. Used when a subgroup is encountered while parsing its parent
	// group, so the parent's remaining instances - and any data after the group - don't get
	// misaligned by the subgroup's tags. Nested group content is not currently exposed in the
	// `groups` column (that would need a recursive output type); this only prevents corruption.
	static size_t SkipGroupSpan(const std::vector<std::pair<int, ParsedFixMessage::TagValue>> &ordered_tags,
	                            size_t pos, const FixGroupDef &group_def);

	// Check if a tag belongs to a group's field list
	static bool IsGroupField(int tag, const std::vector<int> &group_field_tags);

	// Get group count from other_tags map
	static int GetGroupCount(const std::unordered_map<int, ParsedFixMessage::TagValue> &other_tags, int count_tag,
	                         std::vector<std::string> &errors);

	// Find position of count tag in ordered list
	static size_t FindCountTagPosition(const std::vector<std::pair<int, ParsedFixMessage::TagValue>> &ordered_tags,
	                                   int count_tag);
};

} // namespace duckdb
