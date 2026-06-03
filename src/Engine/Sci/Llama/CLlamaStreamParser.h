#pragma once

// Header-only streaming parser for structured LLM output.
// Handles <think>...</think> and <|channel|>NAME<|message|>...<|end|> formats.

#include "CLlamaTypes.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// ─── Internal segment type ───────────────────────────────────────────────────

struct LlamaTextSegment {
	bool isThinking;
	std::string text;
};

// ─── ParseThinkSegments ──────────────────────────────────────────────────────

// Splits raw LLM output into thinking/regular segments.
// Handled formats:
//   <think>...</think>                   — thinking block (Qwen-style)
//   <|channel|>NAME<|message|>...<|end|> — non-"final" channels treated as thinking
//   <|start|>...                         — navigation tokens, skipped silently
// Incomplete (streaming) blocks at end of input are shown as in-progress.
// Empty or whitespace-only blocks are dropped.
inline std::vector<LlamaTextSegment> ParseThinkSegments(const std::string &raw)
{
	std::vector<LlamaTextSegment> result;
	size_t pos = 0;

	auto hasNonWhitespace = [](const std::string &s) {
		for (char c : s) if (!isspace((unsigned char)c)) return true;
		return false;
	};

	auto addSeg = [&](bool thinking, std::string text) {
		while (!text.empty() && (text.front() == '\n' || text.front() == '\r'))
			text.erase(0, 1);
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
			text.pop_back();
		if (hasNonWhitespace(text))
			result.push_back({thinking, std::move(text)});
	};

	while (pos < raw.size())
	{
		// --- <think>...</think> ---
		if (raw.compare(pos, 7, "<think>") == 0)
		{
			pos += 7;
			size_t end = raw.find("</think>", pos);
			if (end == std::string::npos)
			{
				addSeg(true, raw.substr(pos)); // streaming: incomplete
				return result;
			}
			addSeg(true, raw.substr(pos, end - pos));
			pos = end + 8;
			continue;
		}

		// --- <|channel|>NAME<|message|>CONTENT<|end|> ---
		if (raw.compare(pos, 11, "<|channel|>") == 0)
		{
			pos += 11;
			size_t msgPos = raw.find("<|message|>", pos);
			if (msgPos == std::string::npos) break; // incomplete channel open tag
			std::string channelName = raw.substr(pos, msgPos - pos);
			bool isFinal = (channelName == "final");
			pos = msgPos + 11;

			size_t endPos = raw.find("<|end|>", pos);
			if (endPos == std::string::npos)
			{
				addSeg(!isFinal, raw.substr(pos)); // streaming: incomplete
				return result;
			}
			addSeg(!isFinal, raw.substr(pos, endPos - pos));
			pos = endPos + 7;
			continue;
		}

		// --- <|start|> — navigation token, skip to next <|channel|> ---
		if (raw.compare(pos, 9, "<|start|>") == 0)
		{
			size_t next = raw.find("<|channel|>", pos);
			pos = (next != std::string::npos) ? next : raw.size();
			continue;
		}

		// --- Regular text: consume up to the next known tag opener ---
		size_t nextTag = raw.size();
		for (const char *tag : {"<think>", "<|channel|>", "<|start|>"})
		{
			size_t found = raw.find(tag, pos);
			if (found != std::string::npos && found < nextTag)
				nextTag = found;
		}
		if (nextTag > pos)
			addSeg(false, raw.substr(pos, nextTag - pos));
		pos = nextTag;
	}

	return result;
}

// ─── CLlamaStreamParser ──────────────────────────────────────────────────────

// Streaming parser that wraps ParseThinkSegments with incremental emit tracking.
// Feed() accumulates raw token chunks and fires segmentCb for each new text delta.
// Finish() flushes any remaining buffered text and returns the complete parse result.
class CLlamaStreamParser
{
public:
	// When the prompt already ends with "<think>\n" (prefilled thinking mode),
	// call this before Feed() so the parser treats initial output as thinking
	// content even though the model won't emit an opening <think> tag.
	void SetStartInThinkingMode(bool v) { startInThinkingMode = v; }

	void Feed(const std::string &chunk, MT_LlamaSegmentCallback segmentCb)
	{
		rawBuffer += chunk;
		EmitNewSegments(SafeLen(rawBuffer), segmentCb);
	}

	MT_LlamaParseResult Finish(MT_LlamaSegmentCallback segmentCb)
	{
		EmitNewSegments(rawBuffer.size(), segmentCb);
		return {allThinking, allAnswer};
	}

private:
	std::string rawBuffer;
	std::string allThinking;
	std::string allAnswer;
	bool startInThinkingMode = false;

	// Returns the number of bytes from the start of buf that are safe to parse,
	// i.e. the tail cannot be a partial prefix of any known tag opener.
	static size_t SafeLen(const std::string &buf)
	{
		static const char *kTags[] = {
			"<think>", "</think>", "<|channel|>", "<|message|>", "<|end|>", "<|start|>"
		};
		size_t holdBack = 0;
		for (const char *tag : kTags)
		{
			size_t tagLen = std::strlen(tag);
			size_t checkLen = std::min(tagLen - 1, buf.size());
			for (size_t prefLen = checkLen; prefLen > 0; prefLen--)
			{
				// Check: do the last prefLen chars of buf match the first prefLen chars of tag?
				if (buf.compare(buf.size() - prefLen, prefLen, tag, prefLen) == 0)
				{
					if (prefLen > holdBack) holdBack = prefLen;
					break;
				}
			}
		}
		return buf.size() > holdBack ? buf.size() - holdBack : 0;
	}

	void EmitNewSegments(size_t safeLen, MT_LlamaSegmentCallback &segmentCb)
	{
		if (safeLen == 0) return;

		std::string safe = rawBuffer.substr(0, safeLen);

		// When the prompt already prefilled "<think>\n", the model output starts
		// inside a thinking block but without an opening tag. Prepend it so
		// ParseThinkSegments correctly classifies the initial content as thinking.
		if (startInThinkingMode)
			safe = "<think>" + safe;

		auto segments = ParseThinkSegments(safe);

		// Concatenate all thinking and answer text from segments
		std::string newThinking, newAnswer;
		for (const auto &seg : segments)
		{
			if (seg.isThinking) newThinking += seg.text;
			else                newAnswer   += seg.text;
		}

		// Emit only the delta since last emit
		if (newThinking.size() > allThinking.size())
		{
			std::string delta = newThinking.substr(allThinking.size());
			allThinking = std::move(newThinking);
			if (segmentCb) segmentCb(MT_LlamaSegment::Thinking, delta);
		}
		if (newAnswer.size() > allAnswer.size())
		{
			std::string delta = newAnswer.substr(allAnswer.size());
			allAnswer = std::move(newAnswer);
			if (segmentCb) segmentCb(MT_LlamaSegment::Answer, delta);
		}
	}
};
