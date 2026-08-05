#include "xml_loader.hpp"
#include "tinyxml2.h"
#include "duckdb/common/file_system.hpp"
#include <stdexcept>
#include <iostream>

FixDictionary FixDictionaryLoader::LoadFromString(const std::string &xml_content) {
	FixDictionary dict;
	tinyxml2::XMLDocument doc;

	// Parse XML from string
	if (doc.Parse(xml_content.c_str()) != tinyxml2::XML_SUCCESS) {
		throw std::runtime_error("Failed to parse dictionary XML from string");
	}

	auto *root = doc.RootElement();
	if (!root) {
		throw std::runtime_error("Invalid FIX dictionary XML: no root element.");
	}

	// ---------------------------
	// Load <fields>
	// ---------------------------
	auto *fields_root = root->FirstChildElement("fields");
	if (fields_root) {
		LoadFields(dict, fields_root);
	}

	// ---------------------------
	// Load <components> (BEFORE messages)
	// ---------------------------
	auto *components_root = root->FirstChildElement("components");
	if (components_root) {
		LoadComponents(dict, components_root);
	}

	// ---------------------------
	// Load <messages>
	// ---------------------------
	auto *messages_root = root->FirstChildElement("messages");
	if (messages_root) {
		LoadMessages(dict, messages_root);
	}

	return dict;
}

FixDictionary FixDictionaryLoader::LoadBase(duckdb::ClientContext &context, const std::string &path) {
	FixDictionary dict;
	tinyxml2::XMLDocument doc;

	// Phase 7.8: Use DuckDB FileSystem to support S3, HTTP, etc.
	auto &fs = duckdb::FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, duckdb::FileOpenFlags::FILE_FLAGS_READ);

	// Read entire file into string
	auto file_size = fs.GetFileSize(*handle);
	std::string xml_content;
	xml_content.resize(file_size);
	handle->Read((void *)xml_content.data(), file_size);

	// Parse XML from string
	if (doc.Parse(xml_content.c_str()) != tinyxml2::XML_SUCCESS) {
		throw std::runtime_error("Failed to parse dictionary XML from: " + path);
	}

	auto *root = doc.RootElement();
	if (!root) {
		throw std::runtime_error("Invalid FIX dictionary XML: no root element.");
	}

	// ---------------------------
	// Load <fields>
	// ---------------------------
	auto *fields_root = root->FirstChildElement("fields");
	if (fields_root) {
		LoadFields(dict, fields_root);
	}

	// ---------------------------
	// Load <components> (BEFORE messages)
	// ---------------------------
	auto *components_root = root->FirstChildElement("components");
	if (components_root) {
		LoadComponents(dict, components_root);
	}

	// ---------------------------
	// Load <messages>
	// ---------------------------
	auto *messages_root = root->FirstChildElement("messages");
	if (messages_root) {
		LoadMessages(dict, messages_root);
	}

	return dict;
}

// ===========================================================
// FIELD LOADING
// ===========================================================
void FixDictionaryLoader::LoadFields(FixDictionary &dict, tinyxml2::XMLElement *fields_root) {
	for (tinyxml2::XMLElement *field = fields_root->FirstChildElement("field"); field != nullptr;
	     field = field->NextSiblingElement("field")) {
		FixFieldDef def;

		int tag;
		if (field->QueryIntAttribute("number", &tag) != tinyxml2::XML_SUCCESS) {
			throw std::runtime_error("<field> element missing or invalid 'number' attribute");
		}
		def.tag = tag;

		const char *name = field->Attribute("name");
		if (!name) {
			throw std::runtime_error("<field number=\"" + std::to_string(tag) + "\"> missing 'name' attribute");
		}
		def.name = name;

		const char *type = field->Attribute("type");
		if (!type) {
			throw std::runtime_error("<field name=\"" + def.name + "\"> missing 'type' attribute");
		}
		def.type = type;

		dict.name_to_tag[def.name] = def.tag;

		// Enums
		for (tinyxml2::XMLElement *val = field->FirstChildElement("value"); val != nullptr;
		     val = val->NextSiblingElement("value")) {
			FixEnum e;
			const char *enum_value = val->Attribute("enum");
			if (!enum_value) {
				throw std::runtime_error("<value> under field '" + def.name + "' missing 'enum' attribute");
			}
			e.enum_value = enum_value;
			const char *description = val->Attribute("description");
			e.description = description ? description : "";
			def.enums.push_back(e);
		}

		dict.fields[def.tag] = def;
	}
}

// ===========================================================
// GROUP LOADER (recursive)
// ===========================================================
FixGroupDef FixDictionaryLoader::LoadGroup(FixDictionary &dict, tinyxml2::XMLElement *group) {
	FixGroupDef g;

	const char *group_name = group->Attribute("name");
	if (!group_name) {
		throw std::runtime_error("<group> node missing 'name' attribute");
	}

	auto count_it = dict.name_to_tag.find(group_name);
	if (count_it == dict.name_to_tag.end()) {
		throw std::runtime_error(std::string("<group name=\"") + group_name + "\"> references unknown field name");
	}
	g.count_tag = count_it->second;

	// group fields
	for (tinyxml2::XMLElement *f = group->FirstChildElement("field"); f != nullptr;
	     f = f->NextSiblingElement("field")) {
		const char *fname = f->Attribute("name");
		if (!fname) {
			throw std::runtime_error(std::string("<field> in group '") + group_name + "' missing 'name' attribute");
		}
		auto field_it = dict.name_to_tag.find(fname);
		if (field_it == dict.name_to_tag.end()) {
			throw std::runtime_error(std::string("Group '") + group_name + "' references unknown field name: " + fname);
		}
		g.field_tags.push_back(field_it->second);
	}

	// nested groups
	for (tinyxml2::XMLElement *sub = group->FirstChildElement("group"); sub != nullptr;
	     sub = sub->NextSiblingElement("group")) {
		FixGroupDef sub_def = LoadGroup(dict, sub);
		g.subgroups[sub_def.count_tag] = std::make_shared<FixGroupDef>(sub_def);
	}

	return g;
}

// ===========================================================
// COMPONENT LOADING
// ===========================================================
void FixDictionaryLoader::LoadComponents(FixDictionary &dict, tinyxml2::XMLElement *components_root) {
	for (tinyxml2::XMLElement *comp = components_root->FirstChildElement("component"); comp != nullptr;
	     comp = comp->NextSiblingElement("component")) {
		FixComponentDef c;
		const char *comp_name = comp->Attribute("name");
		if (!comp_name) {
			throw std::runtime_error("<component> element missing 'name' attribute");
		}
		c.name = comp_name;

		// fields in component
		for (tinyxml2::XMLElement *field = comp->FirstChildElement("field"); field != nullptr;
		     field = field->NextSiblingElement("field")) {
			const char *fname = field->Attribute("name");
			if (!fname) {
				throw std::runtime_error("<field> in component '" + c.name + "' missing 'name' attribute");
			}
			auto field_it = dict.name_to_tag.find(fname);
			if (field_it == dict.name_to_tag.end()) {
				throw std::runtime_error("Component '" + c.name + "' references unknown field name: " +
				                         std::string(fname));
			}
			c.field_tags.push_back(field_it->second);
		}

		// groups in component
		for (tinyxml2::XMLElement *group = comp->FirstChildElement("group"); group != nullptr;
		     group = group->NextSiblingElement("group")) {
			FixGroupDef g = LoadGroup(dict, group);
			c.groups[g.count_tag] = std::make_shared<FixGroupDef>(g);
		}

		dict.components[c.name] = c;
	}
}

// ===========================================================
// EXPAND COMPONENT REFERENCE INTO MESSAGE
// ===========================================================
void FixDictionaryLoader::ExpandComponent(FixDictionary &dict, FixMessageDef &msg, tinyxml2::XMLElement *comp_ref) {
	const char *comp_name = comp_ref->Attribute("name");
	if (!comp_name) {
		return;
	}

	auto it = dict.components.find(comp_name);
	if (it == dict.components.end()) {
		return;
	}

	const FixComponentDef &comp = it->second;

	// Add component's fields to message
	bool required = comp_ref->Attribute("required") && strcmp(comp_ref->Attribute("required"), "Y") == 0;
	for (int tag : comp.field_tags) {
		if (required) {
			msg.required_fields.push_back(tag);
		} else {
			msg.optional_fields.push_back(tag);
		}
	}

	// Add component's groups to message with deterministic ordering
	// Sort groups by count_tag before adding to ensure consistent behavior across architectures
	std::vector<std::pair<int, std::shared_ptr<FixGroupDef>>> sorted_groups(comp.groups.begin(), comp.groups.end());
	std::sort(sorted_groups.begin(), sorted_groups.end(),
	          [](const auto &a, const auto &b) { return a.first < b.first; });

	for (const auto &[count_tag, group_def] : sorted_groups) {
		msg.groups[count_tag] = group_def;
	}
}

// ===========================================================
// MESSAGE LOADING
// ===========================================================
void FixDictionaryLoader::LoadMessages(FixDictionary &dict, tinyxml2::XMLElement *messages_root) {

	for (tinyxml2::XMLElement *msg = messages_root->FirstChildElement("message"); msg != nullptr;
	     msg = msg->NextSiblingElement("message")) {
		FixMessageDef m;
		const char *msg_name = msg->Attribute("name");
		if (!msg_name) {
			throw std::runtime_error("<message> element missing 'name' attribute");
		}
		m.name = msg_name;

		const char *msg_type = msg->Attribute("msgtype");
		if (!msg_type) {
			throw std::runtime_error("<message name=\"" + m.name + "\"> missing 'msgtype' attribute");
		}
		m.msg_type = msg_type;

		// Iterate through all child elements in order
		for (tinyxml2::XMLElement *child = msg->FirstChildElement(); child != nullptr;
		     child = child->NextSiblingElement()) {
			const char *child_name = child->Name();

			if (strcmp(child_name, "field") == 0) {
				// Direct field
				const char *fname = child->Attribute("name");
				if (!fname) {
					throw std::runtime_error("<field> in message '" + m.name + "' missing 'name' attribute");
				}
				bool required = child->Attribute("required") && strcmp(child->Attribute("required"), "Y") == 0;

				auto field_it = dict.name_to_tag.find(fname);
				if (field_it == dict.name_to_tag.end()) {
					throw std::runtime_error("Message '" + m.name + "' references unknown field name: " +
					                         std::string(fname));
				}
				int tag = field_it->second;
				if (required) {
					m.required_fields.push_back(tag);
				} else {
					m.optional_fields.push_back(tag);
				}
			} else if (strcmp(child_name, "group") == 0) {
				// Direct group
				FixGroupDef g = LoadGroup(dict, child);
				m.groups[g.count_tag] = std::make_shared<FixGroupDef>(g);
			} else if (strcmp(child_name, "component") == 0) {
				// Component reference - expand it
				ExpandComponent(dict, m, child);
			}
		}

		dict.messages[m.msg_type] = m;
	}
}

// ===========================================================
// APPLY OVERLAY XML (dialects, custom fields, etc.)
// ===========================================================
void FixDictionaryLoader::ApplyOverlay(duckdb::ClientContext &context, FixDictionary &dict, const std::string &path) {
	tinyxml2::XMLDocument doc;

	// Phase 7.8: Use DuckDB FileSystem to support S3, HTTP, etc.
	auto &fs = duckdb::FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, duckdb::FileOpenFlags::FILE_FLAGS_READ);

	// Read entire file into string
	auto file_size = fs.GetFileSize(*handle);
	std::string xml_content;
	xml_content.resize(file_size);
	handle->Read((void *)xml_content.data(), file_size);

	// Parse XML from string
	if (doc.Parse(xml_content.c_str()) != tinyxml2::XML_SUCCESS) {
		throw std::runtime_error("Failed to parse overlay XML from: " + path);
	}

	auto *root = doc.RootElement();
	if (!root) {
		throw std::runtime_error("Overlay XML missing root element.");
	}

	if (auto *fields_root = root->FirstChildElement("fields")) {
		LoadFields(dict, fields_root);
	}

	if (auto *messages_root = root->FirstChildElement("messages")) {
		LoadMessages(dict, messages_root);
	}
}
