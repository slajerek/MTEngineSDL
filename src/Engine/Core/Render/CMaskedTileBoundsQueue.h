#ifndef _CMaskedTileBoundsQueue_h_
#define _CMaskedTileBoundsQueue_h_

#include <vector>

// The queued masked-tile shader's per-frame bounds queue.
//
// Hoisted out of CRenderShaderMaskedTileQueued so the Metal port shares it
// rather than carrying a second copy. The logic is entirely
// backend-independent -- a push/pop of plain floats and an opaque texture
// handle -- and the subtle part is the LIFETIME, which is the same on both
// backends and easy to get wrong twice:
//
//   ImGui draw callbacks are DEFERRED. Pop() runs inside the callback, long
//   after Push() returned, so the queue must survive until the draw lists are
//   walked. That is why Clear() belongs to BeginBatch() at the start of a frame
//   and NOT to ResetState(), which runs while entries are still unconsumed.
struct CMaskedTileBounds
{
	void *maskTexture;
	float px, py, sx, sy;
};

class CMaskedTileBoundsQueue
{
public:
	CMaskedTileBoundsQueue() : readIndex(0) {}

	void Clear()
	{
		entries.clear();
		readIndex = 0;
	}

	void Push(void *maskTexture, float px, float py, float sx, float sy)
	{
		CMaskedTileBounds b = { maskTexture, px, py, sx, sy };
		entries.push_back(b);
	}

	// Returns NULL when the queue is exhausted, which callers treat as "fall
	// back to the member variables set through SetMaskTexture/SetTileBounds" --
	// the single-tile path.
	const CMaskedTileBounds *Pop()
	{
		if (readIndex >= (int)entries.size())
			return NULL;
		return &entries[readIndex++];
	}

private:
	std::vector<CMaskedTileBounds> entries;
	int readIndex;
};

#endif
