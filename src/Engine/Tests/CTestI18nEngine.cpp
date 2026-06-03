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
