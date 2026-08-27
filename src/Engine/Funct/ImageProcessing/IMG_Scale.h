#include "SYS_Defs.h"
#include "CImageData.h"

#include <atomic>

// Options for the IMG_Scale resampler (spec A3 #4.1.1).
struct IMG_ScaleOptions
{
	float gamma = 1.75f;               // resample working-space gamma; 1.75 =
	                                    // today's mipmap default. Export passes ~2.2.
	const std::atomic<bool> *cancelFlag = nullptr; // polled once per OUTPUT ROW via load();
	                                    // non-null + true aborts and returns NULL.
	                                    // nullptr = uncancellable. An ATOMIC, not a
	                                    // volatile bool: IMG_Scale runs synchronously
	                                    // on the executor thread while Cancel() is set
	                                    // from another thread -- a plain load must be
	                                    // a real atomic load to be observed mid-scale.
	                                    // The executor passes its existing &cancel_
	                                    // (std::atomic<bool>) directly.
	std::atomic<bool> *startedFlag = nullptr; // test-only observability: set true on
	                                    // the row loop's first output row. Null in
	                                    // production; harmless when set.
};

CImageData *IMG_Scale(CImageData *imgIn, float scaleX, float scaleY, bool isSheet);
CImageData *IMG_Scale(CImageData *imgIn, int destWidth, int destHeight);
CImageData *IMG_Scale(CImageData *imgIn, int destWidth, int destHeight,
                      const IMG_ScaleOptions &opts);

void IMG_ScaleShrinkHalfWidth(CImageData *imgIn, CImageData *imgOut);
