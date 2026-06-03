#include "CSlrMusicCueTransitionPlanner.h"

int64_t CSlrMusicCueTransitionPlanner::FirstTrackStartSample()
{
	return 0;
}

// Plan overlap so outgoing cue-out aligns with incoming cue-in.
// currentOutgoingSample: currently played sample position in outgoing track.
// outgoingCueOutSample: designated outgoing cue out.
// incomingCueInSample: designated incoming cue in.
CSlrMusicCueTransitionPlan CSlrMusicCueTransitionPlanner::PlanTransition(int64_t currentOutgoingSample,
									 int64_t outgoingCueOutSample,
									 int64_t incomingCueInSample)
{
	CSlrMusicCueTransitionPlan plan;

	if (currentOutgoingSample < 0)
		currentOutgoingSample = 0;
	if (outgoingCueOutSample < currentOutgoingSample)
		outgoingCueOutSample = currentOutgoingSample;
	if (incomingCueInSample < 0)
		incomingCueInSample = 0;

	int64_t remainingOutgoingSamples = outgoingCueOutSample - currentOutgoingSample;

	if (incomingCueInSample <= remainingOutgoingSamples)
	{
		plan.startDelaySamples = remainingOutgoingSamples - incomingCueInSample;
		plan.incomingStartSample = 0;
	}
	else
	{
		plan.startDelaySamples = 0;
		plan.incomingStartSample = incomingCueInSample - remainingOutgoingSamples;
	}

	plan.alignmentOutgoingSample = outgoingCueOutSample;
	plan.alignmentIncomingSample = incomingCueInSample;
	return plan;
}
