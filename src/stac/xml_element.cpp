#include "xml_element.hpp"

namespace duckdb {

//! Silent structured error handler for XPath errors
static void XMLSilentXPathErrorHandler(void *ctx, const xmlError *error) {
	// This handler is set on individual XPath contexts to suppress errors like "Undefined namespace prefix"
	// Silently ignore XPath errors without printing to stderr
	// This is thread-safe because it's set per-context, not globally
}

XmlDocument::XmlDocument(const std::string &xml_str) : doc(nullptr), xpath_ctx(nullptr) {
	// Parse the XML with options to suppress error messages (thread-safe, per-operation config)
	// XML_PARSE_NOERROR: suppress error reports to stderr
	// XML_PARSE_NOWARNING: suppress warning reports to stderr
	// Note: We intentionally DO NOT use XML_PARSE_RECOVER to maintain strict parsing behavior
	xmlParserCtxtPtr parser_ctx = xmlNewParserCtxt();
	if (parser_ctx) {
		doc = xmlCtxtReadMemory(parser_ctx, xml_str.c_str(), xml_str.length(), nullptr, nullptr,
		                        XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	} else {
		throw IOException("Failed to create XML parser context.");
	}

	// Check if parsing failed (NULL doc means fatal error)
	if (!doc) {
		xmlFreeParserCtxt(parser_ctx);
		parser_ctx = nullptr;
		throw IOException("Failed to parse a XML document.");
	}
	xmlFreeParserCtxt(parser_ctx);
	parser_ctx = nullptr;

	// Create XPath context
	xpath_ctx = xmlXPathNewContext(doc);
	if (!xpath_ctx) {
		throw IOException("Failed to create XPath context for a XML document.");
	}

	// Set silent error handler on XPath context
	xmlSetStructuredErrorFunc(xpath_ctx, XMLSilentXPathErrorHandler);
	// Register all namespace declarations
	RegisterNamespaces(xpath_ctx);
}

XmlDocument::~XmlDocument() {
	// Reset and free the XML resources
	if (xpath_ctx) {
		xmlSetStructuredErrorFunc(xpath_ctx, nullptr);
		xmlXPathFreeContext(xpath_ctx);
		xpath_ctx = nullptr;
	}
	if (doc) {
		xmlFreeDoc(doc);
		doc = nullptr;
	}
}

//! Register all namespace declarations from the document into the XPath context
void XmlDocument::RegisterNamespaces(xmlXPathContextPtr xpath_ctx) {
	// This enables XPath expressions like "//gml:posList" to work when xmlns:gml="..." is declared.
	// Without this, libxml2's XPath engine requires manual registration of each namespace prefix.
	// See: https://grantm.github.io/perl-libxml-by-example/namespaces.html
	if (!doc || !xpath_ctx) {
		return;
	}

	xmlNodePtr root = xmlDocGetRootElement(doc);
	if (!root) {
		return;
	}

	// Get all in-scope namespaces for the root element
	xmlNsPtr *ns_list = xmlGetNsList(doc, root);
	if (!ns_list) {
		return;
	}

	// Register each namespace with a prefix
	for (int i = 0; ns_list[i] != NULL; i++) {
		xmlNsPtr ns = ns_list[i];

		// Only register namespaces that have a prefix (skip default namespace)
		// Default namespace (prefix=NULL) cannot be used in XPath 1.0 expressions directly
		if (ns->prefix && ns->href) {
			xmlXPathRegisterNs(xpath_ctx, ns->prefix, ns->href);
		}
	}

	// Free the array (but not the namespace structures themselves, they belong to the document)
	xmlFree(ns_list);
}

// Initialize libxml2 (call once at extension load)
void XmlUtils::Initialize() {
	xmlInitParser();
}

// Cleanup libxml2 (optional, for clean shutdown)
void XmlUtils::Cleanup() {
	xmlCleanupParser();
}

//! Process a single XML node and extract its information into an XMLElement structure
XmlElement XmlUtils::ProcessNode(xmlNodePtr node) {
	XmlElement element;

	if (!node)
		return element;

	// Get ID attribute if present
	xmlChar *id = xmlGetProp(node, BAD_CAST "id");
	if (id) {
		element.id = std::string((const char *)id);
		xmlFree(id);
	}

	// Handle text nodes differently
	if (node->type == XML_TEXT_NODE) {
		element.name = "#text";
		if (node->content) {
			element.text_content = std::string((const char *)node->content);
		}
		element.line_number = xmlGetLineNo(node);
		return element;
	}

	// Set name
	if (node->name) {
		element.name = std::string((const char *)node->name);
	}

	// Set text content (for element nodes, get direct text content only)
	if (node->type == XML_ELEMENT_NODE) {
		// Get only direct text children, not all descendants
		for (xmlNodePtr child = node->children; child; child = child->next) {
			if (child->type == XML_TEXT_NODE && child->content) {
				element.text_content += std::string((const char *)child->content);
			}
		}
	} else {
		xmlChar *content = xmlNodeGetContent(node);
		if (content) {
			element.text_content = std::string((const char *)content);
			xmlFree(content);
		}
	}

	// Set attributes
	for (xmlAttrPtr attr = node->properties; attr; attr = attr->next) {
		if (attr->name && attr->children && attr->children->content) {
			std::string attr_name((const char *)attr->name);
			std::string attr_value((const char *)attr->children->content);
			element.attributes[attr_name] = attr_value;
		}
	}

	// Set namespace URI
	if (node->ns && node->ns->href) {
		element.namespace_uri = std::string((const char *)node->ns->href);
	}

	// Set path
	element.path = XmlUtils::GetNodePath(node);

	// Set line number
	element.line_number = xmlGetLineNo(node);

	return element;
}

//! Get the value of a specific attribute of a XML node and return a default value if not present
std::string XmlUtils::GetNodeAttributeValue(xmlNodePtr node, const std::string &attribute_name,
                                            const std::string &default_value) {
	if (!node) {
		return default_value;
	}
	xmlChar *value = xmlGetProp(node, BAD_CAST attribute_name.c_str());
	if (!value) {
		return default_value;
	}
	std::string result(reinterpret_cast<const char *>(value));
	xmlFree(value);
	return result;
}

//! Get the text content of a XML node and return a default value if node is null
std::string XmlUtils::GetNodeTextContent(xmlNodePtr node, const std::string &default_value) {
	if (!node) {
		return default_value;
	}
	xmlChar *content = xmlNodeGetContent(node);
	if (!content) {
		return default_value;
	}
	std::string result(reinterpret_cast<const char *>(content));
	xmlFree(content);
	return result;
}

//! Get the full XPath-like path of a XML node
std::string XmlUtils::GetNodePath(xmlNodePtr node) {
	if (!node)
		return "";

	std::vector<std::string> path_parts;
	xmlNodePtr current = node;

	while (current && current->type == XML_ELEMENT_NODE) {
		if (current->name) {
			path_parts.insert(path_parts.begin(), std::string((const char *)current->name));
		}
		current = current->parent;
	}

	std::string path = "/";
	for (const auto &part : path_parts) {
		path += part + "/";
	}
	return path.substr(0, path.length() - 1); // Remove trailing slash
}

} // namespace duckdb
