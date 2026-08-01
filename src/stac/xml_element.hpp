#pragma once

#include "duckdb.hpp"

// LibXml2
#include "libxml/parser.h"
#include "libxml/xpath.h"
#include "libxml/xpathInternals.h"

namespace duckdb {

// *** NOTE:
// 	Code in this file was extracted from 'duckdb-sitemap' extension:
// 	https://github.com/midwork-finds-jobs/duckdb-sitemap
// 	Thanks a lot to Onni Hakala (onnimonni) for open sourcing it!

//! Structure to hold extracted XML element information
struct XmlElement {
	string id;
	string name;
	string namespace_uri;
	string path;
	string text_content;
	std::map<std::string, std::string> attributes;
	int64_t line_number;
};

//! Class to represent and manage one XML document
class XmlDocument {
public:
	XmlDocument(const std::string &xml_str);
	~XmlDocument();

public:
	//! LibXml2 document pointer accessor
	inline xmlDocPtr GetDoc() const {
		return doc;
	}

	//! LibXml2 XPath context pointer accessor
	inline xmlXPathContextPtr GetXPathContext() const {
		return xpath_ctx;
	}

	//! Register all namespace declarations from the document into the XPath context
	void RegisterNamespaces(xmlXPathContextPtr xpath_ctx);

private:
	//! LibXml2 document and XPath context
	xmlDocPtr doc;
	//! XPath context for querying the document
	xmlXPathContextPtr xpath_ctx;
};

//! Utility functions for XML processing
struct XmlUtils {
	//! Initialize libxml2 (call once at extension load)
	static void Initialize();
	//! Cleanup libxml2 (optional, for clean shutdown)
	static void Cleanup();

	//! Process a single XML node and extract its information into an XMLElement
	static XmlElement ProcessNode(xmlNodePtr node);

	//! Get the value of a specific attribute of a XML node and return a default value if not present
	static string GetNodeAttributeValue(xmlNodePtr node, const string &attribute_name,
	                                    const string &default_value = "");

	//! Get the text content of a XML node and return a default value if node is null
	static std::string GetNodeTextContent(xmlNodePtr node, const std::string &default_value = "");

	//! Get the full XPath-like path of a XML node
	static string GetNodePath(xmlNodePtr node);
};

} // namespace duckdb
