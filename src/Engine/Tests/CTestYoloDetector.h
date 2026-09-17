#pragma once

#include "CTest.h"

// ENGINE-owned test for the object detector and its model catalogue.
//
// Two of its assertions are worth naming, because they exist to catch specific
// failures rather than to raise the count:
//
//   THE LICENCE POLICY IS A TEST, NOT A COMMENT. Every shipped catalogue row
//   must carry a permissive licence. Every Ultralytics YOLO is AGPL-3.0, which
//   obliges the whole application that uses it to be open-sourced -- so a row
//   added later under that licence must fail here rather than quietly put that
//   obligation on every host that copies the template.
//
//   THE DECODE IS ASSERTED AGAINST COORDINATES. The YOLOX head is raw: a
//   decoder that forgets the grid and stride step still returns boxes, and they
//   look plausible. Only checking WHERE a box lands catches it, so the test
//   draws one object at a known position and asserts the detector finds it
//   there. That half needs a model file and reports SKIPPED without one.
class CTestYoloDetector : public CTest
{
public:
	CTestYoloDetector();
	virtual ~CTestYoloDetector();

	virtual const char *GetName() override { return "YoloDetector"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
};
