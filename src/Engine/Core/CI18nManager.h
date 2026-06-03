#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <functional>
#include <cmath>
#include <cstring>
#include <cstdlib>

using namespace std;

// --- CLDR plural categories (all 6, universal) ---
enum EI18nPluralCategory {
	I18N_PLURAL_ZERO,
	I18N_PLURAL_ONE,
	I18N_PLURAL_TWO,
	I18N_PLURAL_FEW,
	I18N_PLURAL_MANY,
	I18N_PLURAL_OTHER
};

// --- Locale number formatting ---
struct SI18nNumberFormat {
	string decimalSeparator   = ".";
	string thousandsSeparator = ",";
	int groupingSize          = 3;
};

// Forward declaration for SI18nArg::ToString
struct SI18nLocale;

// --- Typed argument for Format() ---
struct SI18nArg {
	enum Type { ARG_INT, ARG_DOUBLE, ARG_STRING };
	Type type;
	int intVal;
	double doubleVal;
	string strVal;
	int visibleFractionDigits;  // v operand: -1 = unknown (treat as 0 for plural)

	// Integer
	SI18nArg(int v)
		: type(ARG_INT), intVal(v), doubleVal(v), visibleFractionDigits(0) {}

	// Double
	SI18nArg(double v, int fracDigits = -1)
		: type(ARG_DOUBLE), intVal((int)v), doubleVal(v), visibleFractionDigits(fracDigits) {}

	// Construct from decimal source string — computes exact CLDR operands
	static SI18nArg FromDecimal(const char *decimalStr);

	// String
	SI18nArg(const string &v)
		: type(ARG_STRING), intVal(0), doubleVal(0), visibleFractionDigits(0), strVal(v) {}
	SI18nArg(const char *v)
		: type(ARG_STRING), intVal(0), doubleVal(0), visibleFractionDigits(0), strVal(v) {}

	// Compute all 6 CLDR operands
	void GetPluralOperands(double &n, int &i, int &v, int &w, int &f, int &t) const;

	// String representation (locale-formatted for numbers)
	string ToString(const SI18nLocale *locale) const;
};

// --- Locale definition ---
struct SI18nLocale {
	string tag;          // BCP-47: "en", "pl", "it"
	string displayName;  // "English", "Polski", "Italiano"

	vector<string> explicitFallbacks;  // appended after auto-truncation chain

	// Pluggable plural rules — game registers these. NULL = always OTHER.
	EI18nPluralCategory (*cardinalRule)(double n, int i, int v, int w, int f, int t) = nullptr;
	EI18nPluralCategory (*ordinalRule)(double n, int i, int v, int w, int f, int t) = nullptr;

	SI18nNumberFormat numberFormat;
};

// --- Compiled pattern AST node (internal) ---
struct SI18nPatternNode;

struct SI18nCompiledPattern {
	vector<unique_ptr<SI18nPatternNode>> nodes;
};

// --- CI18nManager ---
class CI18nManager {
public:
	// Locale management
	void RegisterLocale(const SI18nLocale &locale);
	void SetActiveLocale(const string &tag);
	const string &GetActiveLocale() const;
	const SI18nLocale *GetLocale(const string &tag) const;
	const vector<SI18nLocale> &GetRegisteredLocales() const;

	// String table loading from JSON
	bool LoadStrings(const string &localeTag, const string &jsonPath);
	void SetString(const string &localeTag, const string &key, const string &value);

	// Lookup with fallback chain
	const char *Get(const string &key) const;
	const char *Get(const string &key, const string &localeTag) const;

	// Plural resolution
	EI18nPluralCategory GetPluralCategory(double n) const;
	EI18nPluralCategory GetPluralCategory(double n, const string &localeTag) const;
	static const char *PluralCategoryName(EI18nPluralCategory cat);

	// ICU-like MessageFormat subset
	string Format(const string &pattern, const map<string, SI18nArg> &args) const;
	string Format(const string &pattern, const map<string, SI18nArg> &args,
				  const string &localeTag) const;

	// Number formatting
	string FormatNumber(double n) const;
	string FormatNumber(int n) const;
	string FormatNumber(double n, const string &localeTag) const;
	string FormatNumber(int n, const string &localeTag) const;

	// Singleton
	static CI18nManager *Instance();

private:
	string activeLocale;
	vector<SI18nLocale> registeredLocales;
	map<string, int> localeIndex;  // tag -> index in registeredLocales

	// String tables: locale -> (key -> string)
	map<string, map<string, string>> stringTables;

	// Fallback chain cache: locale tag -> ordered list of tags to try
	map<string, vector<string>> fallbackChains;
	void RebuildFallbackChains();
	vector<string> BuildFallbackChain(const string &tag) const;

	// Reverse dependency map for missCache invalidation
	map<string, set<string>> fallbackDependents;

	// Fallback miss cache
	mutable set<pair<string,string>> missCache;
	void InvalidateMissCache(const string &localeTag);

	// Stable storage for missing keys
	mutable map<string, string> missingKeyStorage;
	static const int kMaxMissingKeys = 8192;
	static const char *kOverflowStr;

	// Pattern cache
	mutable map<string, unique_ptr<SI18nCompiledPattern>> patternCache;
	static const int kMaxPatternCache = 4096;

	const SI18nCompiledPattern *GetCompiledPattern(const string &pattern) const;
	string EvaluatePattern(const SI18nCompiledPattern *compiled,
						   const map<string, SI18nArg> &args,
						   const SI18nLocale *locale) const;

	// Internal helpers
	const SI18nLocale *GetLocaleOrFallback(const string &tag) const;
	const char *LookupString(const string &key, const string &localeTag) const;
	string FormatNumberImpl(double n, bool isInt, const SI18nLocale *locale) const;

	// BCP-47 normalization
	static string NormalizeLocaleTag(const string &tag);

	// Singleton instance
	static CI18nManager *instance;
};

// --- Convenience macros ---
#define _T(key) CI18nManager::Instance()->Get(key)

// For ImGui widgets: returns "Translated Label##stableId" (thread-local buffer)
const char *_TID(const char *key, const char *imguiId);
