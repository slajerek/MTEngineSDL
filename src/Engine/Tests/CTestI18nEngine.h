#pragma once

#include "CTest.h"

// Generic CI18nManager engine test — tests locale registration,
// string lookup with fallback, MessageFormat (plural/select/selectordinal),
// number formatting, and CLDR plural operands.
class CTestI18nEngine : public CTest
{
public:
	CTestI18nEngine();
	virtual ~CTestI18nEngine();

	virtual const char *GetName() override { return "I18nEngine"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
	virtual void Teardown() override;
};
