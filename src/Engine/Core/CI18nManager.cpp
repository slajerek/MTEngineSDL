#include "CI18nManager.h"
#include "DBG_Log.h"
#include "json.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cassert>
#include <cctype>

using namespace nlohmann;

// ============================================================================
// Singleton
// ============================================================================

CI18nManager *CI18nManager::instance = nullptr;
const char *CI18nManager::kOverflowStr = "[i18n overflow]";

CI18nManager *CI18nManager::Instance()
{
	if (!instance)
	{
		instance = new CI18nManager();
	}
	return instance;
}

// ============================================================================
// SI18nArg
// ============================================================================

SI18nArg SI18nArg::FromDecimal(const char *decimalStr)
{
	double val = atof(decimalStr);
	int fracDigits = 0;
	const char *dot = strchr(decimalStr, '.');
	if (dot)
	{
		fracDigits = (int)strlen(dot + 1);
	}
	SI18nArg arg(val, fracDigits);
	arg.strVal = decimalStr;  // preserve source
	return arg;
}

void SI18nArg::GetPluralOperands(double &n, int &i, int &v, int &w, int &f, int &t) const
{
	if (type == ARG_STRING)
	{
		n = 0; i = 0; v = 0; w = 0; f = 0; t = 0;
		return;
	}

	n = fabs(type == ARG_INT ? (double)intVal : doubleVal);
	i = (int)n;

	if (type == ARG_INT || visibleFractionDigits <= 0)
	{
		v = 0; w = 0; f = 0; t = 0;
		return;
	}

	v = visibleFractionDigits;

	// Compute f: visible fraction digits as integer WITH trailing zeros
	// e.g. for 1.50 with v=2: f = 50
	double fracPart = n - (double)i;
	double shifted = fracPart;
	for (int k = 0; k < v; k++)
		shifted *= 10.0;
	f = (int)round(shifted);

	// Compute t: f without trailing zeros
	t = f;
	while (t > 0 && (t % 10) == 0)
		t /= 10;

	// Compute w: number of digits in t
	if (t == 0)
	{
		w = 0;
	}
	else
	{
		w = 0;
		int tmp = t;
		while (tmp > 0)
		{
			w++;
			tmp /= 10;
		}
	}
}

string SI18nArg::ToString(const SI18nLocale *locale) const
{
	if (type == ARG_STRING)
		return strVal;

	if (locale)
	{
		if (type == ARG_INT)
			return CI18nManager::Instance()->FormatNumber(intVal, locale->tag);
		else
			return CI18nManager::Instance()->FormatNumber(doubleVal, locale->tag);
	}

	if (type == ARG_INT)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "%d", intVal);
		return buf;
	}
	else
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "%g", doubleVal);
		return buf;
	}
}

// ============================================================================
// BCP-47 normalization
// ============================================================================

string CI18nManager::NormalizeLocaleTag(const string &tag)
{
	if (tag.empty())
		return tag;

	// Split on '-'
	vector<string> parts;
	string current;
	for (char c : tag)
	{
		if (c == '-')
		{
			parts.push_back(current);
			current.clear();
		}
		else
		{
			current += c;
		}
	}
	parts.push_back(current);

	if (parts.empty())
		return tag;

	// Part 0: language — lowercase (2-3 chars)
	string result;
	for (char &c : parts[0])
		c = (char)tolower(c);
	result = parts[0];

	for (size_t i = 1; i < parts.size(); i++)
	{
		string &p = parts[i];
		if (p.length() == 4 && isalpha(p[0]))
		{
			// Script: titlecase (4 letters)
			p[0] = (char)toupper(p[0]);
			for (size_t j = 1; j < p.length(); j++)
				p[j] = (char)tolower(p[j]);
		}
		else if (p.length() == 2 && isalpha(p[0]))
		{
			// Region: uppercase (2 letters)
			for (char &c : p)
				c = (char)toupper(c);
		}
		// Else: pass through as-is (variants, extensions)

		result += "-";
		result += p;
	}

	return result;
}

// ============================================================================
// Locale management
// ============================================================================

void CI18nManager::RegisterLocale(const SI18nLocale &locale)
{
	SI18nLocale normalized = locale;
	normalized.tag = NormalizeLocaleTag(locale.tag);

	// Check if already registered
	auto it = localeIndex.find(normalized.tag);
	if (it != localeIndex.end())
	{
		registeredLocales[it->second] = normalized;
	}
	else
	{
		localeIndex[normalized.tag] = (int)registeredLocales.size();
		registeredLocales.push_back(normalized);
	}

	RebuildFallbackChains();

	if (activeLocale.empty())
	{
		activeLocale = normalized.tag;
	}
}

void CI18nManager::SetActiveLocale(const string &tag)
{
	activeLocale = NormalizeLocaleTag(tag);
}

const string &CI18nManager::GetActiveLocale() const
{
	return activeLocale;
}

const SI18nLocale *CI18nManager::GetLocale(const string &tag) const
{
	string normalized = NormalizeLocaleTag(tag);
	auto it = localeIndex.find(normalized);
	if (it != localeIndex.end())
	{
		return &registeredLocales[it->second];
	}
	return nullptr;
}

const vector<SI18nLocale> &CI18nManager::GetRegisteredLocales() const
{
	return registeredLocales;
}

// ============================================================================
// Fallback chains
// ============================================================================

vector<string> CI18nManager::BuildFallbackChain(const string &tag) const
{
	vector<string> chain;
	chain.push_back(tag);

	// Auto-truncation: "zh-Hans-CN" -> "zh-Hans" -> "zh"
	string current = tag;
	while (true)
	{
		size_t lastDash = current.rfind('-');
		if (lastDash == string::npos)
			break;
		current = current.substr(0, lastDash);
		chain.push_back(current);
	}

	// Append explicit fallbacks from locale definition
	auto it = localeIndex.find(tag);
	if (it != localeIndex.end())
	{
		const SI18nLocale &loc = registeredLocales[it->second];
		for (const string &fb : loc.explicitFallbacks)
		{
			string norm = NormalizeLocaleTag(fb);
			// Avoid duplicates
			bool found = false;
			for (const string &existing : chain)
			{
				if (existing == norm) { found = true; break; }
			}
			if (!found)
				chain.push_back(norm);
		}
	}

	return chain;
}

void CI18nManager::RebuildFallbackChains()
{
	fallbackChains.clear();
	fallbackDependents.clear();

	for (const SI18nLocale &loc : registeredLocales)
	{
		vector<string> chain = BuildFallbackChain(loc.tag);
		fallbackChains[loc.tag] = chain;

		// Build reverse dependency map
		for (size_t i = 1; i < chain.size(); i++)
		{
			fallbackDependents[chain[i]].insert(loc.tag);
		}
	}
}

// ============================================================================
// Miss cache
// ============================================================================

void CI18nManager::InvalidateMissCache(const string &localeTag)
{
	// Remove all entries for this locale
	for (auto it = missCache.begin(); it != missCache.end(); )
	{
		if (it->first == localeTag)
			it = missCache.erase(it);
		else
			++it;
	}

	// Also invalidate locales that depend on this locale in their fallback chain
	auto depIt = fallbackDependents.find(localeTag);
	if (depIt != fallbackDependents.end())
	{
		for (const string &dependent : depIt->second)
		{
			for (auto it = missCache.begin(); it != missCache.end(); )
			{
				if (it->first == dependent)
					it = missCache.erase(it);
				else
					++it;
			}
		}
	}
}

// ============================================================================
// String table loading
// ============================================================================

bool CI18nManager::LoadStrings(const string &localeTag, const string &jsonPath)
{
	string normalized = NormalizeLocaleTag(localeTag);

	ifstream file(jsonPath);
	if (!file.is_open())
	{
		LOGError("CI18nManager::LoadStrings: Failed to open %s", jsonPath.c_str());
		return false;
	}

	try
	{
		json jData = json::parse(file);

		if (!jData.contains("strings") || !jData["strings"].is_object())
		{
			LOGError("CI18nManager::LoadStrings: Missing 'strings' object in %s", jsonPath.c_str());
			return false;
		}

		auto &table = stringTables[normalized];
		json &strings = jData["strings"];
		for (auto it = strings.begin(); it != strings.end(); ++it)
		{
			if (it.value().is_string())
			{
				table[it.key()] = it.value().get<string>();
			}
		}

		InvalidateMissCache(normalized);
		LOGD("CI18nManager::LoadStrings: Loaded %d strings for '%s' from %s",
			 (int)strings.size(), normalized.c_str(), jsonPath.c_str());
		return true;
	}
	catch (const json::parse_error &e)
	{
		LOGError("CI18nManager::LoadStrings: JSON parse error in %s: %s", jsonPath.c_str(), e.what());
		return false;
	}
}

void CI18nManager::SetString(const string &localeTag, const string &key, const string &value)
{
	string normalized = NormalizeLocaleTag(localeTag);
	stringTables[normalized][key] = value;
	InvalidateMissCache(normalized);
}

// ============================================================================
// String lookup with fallback
// ============================================================================

const char *CI18nManager::LookupString(const string &key, const string &localeTag) const
{
	// Check miss cache
	if (missCache.count({localeTag, key}))
		return nullptr;

	// Get fallback chain
	auto chainIt = fallbackChains.find(localeTag);
	if (chainIt != fallbackChains.end())
	{
		for (const string &tag : chainIt->second)
		{
			auto tableIt = stringTables.find(tag);
			if (tableIt != stringTables.end())
			{
				auto strIt = tableIt->second.find(key);
				if (strIt != tableIt->second.end())
				{
					return strIt->second.c_str();
				}
			}
		}
	}
	else
	{
		// No chain — try direct lookup
		auto tableIt = stringTables.find(localeTag);
		if (tableIt != stringTables.end())
		{
			auto strIt = tableIt->second.find(key);
			if (strIt != tableIt->second.end())
			{
				return strIt->second.c_str();
			}
		}

		// Try tag truncation on the fly
		string current = localeTag;
		while (true)
		{
			size_t lastDash = current.rfind('-');
			if (lastDash == string::npos)
				break;
			current = current.substr(0, lastDash);
			auto tableIt2 = stringTables.find(current);
			if (tableIt2 != stringTables.end())
			{
				auto strIt = tableIt2->second.find(key);
				if (strIt != tableIt2->second.end())
				{
					return strIt->second.c_str();
				}
			}
		}
	}

	// Cache the miss
	missCache.insert({localeTag, key});
	return nullptr;
}

const char *CI18nManager::Get(const string &key) const
{
	return Get(key, activeLocale);
}

const char *CI18nManager::Get(const string &key, const string &localeTag) const
{
	string normalized = NormalizeLocaleTag(localeTag);

	const char *result = LookupString(key, normalized);
	if (result)
		return result;

	// Try "en" as ultimate fallback
	if (normalized != "en")
	{
		auto tableIt = stringTables.find("en");
		if (tableIt != stringTables.end())
		{
			auto strIt = tableIt->second.find(key);
			if (strIt != tableIt->second.end())
			{
				return strIt->second.c_str();
			}
		}
	}

	// Try first registered locale
	if (!registeredLocales.empty() && registeredLocales[0].tag != normalized)
	{
		auto tableIt = stringTables.find(registeredLocales[0].tag);
		if (tableIt != stringTables.end())
		{
			auto strIt = tableIt->second.find(key);
			if (strIt != tableIt->second.end())
			{
				return strIt->second.c_str();
			}
		}
	}

	// Return the key itself — intern it for stable pointer
	if ((int)missingKeyStorage.size() >= kMaxMissingKeys)
	{
		LOGWarning("CI18nManager::Get: Missing key storage overflow, key='%s'", key.c_str());
		return kOverflowStr;
	}

	auto it = missingKeyStorage.find(key);
	if (it == missingKeyStorage.end())
	{
		LOGWarning("CI18nManager::Get: Missing i18n key '%s' for locale '%s'", key.c_str(), normalized.c_str());
		missingKeyStorage[key] = key;
		it = missingKeyStorage.find(key);
	}
	return it->second.c_str();
}

// ============================================================================
// Plural resolution
// ============================================================================

const SI18nLocale *CI18nManager::GetLocaleOrFallback(const string &tag) const
{
	const SI18nLocale *loc = GetLocale(tag);
	if (loc) return loc;

	// Try truncation
	string current = tag;
	while (true)
	{
		size_t lastDash = current.rfind('-');
		if (lastDash == string::npos)
			break;
		current = current.substr(0, lastDash);
		loc = GetLocale(current);
		if (loc) return loc;
	}

	// Try "en"
	loc = GetLocale("en");
	if (loc) return loc;

	// First registered
	if (!registeredLocales.empty())
		return &registeredLocales[0];

	return nullptr;
}

EI18nPluralCategory CI18nManager::GetPluralCategory(double n) const
{
	return GetPluralCategory(n, activeLocale);
}

EI18nPluralCategory CI18nManager::GetPluralCategory(double n, const string &localeTag) const
{
	const SI18nLocale *loc = GetLocaleOrFallback(localeTag);
	if (!loc || !loc->cardinalRule)
		return I18N_PLURAL_OTHER;

	SI18nArg arg(n);
	double pn; int pi, pv, pw, pf, pt;
	arg.GetPluralOperands(pn, pi, pv, pw, pf, pt);
	return loc->cardinalRule(pn, pi, pv, pw, pf, pt);
}

const char *CI18nManager::PluralCategoryName(EI18nPluralCategory cat)
{
	switch (cat)
	{
		case I18N_PLURAL_ZERO:  return "zero";
		case I18N_PLURAL_ONE:   return "one";
		case I18N_PLURAL_TWO:   return "two";
		case I18N_PLURAL_FEW:   return "few";
		case I18N_PLURAL_MANY:  return "many";
		case I18N_PLURAL_OTHER: return "other";
	}
	return "other";
}

// ============================================================================
// Number formatting
// ============================================================================

string CI18nManager::FormatNumberImpl(double n, bool isInt, const SI18nLocale *locale) const
{
	SI18nNumberFormat fmt;
	if (locale)
		fmt = locale->numberFormat;

	bool negative = n < 0;
	if (negative) n = -n;

	long long intPart = (long long)n;
	string intStr;

	if (intPart == 0)
	{
		intStr = "0";
	}
	else
	{
		while (intPart > 0)
		{
			intStr = char('0' + (intPart % 10)) + intStr;
			intPart /= 10;
		}
	}

	// Insert thousands separators
	if (fmt.groupingSize > 0 && !fmt.thousandsSeparator.empty() && (int)intStr.length() > fmt.groupingSize)
	{
		string grouped;
		int count = 0;
		for (int i = (int)intStr.length() - 1; i >= 0; i--)
		{
			if (count > 0 && count % fmt.groupingSize == 0)
			{
				grouped = fmt.thousandsSeparator + grouped;
			}
			grouped = intStr[i] + grouped;
			count++;
		}
		intStr = grouped;
	}

	string result = negative ? "-" : "";
	result += intStr;

	if (!isInt)
	{
		double fracPart = n - (long long)n;
		if (fracPart > 0.0000001)
		{
			char fracBuf[32];
			snprintf(fracBuf, sizeof(fracBuf), "%g", fracPart);
			// fracBuf is something like "0.123" — extract after the '.'
			const char *dot = strchr(fracBuf, '.');
			if (dot)
			{
				result += fmt.decimalSeparator;
				result += (dot + 1);
			}
		}
	}

	return result;
}

string CI18nManager::FormatNumber(double n) const
{
	return FormatNumber(n, activeLocale);
}

string CI18nManager::FormatNumber(int n) const
{
	return FormatNumber(n, activeLocale);
}

string CI18nManager::FormatNumber(double n, const string &localeTag) const
{
	const SI18nLocale *loc = GetLocaleOrFallback(localeTag);
	return FormatNumberImpl(n, false, loc);
}

string CI18nManager::FormatNumber(int n, const string &localeTag) const
{
	const SI18nLocale *loc = GetLocaleOrFallback(localeTag);
	return FormatNumberImpl((double)n, true, loc);
}

// ============================================================================
// MessageFormat parser — AST nodes
// ============================================================================

enum EI18nNodeType {
	NODE_TEXT,          // literal text
	NODE_ARGUMENT,      // simple {var} substitution
	NODE_PLURAL,        // {var, plural, ...}
	NODE_SELECT,        // {var, select, ...}
	NODE_SELECTORDINAL, // {var, selectordinal, ...}
	NODE_HASH           // # (number placeholder in plural/selectordinal)
};

struct SI18nPatternNode {
	EI18nNodeType type;
	string text;        // for NODE_TEXT
	string argName;     // for NODE_ARGUMENT, NODE_PLURAL, NODE_SELECT, NODE_SELECTORDINAL
	int offset = 0;     // for plural offset:N

	// Branches for plural/select: keyword -> sub-pattern
	// Keywords: "one", "two", "few", "many", "other", "=0", "=1", etc.
	vector<pair<string, vector<unique_ptr<SI18nPatternNode>>>> branches;
};

// ============================================================================
// MessageFormat parser
// ============================================================================

class CI18nPatternParser {
public:
	static bool Parse(const string &pattern, SI18nCompiledPattern &out);

private:
	const string &src;
	size_t pos;

	CI18nPatternParser(const string &s) : src(s), pos(0) {}

	bool ParseNodes(vector<unique_ptr<SI18nPatternNode>> &nodes, bool insideBrace);
	bool ParsePlaceholder(vector<unique_ptr<SI18nPatternNode>> &nodes);
	bool ParseBranches(SI18nPatternNode *node);
	bool ParseBranchBody(vector<unique_ptr<SI18nPatternNode>> &nodes);
	void SkipWhitespace();
	string ReadIdentifier();

	bool atEnd() const { return pos >= src.length(); }
	char peek() const { return pos < src.length() ? src[pos] : '\0'; }
	char advance() { return pos < src.length() ? src[pos++] : '\0'; }
};

bool CI18nPatternParser::Parse(const string &pattern, SI18nCompiledPattern &out)
{
	CI18nPatternParser parser(pattern);
	out.nodes.clear();
	return parser.ParseNodes(out.nodes, false);
}

void CI18nPatternParser::SkipWhitespace()
{
	while (pos < src.length() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r'))
		pos++;
}

string CI18nPatternParser::ReadIdentifier()
{
	string id;
	while (pos < src.length() && (isalnum(src[pos]) || src[pos] == '_' || src[pos] == '-' || src[pos] == '.'))
	{
		id += src[pos++];
	}
	return id;
}

bool CI18nPatternParser::ParseNodes(vector<unique_ptr<SI18nPatternNode>> &nodes, bool insideBrace)
{
	string textAccum;

	auto flushText = [&]() {
		if (!textAccum.empty())
		{
			auto node = make_unique<SI18nPatternNode>();
			node->type = NODE_TEXT;
			node->text = textAccum;
			nodes.push_back(std::move(node));
			textAccum.clear();
		}
	};

	while (pos < src.length())
	{
		char c = src[pos];

		// Stop at closing brace if inside a branch
		if (c == '}' && insideBrace)
		{
			break;
		}

		// Apostrophe escaping (ICU-like)
		if (c == '\'')
		{
			pos++;
			if (pos < src.length() && src[pos] == '\'')
			{
				// '' -> literal '
				textAccum += '\'';
				pos++;
			}
			else
			{
				// Quoted section: '...'
				while (pos < src.length())
				{
					if (src[pos] == '\'')
					{
						pos++;
						// Check for '' inside quotes (escaped apostrophe)
						if (pos < src.length() && src[pos] == '\'')
						{
							textAccum += '\'';
							pos++;
						}
						else
						{
							break;  // End of quoted section
						}
					}
					else
					{
						textAccum += src[pos++];
					}
				}
				// If we reach end without closing quote, the unmatched ' is treated as literal
			}
			continue;
		}

		// {{ -> literal {
		if (c == '{' && pos + 1 < src.length() && src[pos + 1] == '{')
		{
			textAccum += '{';
			pos += 2;
			continue;
		}

		// }} -> literal }
		if (c == '}' && !insideBrace && pos + 1 < src.length() && src[pos + 1] == '}')
		{
			textAccum += '}';
			pos += 2;
			continue;
		}

		// Bare } outside brace context -> literal
		if (c == '}' && !insideBrace)
		{
			textAccum += c;
			pos++;
			continue;
		}

		// # placeholder (only meaningful inside plural/selectordinal, handled at eval time)
		if (c == '#')
		{
			flushText();
			auto node = make_unique<SI18nPatternNode>();
			node->type = NODE_HASH;
			nodes.push_back(std::move(node));
			pos++;
			continue;
		}

		// { -> start placeholder
		if (c == '{')
		{
			flushText();
			pos++;  // consume '{'
			if (!ParsePlaceholder(nodes))
				return false;
			continue;
		}

		textAccum += c;
		pos++;
	}

	flushText();
	return true;
}

bool CI18nPatternParser::ParsePlaceholder(vector<unique_ptr<SI18nPatternNode>> &nodes)
{
	SkipWhitespace();
	string argName = ReadIdentifier();
	if (argName.empty())
	{
		LOGWarning("CI18nManager: Empty argument name in pattern");
		return false;
	}

	SkipWhitespace();

	if (pos < src.length() && src[pos] == '}')
	{
		// Simple substitution: {var}
		pos++;  // consume '}'
		auto node = make_unique<SI18nPatternNode>();
		node->type = NODE_ARGUMENT;
		node->argName = argName;
		nodes.push_back(std::move(node));
		return true;
	}

	if (pos < src.length() && src[pos] == ',')
	{
		pos++;  // consume ','
		SkipWhitespace();
		string formatType = ReadIdentifier();
		SkipWhitespace();

		if (pos < src.length() && src[pos] == ',')
			pos++;  // consume ','

		SkipWhitespace();

		auto node = make_unique<SI18nPatternNode>();
		node->argName = argName;

		if (formatType == "plural")
		{
			node->type = NODE_PLURAL;
		}
		else if (formatType == "select")
		{
			node->type = NODE_SELECT;
		}
		else if (formatType == "selectordinal")
		{
			node->type = NODE_SELECTORDINAL;
		}
		else
		{
			LOGWarning("CI18nManager: Unknown format type '%s'", formatType.c_str());
			return false;
		}

		if (!ParseBranches(node.get()))
			return false;

		if (pos < src.length() && src[pos] == '}')
			pos++;  // consume closing '}'
		else
		{
			LOGWarning("CI18nManager: Expected '}' at end of %s", formatType.c_str());
			return false;
		}

		nodes.push_back(std::move(node));
		return true;
	}

	LOGWarning("CI18nManager: Unexpected character in placeholder");
	return false;
}

bool CI18nPatternParser::ParseBranches(SI18nPatternNode *node)
{
	// Check for offset:N (plural only)
	if (node->type == NODE_PLURAL || node->type == NODE_SELECTORDINAL)
	{
		size_t savedPos = pos;
		string maybeOffset = ReadIdentifier();
		if (maybeOffset == "offset")
		{
			SkipWhitespace();
			if (pos < src.length() && src[pos] == ':')
			{
				pos++;
				SkipWhitespace();
				string numStr;
				while (pos < src.length() && isdigit(src[pos]))
					numStr += src[pos++];
				if (!numStr.empty())
					node->offset = atoi(numStr.c_str());
				SkipWhitespace();
			}
			else
			{
				pos = savedPos;  // Not an offset
			}
		}
		else
		{
			pos = savedPos;
		}
	}

	while (pos < src.length() && src[pos] != '}')
	{
		SkipWhitespace();
		if (pos >= src.length() || src[pos] == '}')
			break;

		// Read keyword: "one", "other", "=0", "=1", etc.
		string keyword;
		if (src[pos] == '=')
		{
			keyword += src[pos++];
			while (pos < src.length() && isdigit(src[pos]))
				keyword += src[pos++];
		}
		else
		{
			keyword = ReadIdentifier();
		}

		if (keyword.empty())
		{
			LOGWarning("CI18nManager: Empty keyword in branches");
			return false;
		}

		SkipWhitespace();

		if (pos >= src.length() || src[pos] != '{')
		{
			LOGWarning("CI18nManager: Expected '{' after keyword '%s'", keyword.c_str());
			return false;
		}
		pos++;  // consume '{'

		vector<unique_ptr<SI18nPatternNode>> branchNodes;
		if (!ParseBranchBody(branchNodes))
			return false;

		if (pos >= src.length() || src[pos] != '}')
		{
			LOGWarning("CI18nManager: Expected '}' to close branch '%s'", keyword.c_str());
			return false;
		}
		pos++;  // consume '}'

		node->branches.push_back({keyword, std::move(branchNodes)});
		SkipWhitespace();
	}

	return true;
}

bool CI18nPatternParser::ParseBranchBody(vector<unique_ptr<SI18nPatternNode>> &nodes)
{
	return ParseNodes(nodes, true);
}

// ============================================================================
// Pattern compilation and caching
// ============================================================================

const SI18nCompiledPattern *CI18nManager::GetCompiledPattern(const string &pattern) const
{
	auto it = patternCache.find(pattern);
	if (it != patternCache.end())
		return it->second.get();

	// Evict cache if full
	if ((int)patternCache.size() >= kMaxPatternCache)
	{
		LOGWarning("CI18nManager: Pattern cache overflow (%d entries), clearing", (int)patternCache.size());
		patternCache.clear();
	}

	auto compiled = make_unique<SI18nCompiledPattern>();
	if (!CI18nPatternParser::Parse(pattern, *compiled))
	{
		LOGWarning("CI18nManager: Failed to parse pattern: %s", pattern.c_str());
		// Store a single text node with the raw pattern as fallback
		compiled->nodes.clear();
		auto node = make_unique<SI18nPatternNode>();
		node->type = NODE_TEXT;
		node->text = pattern;
		compiled->nodes.push_back(std::move(node));
	}

	const SI18nCompiledPattern *ptr = compiled.get();
	patternCache[pattern] = std::move(compiled);
	return ptr;
}

// ============================================================================
// Pattern evaluation
// ============================================================================

static void EvaluateNodes(const vector<unique_ptr<SI18nPatternNode>> &nodes,
						  const map<string, SI18nArg> &args,
						  const SI18nLocale *locale,
						  const SI18nArg *pluralArg,
						  int pluralOffset,
						  string &out);

static void EvaluateNodes(const vector<unique_ptr<SI18nPatternNode>> &nodes,
						  const map<string, SI18nArg> &args,
						  const SI18nLocale *locale,
						  const SI18nArg *pluralArg,
						  int pluralOffset,
						  string &out)
{
	for (const auto &nodePtr : nodes)
	{
		const SI18nPatternNode &node = *nodePtr;

		switch (node.type)
		{
			case NODE_TEXT:
				out += node.text;
				break;

			case NODE_HASH:
			{
				// # is replaced with the plural argument value minus offset, locale-formatted
				if (pluralArg)
				{
					if (pluralArg->type == SI18nArg::ARG_INT)
					{
						int val = pluralArg->intVal - pluralOffset;
						out += CI18nManager::Instance()->FormatNumber(val, locale ? locale->tag : "en");
					}
					else if (pluralArg->type == SI18nArg::ARG_DOUBLE)
					{
						double val = pluralArg->doubleVal - (double)pluralOffset;
						out += CI18nManager::Instance()->FormatNumber(val, locale ? locale->tag : "en");
					}
					else
					{
						out += pluralArg->strVal;
					}
				}
				else
				{
					out += "#";
				}
				break;
			}

			case NODE_ARGUMENT:
			{
				auto it = args.find(node.argName);
				if (it != args.end())
				{
					out += it->second.ToString(locale);
				}
				else
				{
					LOGWarning("CI18nManager: Missing argument '%s'", node.argName.c_str());
					out += "{";
					out += node.argName;
					out += "}";
				}
				break;
			}

			case NODE_PLURAL:
			case NODE_SELECTORDINAL:
			{
				auto argIt = args.find(node.argName);
				if (argIt == args.end())
				{
					LOGWarning("CI18nManager: Missing plural argument '%s'", node.argName.c_str());
					out += "{";
					out += node.argName;
					out += "}";
					break;
				}

				const SI18nArg &arg = argIt->second;
				double pn; int pi, pv, pw, pf, pt;
				arg.GetPluralOperands(pn, pi, pv, pw, pf, pt);

				// Apply offset for plural category selection
				double offsetN = pn - node.offset;

				// Check exact match first (=0, =1, etc.)
				string exactKey = "=" + to_string((int)pn);
				const vector<unique_ptr<SI18nPatternNode>> *selectedBranch = nullptr;

				for (const auto &branch : node.branches)
				{
					if (branch.first == exactKey)
					{
						selectedBranch = &branch.second;
						break;
					}
				}

				if (!selectedBranch)
				{
					// Get plural category
					EI18nPluralCategory cat = I18N_PLURAL_OTHER;
					if (locale)
					{
						auto rule = (node.type == NODE_PLURAL) ? locale->cardinalRule : locale->ordinalRule;
						if (rule)
						{
							// Use offset values for category determination
							SI18nArg offsetArg(offsetN);
							double on; int oi, ov, ow, of2, ot;
							offsetArg.GetPluralOperands(on, oi, ov, ow, of2, ot);
							cat = rule(on, oi, ov, ow, of2, ot);
						}
					}

					const char *catName = CI18nManager::PluralCategoryName(cat);
					for (const auto &branch : node.branches)
					{
						if (branch.first == catName)
						{
							selectedBranch = &branch.second;
							break;
						}
					}

					// Fallback to "other"
					if (!selectedBranch)
					{
						for (const auto &branch : node.branches)
						{
							if (branch.first == "other")
							{
								selectedBranch = &branch.second;
								break;
							}
						}
					}
				}

				if (selectedBranch)
				{
					EvaluateNodes(*selectedBranch, args, locale, &arg, node.offset, out);
				}
				else
				{
					LOGWarning("CI18nManager: No matching branch for plural/selectordinal '%s'", node.argName.c_str());
				}
				break;
			}

			case NODE_SELECT:
			{
				auto argIt = args.find(node.argName);
				string selectValue;
				if (argIt != args.end())
				{
					selectValue = argIt->second.type == SI18nArg::ARG_STRING
						? argIt->second.strVal
						: argIt->second.ToString(locale);
				}
				else
				{
					LOGWarning("CI18nManager: Missing select argument '%s'", node.argName.c_str());
				}

				const vector<unique_ptr<SI18nPatternNode>> *selectedBranch = nullptr;

				for (const auto &branch : node.branches)
				{
					if (branch.first == selectValue)
					{
						selectedBranch = &branch.second;
						break;
					}
				}

				// Fallback to "other"
				if (!selectedBranch)
				{
					for (const auto &branch : node.branches)
					{
						if (branch.first == "other")
						{
							selectedBranch = &branch.second;
							break;
						}
					}
				}

				if (selectedBranch)
				{
					EvaluateNodes(*selectedBranch, args, locale, pluralArg, pluralOffset, out);
				}
				else
				{
					LOGWarning("CI18nManager: No matching branch for select '%s' value '%s'",
							   node.argName.c_str(), selectValue.c_str());
				}
				break;
			}
		}
	}
}

string CI18nManager::EvaluatePattern(const SI18nCompiledPattern *compiled,
									 const map<string, SI18nArg> &args,
									 const SI18nLocale *locale) const
{
	string result;
	EvaluateNodes(compiled->nodes, args, locale, nullptr, 0, result);
	return result;
}

// ============================================================================
// Format (public API)
// ============================================================================

string CI18nManager::Format(const string &pattern, const map<string, SI18nArg> &args) const
{
	return Format(pattern, args, activeLocale);
}

string CI18nManager::Format(const string &pattern, const map<string, SI18nArg> &args,
							const string &localeTag) const
{
	const SI18nCompiledPattern *compiled = GetCompiledPattern(pattern);
	const SI18nLocale *locale = GetLocaleOrFallback(localeTag);
	return EvaluatePattern(compiled, args, locale);
}

// ============================================================================
// _TID helper
// ============================================================================

const char *_TID(const char *key, const char *imguiId)
{
	static thread_local char buffer[4096];
	const char *translated = CI18nManager::Instance()->Get(key);
	snprintf(buffer, sizeof(buffer), "%s##%s", translated, imguiId);
	return buffer;
}
