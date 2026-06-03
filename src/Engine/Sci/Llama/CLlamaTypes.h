#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct MT_LlamaLoadParams {
	// Context window size in tokens. Larger values allow longer conversations
	// but require more VRAM/RAM. Must be at least as large as the longest prompt + response.
	int32_t n_ctx = 8192; //512; //131072

	// Number of CPU threads for inference. 0 = let the backend choose automatically.
	int32_t n_threads = 0;

	// Number of model layers to offload to GPU. -1 = all layers (full GPU acceleration
	// via Metal on macOS or CUDA on Windows/Linux). 0 = CPU-only inference.
	int32_t n_gpu_layers = -1;
};

struct MT_LlamaGenerateParams {
	// Maximum number of tokens to generate in a single response.
	// One token is roughly 0.75 words. 0 = process prompt only (no generation).
	int32_t max_tokens = 1024;

	// Random seed for sampling. 0xFFFFFFFF = random seed each run.
	// Set a fixed value to get reproducible outputs.
	uint32_t seed = 0xFFFFFFFFu;

	// Sampling temperature. Controls randomness of the output.
	// 0.0 = deterministic (always picks the most likely token),
	// 1.0 = full randomness. Values around 0.7-0.9 work well for chat.
	float temperature = 0.8f;

	// Top-K sampling: keep only the K most likely tokens at each step.
	// 0 = disabled (no top-k filtering).
	int32_t top_k = 40;

	// Top-P (nucleus) sampling: keep the smallest set of tokens whose cumulative
	// probability exceeds P. 1.0 = disabled.
	float top_p = 0.95f;

	// Min-P sampling: keep tokens with probability >= min_p * (probability of top token).
	// 0.0 = disabled.
	float min_p = 0.05f;

	// Repetition penalty. 1.0 = disabled; 1.1 is a typical default that prevents
	// "own own own..." loops. Applied to the last repeat_last_n generated tokens.
	float repeat_penalty = 1.1f;

	// How many recent tokens to check for repetition. 64 is a common default.
	int32_t repeat_last_n = 64;

	// DRY sampler: exponential penalty for repeated token sequences.
	// Catches loops that repeat_penalty alone misses (e.g. "own own own...").
	// 0.0 = disabled.
	float dry_multiplier = 0.8f;

	// DRY exponential base. Higher = harsher penalty for longer repeated runs.
	float dry_base = 1.75f;

	// DRY: token sequences up to this length are exempt from penalty.
	int32_t dry_allowed_length = 2;

	// DRY: how many recent tokens to scan. -1 = full context.
	int32_t dry_penalty_last_n = -1;

	// Stop generation when any of these strings appear in the output.
	// The stop string itself is NOT included in the output.
	// Typically set automatically from the model's chat template (e.g. "<|im_end|>").
	std::vector<std::string> stopSequences;
};

// Reason why generation stopped
enum class MT_LlamaStopReason {
	None,
	EndOfTurn,     // reached EOG token
	MaxTokens,     // reached user defined limit
	StopSequence,  // user-provided stop string (e.g. <|im_end|>)
	UserStop       // user clicked Stop
};

using MT_LlamaTokenCallback = std::function<void(const std::string &token)>;

// ─── Structured segment API ───────────────────────────────────────────────────

// Segment type returned by the structured generation API.
// Thinking: model's internal reasoning (from <think> blocks or non-final channels).
// Answer:   the final response intended for the user.
enum class MT_LlamaSegment { Thinking, Answer };

// Called during streaming with each new text delta tagged with its segment type.
using MT_LlamaSegmentCallback = std::function<void(MT_LlamaSegment, const std::string &text)>;

// Complete result returned when GenerateWithSegmentsAsync finishes.
struct MT_LlamaParseResult {
	std::string thinking; // all thinking content concatenated
	std::string answer;   // final answer content
	MT_LlamaStopReason stopReason = MT_LlamaStopReason::None;
};
