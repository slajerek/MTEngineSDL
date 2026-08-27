#include "CTestI18nEngine.h"
#include "CI18nManager.h"
#include "SYS_Main.h"
#include "DBG_Log.h"
#include <string>
#include <map>
#include <cmath>
using namespace std;

// Helper macro for assertions within test
#define ASSERT_EQ(actual, expected, msg) \
	do { \
		if ((actual) != (expected)) { \
			char buf[512]; \
			snprintf(buf, sizeof(buf), "FAIL: %s — expected '%s', got '%s'", \
				msg, string(expected).c_str(), string(actual).c_str()); \
			LOGD("CTestI18nEngine: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

#define ASSERT_TRUE(cond, msg) \
	do { \
		if (!(cond)) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", msg); \
			LOGD("CTestI18nEngine: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

// ============================================================================
// Test plural rules for the test
// ============================================================================

static EI18nPluralCategory TestPluralEN(double n, int i, int v, int w, int f, int t)
{
	if (i == 1 && v == 0) return I18N_PLURAL_ONE;
	return I18N_PLURAL_OTHER;
}

static EI18nPluralCategory TestOrdinalEN(double n, int i, int v, int w, int f, int t)
{
	int mod10 = i % 10;
	int mod100 = i % 100;
	if (mod10 == 1 && mod100 != 11) return I18N_PLURAL_ONE;
	if (mod10 == 2 && mod100 != 12) return I18N_PLURAL_TWO;
	if (mod10 == 3 && mod100 != 13) return I18N_PLURAL_FEW;
	return I18N_PLURAL_OTHER;
}

static EI18nPluralCategory TestPluralPL(double n, int i, int v, int w, int f, int t)
{
	if (i == 1 && v == 0) return I18N_PLURAL_ONE;
	int mod10 = i % 10;
	int mod100 = i % 100;
	if (v != 0) return I18N_PLURAL_OTHER;
	if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return I18N_PLURAL_FEW;
	if ((mod10 == 0 || mod10 == 1) || (mod10 >= 5 && mod10 <= 9) || (mod100 >= 12 && mod100 <= 14))
		return I18N_PLURAL_MANY;
	return I18N_PLURAL_OTHER;
}

// ============================================================================
// Test implementation
// ============================================================================

CTestI18nEngine::CTestI18nEngine() {}
CTestI18nEngine::~CTestI18nEngine() {}

void CTestI18nEngine::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	LOGD("CTestI18nEngine: Starting CI18nManager engine tests");

	CI18nManager *mgr = CI18nManager::Instance();

	// The locales should already be registered by CLightHeroesI18n::Init()
	// Verify they exist
	ASSERT_TRUE(mgr->GetLocale("en") != NULL, "English locale registered");
	ASSERT_TRUE(mgr->GetLocale("pl") != NULL, "Polish locale registered");
	ASSERT_TRUE(mgr->GetLocale("it") != NULL, "Italian locale registered");

	// --- Test 1: String lookup ---
	const char *saved = mgr->GetActiveLocale().c_str();
	mgr->SetActiveLocale("en");
	ASSERT_EQ(string(mgr->Get("menu.file")), string("File"), "Get menu.file in EN");

	mgr->SetActiveLocale("pl");
	ASSERT_EQ(string(mgr->Get("menu.file")), string("Plik"), "Get menu.file in PL");

	mgr->SetActiveLocale("it");
	ASSERT_EQ(string(mgr->Get("menu.file")), string("File"), "Get menu.file in IT");

	// --- Test 2: Explicit locale override ---
	ASSERT_EQ(string(mgr->Get("menu.file.save", "en")), string("Save"), "Get menu.file.save with explicit EN");
	ASSERT_EQ(string(mgr->Get("menu.file.save", "pl")), string("Zapisz"), "Get menu.file.save with explicit PL");

	// --- Test 3: Fallback for missing key ---
	// A key that doesn't exist should return the key itself
	string missingKey = "test.nonexistent.key.12345";
	string result = mgr->Get(missingKey);
	ASSERT_EQ(result, missingKey, "Missing key returns key itself");

	// --- Test 4: Plural categories ---
	mgr->SetActiveLocale("en");
	ASSERT_TRUE(mgr->GetPluralCategory(1) == I18N_PLURAL_ONE, "EN plural: 1 -> one");
	ASSERT_TRUE(mgr->GetPluralCategory(0) == I18N_PLURAL_OTHER, "EN plural: 0 -> other");
	ASSERT_TRUE(mgr->GetPluralCategory(5) == I18N_PLURAL_OTHER, "EN plural: 5 -> other");

	ASSERT_TRUE(mgr->GetPluralCategory(1, "pl") == I18N_PLURAL_ONE, "PL plural: 1 -> one");
	ASSERT_TRUE(mgr->GetPluralCategory(2, "pl") == I18N_PLURAL_FEW, "PL plural: 2 -> few");
	ASSERT_TRUE(mgr->GetPluralCategory(3, "pl") == I18N_PLURAL_FEW, "PL plural: 3 -> few");
	ASSERT_TRUE(mgr->GetPluralCategory(5, "pl") == I18N_PLURAL_MANY, "PL plural: 5 -> many");
	ASSERT_TRUE(mgr->GetPluralCategory(12, "pl") == I18N_PLURAL_MANY, "PL plural: 12 -> many");
	ASSERT_TRUE(mgr->GetPluralCategory(22, "pl") == I18N_PLURAL_FEW, "PL plural: 22 -> few");

	// --- Test 4b: Built-in CLDR rules for the common app languages ---
	// Invoked directly, so the test does not depend on any host registering these
	// locales. Operand order is (n, i, v, w, f, t).
	{
		EI18nPluralRuleFn frCard = CI18nManager::GetBuiltinCardinalRule("fr");
		ASSERT_TRUE(frCard != nullptr, "Builtin cardinal rule exists for fr");
		ASSERT_TRUE(CI18nManager::GetBuiltinCardinalRule("fr-FR") != nullptr,
					"Builtin cardinal rule resolves for fr-FR");
		ASSERT_TRUE(frCard(0.0, 0, 0, 0, 0, 0) == I18N_PLURAL_ONE, "FR cardinal: 0 -> one");
		ASSERT_TRUE(frCard(1.0, 1, 0, 0, 0, 0) == I18N_PLURAL_ONE, "FR cardinal: 1 -> one");
		ASSERT_TRUE(frCard(2.0, 2, 0, 0, 0, 0) == I18N_PLURAL_OTHER, "FR cardinal: 2 -> other");
		ASSERT_TRUE(frCard(1000000.0, 1000000, 0, 0, 0, 0) == I18N_PLURAL_MANY,
					"FR cardinal: 1000000 -> many");
		ASSERT_TRUE(frCard(1000001.0, 1000001, 0, 0, 0, 0) == I18N_PLURAL_OTHER,
					"FR cardinal: 1000001 -> other");

		EI18nPluralRuleFn frOrd = CI18nManager::GetBuiltinOrdinalRule("fr");
		ASSERT_TRUE(frOrd != nullptr, "Builtin ordinal rule exists for fr");
		ASSERT_TRUE(frOrd(1.0, 1, 0, 0, 0, 0) == I18N_PLURAL_ONE, "FR ordinal: 1 -> one");
		ASSERT_TRUE(frOrd(2.0, 2, 0, 0, 0, 0) == I18N_PLURAL_OTHER, "FR ordinal: 2 -> other");

		EI18nPluralRuleFn esCard = CI18nManager::GetBuiltinCardinalRule("es");
		ASSERT_TRUE(esCard != nullptr, "Builtin cardinal rule exists for es");
		ASSERT_TRUE(CI18nManager::GetBuiltinCardinalRule("es-ES") != nullptr,
					"Builtin cardinal rule resolves for es-ES");
		ASSERT_TRUE(esCard(0.0, 0, 0, 0, 0, 0) == I18N_PLURAL_OTHER, "ES cardinal: 0 -> other");
		ASSERT_TRUE(esCard(1.0, 1, 0, 0, 0, 0) == I18N_PLURAL_ONE, "ES cardinal: 1 -> one");
		ASSERT_TRUE(esCard(2.0, 2, 0, 0, 0, 0) == I18N_PLURAL_OTHER, "ES cardinal: 2 -> other");
		ASSERT_TRUE(esCard(1000000.0, 1000000, 0, 0, 0, 0) == I18N_PLURAL_MANY,
					"ES cardinal: 1000000 -> many");

		EI18nPluralRuleFn ptBRCard = CI18nManager::GetBuiltinCardinalRule("pt-BR");
		ASSERT_TRUE(ptBRCard != nullptr, "Builtin cardinal rule exists for pt-BR");
		ASSERT_TRUE(CI18nManager::GetBuiltinCardinalRule("pt") != nullptr,
					"Builtin cardinal rule resolves for generic pt");
		ASSERT_TRUE(ptBRCard(0.0, 0, 0, 0, 0, 0) == I18N_PLURAL_ONE, "PT-BR cardinal: 0 -> one");
		ASSERT_TRUE(ptBRCard(1.0, 1, 0, 0, 0, 0) == I18N_PLURAL_ONE, "PT-BR cardinal: 1 -> one");
		ASSERT_TRUE(ptBRCard(2.0, 2, 0, 0, 0, 0) == I18N_PLURAL_OTHER, "PT-BR cardinal: 2 -> other");
		ASSERT_TRUE(ptBRCard(1000000.0, 1000000, 0, 0, 0, 0) == I18N_PLURAL_MANY,
					"PT-BR cardinal: 1000000 -> many");

		EI18nPluralRuleFn nlCard = CI18nManager::GetBuiltinCardinalRule("nl");
		ASSERT_TRUE(nlCard != nullptr, "Builtin cardinal rule exists for nl");
		ASSERT_TRUE(CI18nManager::GetBuiltinCardinalRule("nl-NL") != nullptr,
					"Builtin cardinal rule resolves for nl-NL");
		ASSERT_TRUE(nlCard(1.0, 1, 0, 0, 0, 0) == I18N_PLURAL_ONE, "NL cardinal: 1 -> one");
		ASSERT_TRUE(nlCard(1.0, 1, 1, 0, 0, 0) == I18N_PLURAL_OTHER, "NL cardinal: 1.0 -> other");
		ASSERT_TRUE(nlCard(2.0, 2, 0, 0, 0, 0) == I18N_PLURAL_OTHER, "NL cardinal: 2 -> other");

		EI18nPluralRuleFn csCard = CI18nManager::GetBuiltinCardinalRule("cs");
		ASSERT_TRUE(csCard != nullptr, "Builtin cardinal rule exists for cs");
		ASSERT_TRUE(csCard(1.0, 1, 0, 0, 0, 0) == I18N_PLURAL_ONE, "CS cardinal: 1 -> one");
		ASSERT_TRUE(csCard(2.0, 2, 0, 0, 0, 0) == I18N_PLURAL_FEW, "CS cardinal: 2 -> few");
		ASSERT_TRUE(csCard(4.0, 4, 0, 0, 0, 0) == I18N_PLURAL_FEW, "CS cardinal: 4 -> few");
		ASSERT_TRUE(csCard(5.0, 5, 0, 0, 0, 0) == I18N_PLURAL_OTHER, "CS cardinal: 5 -> other");
		ASSERT_TRUE(csCard(1.5, 1, 1, 1, 5, 5) == I18N_PLURAL_MANY, "CS cardinal: 1.5 -> many");

		EI18nPluralRuleFn trCard = CI18nManager::GetBuiltinCardinalRule("tr");
		ASSERT_TRUE(trCard != nullptr, "Builtin cardinal rule exists for tr");
		ASSERT_TRUE(CI18nManager::GetBuiltinCardinalRule("tr-TR") != nullptr,
					"Builtin cardinal rule resolves for tr-TR");
		ASSERT_TRUE(trCard(1.0, 1, 0, 0, 0, 0) == I18N_PLURAL_ONE, "TR cardinal: 1 -> one");
		ASSERT_TRUE(trCard(2.0, 2, 0, 0, 0, 0) == I18N_PLURAL_OTHER, "TR cardinal: 2 -> other");

		// Only en/it/fr have ordinal rules; the rest are "other" for all counts.
		ASSERT_TRUE(CI18nManager::GetBuiltinOrdinalRule("es") == nullptr, "ES has no dedicated ordinal rule");
		ASSERT_TRUE(CI18nManager::GetBuiltinOrdinalRule("pt-BR") == nullptr, "PT-BR has no dedicated ordinal rule");
		ASSERT_TRUE(CI18nManager::GetBuiltinOrdinalRule("nl") == nullptr, "NL has no dedicated ordinal rule");
		ASSERT_TRUE(CI18nManager::GetBuiltinOrdinalRule("cs") == nullptr, "CS has no dedicated ordinal rule");
		ASSERT_TRUE(CI18nManager::GetBuiltinOrdinalRule("tr") == nullptr, "TR has no dedicated ordinal rule");

		ASSERT_TRUE(CI18nManager::GetBuiltinCardinalRule("xx") == nullptr,
					"Builtin cardinal rule absent for unknown language");
	}

	// --- Test 5: PluralCategoryName ---
	ASSERT_EQ(string(CI18nManager::PluralCategoryName(I18N_PLURAL_ZERO)), string("zero"), "PluralCategoryName zero");
	ASSERT_EQ(string(CI18nManager::PluralCategoryName(I18N_PLURAL_ONE)), string("one"), "PluralCategoryName one");
	ASSERT_EQ(string(CI18nManager::PluralCategoryName(I18N_PLURAL_OTHER)), string("other"), "PluralCategoryName other");

	// --- Test 6: Simple format substitution ---
	mgr->SetActiveLocale("en");
	{
		map<string, SI18nArg> args;
		args.insert({"name", SI18nArg("World")});
		string r = mgr->Format("Hello, {name}!", args);
		ASSERT_EQ(r, string("Hello, World!"), "Simple format substitution");
	}

	// --- Test 7: Plural format (English) ---
	mgr->SetActiveLocale("en");
	{
		map<string, SI18nArg> args;
		args.insert({"count", SI18nArg(1)});
		string r = mgr->Format("{count, plural, one {# item} other {# items}}", args);
		ASSERT_EQ(r, string("1 item"), "EN plural format count=1");
	}
	{
		map<string, SI18nArg> args;
		args.insert({"count", SI18nArg(5)});
		string r = mgr->Format("{count, plural, one {# item} other {# items}}", args);
		ASSERT_EQ(r, string("5 items"), "EN plural format count=5");
	}

	// --- Test 8: Plural format (Polish) ---
	mgr->SetActiveLocale("pl");
	{
		map<string, SI18nArg> args;
		args.insert({"count", SI18nArg(1)});
		string r = mgr->Format("{count, plural, one {# przedmiot} few {# przedmioty} many {# przedmiotów} other {# przedmiotów}}", args);
		ASSERT_EQ(r, string("1 przedmiot"), "PL plural format count=1");
	}
	{
		map<string, SI18nArg> args;
		args.insert({"count", SI18nArg(3)});
		string r = mgr->Format("{count, plural, one {# przedmiot} few {# przedmioty} many {# przedmiotów} other {# przedmiotów}}", args);
		ASSERT_EQ(r, string("3 przedmioty"), "PL plural format count=3");
	}
	{
		map<string, SI18nArg> args;
		args.insert({"count", SI18nArg(5)});
		string r = mgr->Format("{count, plural, one {# przedmiot} few {# przedmioty} many {# przedmiotów} other {# przedmiotów}}", args);
		ASSERT_EQ(r, string("5 przedmiotów"), "PL plural format count=5");
	}

	// --- Test 9: Select format ---
	mgr->SetActiveLocale("en");
	{
		map<string, SI18nArg> args;
		args.insert({"gender", SI18nArg("masculine")});
		string r = mgr->Format("{gender, select, masculine {He} feminine {She} other {They}}", args);
		ASSERT_EQ(r, string("He"), "Select masculine");
	}
	{
		map<string, SI18nArg> args;
		args.insert({"gender", SI18nArg("feminine")});
		string r = mgr->Format("{gender, select, masculine {He} feminine {She} other {They}}", args);
		ASSERT_EQ(r, string("She"), "Select feminine");
	}
	{
		map<string, SI18nArg> args;
		args.insert({"gender", SI18nArg("neuter")});
		string r = mgr->Format("{gender, select, masculine {He} feminine {She} other {They}}", args);
		ASSERT_EQ(r, string("They"), "Select other (neuter)");
	}

	// --- Test 10: Exact match in plural ---
	mgr->SetActiveLocale("en");
	{
		map<string, SI18nArg> args;
		args.insert({"count", SI18nArg(0)});
		string r = mgr->Format("{count, plural, =0 {none} =1 {exactly one} other {# items}}", args);
		ASSERT_EQ(r, string("none"), "Plural exact match =0");
	}
	{
		map<string, SI18nArg> args;
		args.insert({"count", SI18nArg(1)});
		string r = mgr->Format("{count, plural, =0 {none} =1 {exactly one} other {# items}}", args);
		ASSERT_EQ(r, string("exactly one"), "Plural exact match =1");
	}

	// --- Test 11: Number formatting ---
	mgr->SetActiveLocale("en");
	{
		string r = mgr->FormatNumber(1234567);
		ASSERT_EQ(r, string("1,234,567"), "EN number format 1234567");
	}
	mgr->SetActiveLocale("pl");
	{
		string r = mgr->FormatNumber(1234567);
		// Polish uses space as thousands separator
		string expected = "1\xC2\xA0""234\xC2\xA0""567";  // non-breaking space
		// Actually, depending on implementation it might use regular space
		// Let's check both
		bool ok = (r == "1 234 567" || r == expected);
		ASSERT_TRUE(ok, "PL number format 1234567");
	}

	// --- Test 12: SI18nArg CLDR operands ---
	{
		SI18nArg intArg(42);
		double n; int i, v, w, f, t;
		intArg.GetPluralOperands(n, i, v, w, f, t);
		ASSERT_TRUE(i == 42, "SI18nArg int operand i=42");
		ASSERT_TRUE(v == 0, "SI18nArg int operand v=0");
	}

	// --- Test 13: Multiple arguments ---
	mgr->SetActiveLocale("en");
	{
		map<string, SI18nArg> args;
		args.insert({"name", SI18nArg("Hero")});
		args.insert({"count", SI18nArg(3)});
		string r = mgr->Format("{name} found {count, plural, one {# item} other {# items}}", args);
		ASSERT_EQ(r, string("Hero found 3 items"), "Multiple arguments in format");
	}

	// --- Test 14: SetString and lookup ---
	mgr->SetString("en", "test.dynamic", "Dynamic Value");
	ASSERT_EQ(string(mgr->Get("test.dynamic", "en")), string("Dynamic Value"), "SetString and Get");

	// --- Test 15: Locale switching ---
	mgr->SetActiveLocale("en");
	ASSERT_EQ(mgr->GetActiveLocale(), string("en"), "Active locale is en");
	mgr->SetActiveLocale("pl");
	ASSERT_EQ(mgr->GetActiveLocale(), string("pl"), "Active locale is pl");

	// Restore original locale
	mgr->SetActiveLocale("en");

	LOGD("CTestI18nEngine: All %d steps passed", stepNum - 1);
	TestCompleted(true, "All CI18nManager engine tests passed");
}

void CTestI18nEngine::Cancel()
{
	isRunning = false;
}

void CTestI18nEngine::Teardown()
{
	// Restore English as active locale
	CI18nManager::Instance()->SetActiveLocale("en");
}
