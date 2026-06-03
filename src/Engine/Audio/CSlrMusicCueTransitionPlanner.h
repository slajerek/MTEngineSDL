#pragma once

#include <cstdint>

struct CSlrMusicCueTransitionPlan
{
	// Wait this many samples before starting incoming track.
	int64_t startDelaySamples = 0;

	// Incoming track starting sample offset.
	int64_t incomingStartSample = 0;

	// Outgoing sample position where cues align.
	int64_t alignmentOutgoingSample = 0;

	// Incoming sample position where cues align.
	int64_t alignmentIncomingSample = 0;
};

class CSlrMusicCueTransitionPlanner
{
public:
	static int64_t FirstTrackStartSample();

	// Plan overlap so outgoing cue-out aligns with incoming cue-in.
	// currentOutgoingSample: currently played sample position in outgoing track.
	// outgoingCueOutSample: designated outgoing cue out.
	// incomingCueInSample: designated incoming cue in.
	static CSlrMusicCueTransitionPlan PlanTransition(int64_t currentOutgoingSample,
											 int64_t outgoingCueOutSample,
											 int64_t incomingCueInSample);
};
