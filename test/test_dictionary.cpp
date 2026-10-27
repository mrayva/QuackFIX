#ifdef NDEBUG
#undef NDEBUG
#endif

#include <iostream>
#include <cassert>
#include <string>
#include <stdexcept>
#include <fstream>
#include <sstream>

#include "dictionary/xml_loader.hpp"
#include "dictionary/fix_dictionary.hpp"

int main() {
	std::cout << "Running QuackFIX Dictionary Tests...\n";
	const std::string dictionary_xml = R"xml(
<fix><fields>
  <field number="35" name="MsgType" type="STRING"/>
  <field number="11" name="ClOrdID" type="STRING"/>
  <field number="448" name="PartyID" type="STRING"/>
  <field number="447" name="PartyIDSource" type="CHAR"/>
  <field number="452" name="PartyRole" type="INT"/>
  <field number="453" name="NoPartyIDs" type="NUMINGROUP"/>
</fields><messages>
  <message name="NewOrderSingle" msgtype="D">
    <field name="ClOrdID" required="Y"/>
    <group name="NoPartyIDs" required="N">
      <field name="PartyID" required="Y"/>
      <field name="PartyIDSource" required="N"/>
      <field name="PartyRole" required="N"/>
    </group>
  </message>
</messages></fix>)xml";
	FixDictionary dict = FixDictionaryLoader::LoadFromString(dictionary_xml);

	// Basic field tests
	assert(dict.fields.count(35) == 1); // MsgType
	assert(dict.fields.at(35).name == "MsgType");
	assert(dict.name_to_tag.at("MsgType") == 35);

	// Check a couple more fields
	assert(dict.fields.count(448) == 1); // PartyID
	assert(dict.fields.count(447) == 1); // PartyIDSource

	// ------------------------------------------------------------
	// Test 2: Message definitions
	// ------------------------------------------------------------
	assert(dict.messages.count("D") == 1); // NewOrderSingle

	const FixMessageDef &nos = dict.messages.at("D");
	assert(nos.name == "NewOrderSingle");
	assert(!nos.required_fields.empty());

	// Check required fields include ClOrdID (tag 11)
	bool found_clordid = false;
	for (int tag : nos.required_fields) {
		if (tag == 11)
			found_clordid = true;
	}
	assert(found_clordid);

	// ------------------------------------------------------------
	// Test 3: Repeating groups
	// Example: FIX44 NewOrderSingle has a PartyID group (tag 453)
	// ------------------------------------------------------------
	assert(nos.groups.count(453) == 1);
	const FixGroupDef &party_group = *nos.groups.at(453);
	assert(party_group.count_tag == 453);
	assert(!party_group.field_tags.empty()); // PartyID, PartyRole, etc.

	bool rejected_unknown_field = false;
	try {
		FixDictionaryLoader::LoadFromString(
		    "<fix><fields><field number='35' name='MsgType' type='STRING'/></fields>"
		    "<messages><message name='Bad' msgtype='X'><field name='Missing' required='N'/></message></messages></fix>");
	} catch (const std::runtime_error &) {
		rejected_unknown_field = true;
	}
	assert(rejected_unknown_field && "Unknown field references should be rejected");

	bool rejected_partial_number = false;
	try {
		FixDictionaryLoader::LoadFromString(
		    "<fix><fields><field number='35junk' name='MsgType' type='STRING'/></fields></fix>");
	} catch (const std::runtime_error &) {
		rejected_partial_number = true;
	}
	assert(rejected_partial_number && "Field numbers must contain only decimal digits");

	bool rejected_duplicate_field = false;
	try {
		FixDictionaryLoader::LoadFromString(
		    "<fix><fields><field number='35' name='MsgType' type='STRING'/><field number='35' name='Other' type='STRING'/></fields></fix>");
	} catch (const std::runtime_error &) {
		rejected_duplicate_field = true;
	}
	assert(rejected_duplicate_field && "Duplicate field numbers should be rejected");

	FixDictionary overlay_target = dict;
	FixDictionaryLoader::ApplyOverlayFromString(
	    overlay_target,
	    "<fix><fields><field number='25036' name='ResponseMode' type='STRING'/></fields>"
	    "<messages><message name='CustomMessage' msgtype='Z'><field name='ResponseMode' required='Y'/></message>"
	    "</messages></fix>");
	assert(overlay_target.fields.count(25036) == 1 && "Overlay fields should be added to the dictionary");
	assert(overlay_target.name_to_tag.at("ResponseMode") == 25036);
	assert(overlay_target.messages.count("Z") == 1 && "Overlay messages should be added to the dictionary");
	assert(overlay_target.messages.at("Z").required_fields.size() == 1 &&
	       overlay_target.messages.at("Z").required_fields[0] == 25036);

	bool rejected_overlay_unknown_field = false;
	try {
		FixDictionaryLoader::ApplyOverlayFromString(
		    overlay_target,
		    "<fix><messages><message name='BadOverlay' msgtype='Y'><field name='Missing' required='N'/></message></messages></fix>");
	} catch (const std::runtime_error &) {
		rejected_overlay_unknown_field = true;
	}
	assert(rejected_overlay_unknown_field && "Overlay references to unknown fields should be rejected");

	std::ifstream fix44_file("data/fix44_dictionary.xml");
	assert(fix44_file.good() && "The repository FIX 4.4 dictionary should be available to the test");
	std::ostringstream fix44_xml;
	fix44_xml << fix44_file.rdbuf();
	FixDictionary fix44 = FixDictionaryLoader::LoadFromString(fix44_xml.str());
	assert(fix44.fields.size() > 900 && "The full FIX 4.4 field set should load");
	assert(fix44.messages.size() > 90 && "The full FIX 4.4 message set should load");

	std::cout << "All dictionary tests passed!" << std::endl;
	return 0;
}
