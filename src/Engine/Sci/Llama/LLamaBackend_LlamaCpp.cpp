#include "LLamaBackend_LlamaCpp.h"

#include <algorithm>
#include <mutex>

#include "llama.h"
#include "DBG_Log.h"

static std::once_flag s_llamaBackendInitOnce;

LLamaBackend_LlamaCpp::LLamaBackend_LlamaCpp() {}

LLamaBackend_LlamaCpp::~LLamaBackend_LlamaCpp()
{
	CancelLoadModel();
	StopGeneration();
	UnloadModel();
}

bool LLamaBackend_LlamaCpp::IsAvailable() const
{
	return true;
}

std::string LLamaBackend_LlamaCpp::GetBackendName() const
{
	return activeBackendName;
}

bool LLamaBackend_LlamaCpp::TryLoadModel(const std::string &modelPath, const MT_LlamaLoadParams &params, std::string *errorOut)
{
	StopGeneration();
	UnloadModel();

	std::call_once(s_llamaBackendInitOnce, []() {
		// Suppress llama.cpp/ggml verbose stdout output (e.g. ggml_metal_log_allocated_size)
		llama_log_set([](enum ggml_log_level level, const char *text, void *) {
			(void)level;
			(void)text;
			// Silenced: llama.cpp internal logs no longer printed to stdout
		}, nullptr);
		llama_backend_init();
	});

	llama_model_params mparams = llama_model_default_params();
	mparams.n_gpu_layers = params.n_gpu_layers;

	llama_context_params cparams = llama_context_default_params();

	if (params.n_ctx > 0)
		cparams.n_ctx = params.n_ctx;

	model = llama_model_load_from_file(modelPath.c_str(), mparams);
	if (!model)
	{
		if (errorOut)
			*errorOut = "Failed to load model: " + modelPath;
		return false;
	}

	ctx = llama_init_from_model(model, cparams);
	if (!ctx)
	{
		llama_model_free(model);
		model = nullptr;
		if (errorOut)
			*errorOut = "Failed to init context from model";
		return false;
	}

	n_past = 0;

	if (params.n_gpu_layers != 0 && llama_supports_gpu_offload())
	{
#if defined(__APPLE__)
		activeBackendName = "llama.cpp (metal)";
#else
		activeBackendName = "llama.cpp (gpu)";
#endif
	}
	else
	{
		activeBackendName = "llama.cpp (cpu)";
	}

	return true;
}

void LLamaBackend_LlamaCpp::UnloadModel()
{
	if (loadingModel)
		CancelLoadModel();

	// Stop any ongoing generation and wait for the inference thread to exit
	// before freeing the context (prevents use-after-free in InferenceThread)
	if (generating)
		StopGeneration();

	{
		std::lock_guard<std::mutex> lock(ctxMutex);
		if (ctx)
		{
			llama_free(ctx);
			ctx = nullptr;
		}
	}
	if (model)
	{
		llama_model_free(model);
		model = nullptr;
	}
	n_past = 0;
}

bool LLamaBackend_LlamaCpp::IsGpuOffloadSupported() const
{
	return llama_supports_gpu_offload();
}

bool LLamaBackend_LlamaCpp::HasModelLoaded() const
{
	return model != nullptr && ctx != nullptr;
}

bool LLamaBackend_LlamaCpp::GenerateAsync(const std::string &prompt,
                                           const MT_LlamaGenerateParams &params,
                                           MT_LlamaTokenCallback tokenCb,
                                           std::function<void(MT_LlamaStopReason)> doneCb)
{
	if (!HasModelLoaded()) return false;
	if (generating) return false;

	if (inferenceThread.joinable())
		inferenceThread.join();

	stopRequested = false;
	generating = true;
	inferenceThread = std::thread(&LLamaBackend_LlamaCpp::InferenceThread,
	                              this, prompt, params, tokenCb, doneCb);
	return true;
}

bool LLamaBackend_LlamaCpp::IsGenerating() const
{
	return generating;
}

void LLamaBackend_LlamaCpp::StopGeneration()
{
	stopRequested = true;
	if (inferenceThread.joinable())
		inferenceThread.join();
	stopRequested = false;

	// Clear the recurrent memory state after an interrupted generation.
	// Without this, the recurrent memory (Mamba/RWKV/hybrid models) is left
	// in an inconsistent state, causing GGML_ASSERT failures on the next decode.
	// llama_memory_clear resets cells (pos, seq_id, src, tail), head, used, and buffer data.
	ClearContext();
}

void LLamaBackend_LlamaCpp::ClearContext()
{
	std::lock_guard<std::mutex> lock(ctxMutex);
	if (ctx)
		llama_memory_clear(llama_get_memory(ctx), true);
	n_past = 0;
}

std::vector<int32_t> LLamaBackend_LlamaCpp::Tokenize(const std::string &text, bool addBos, bool parseSpecial)
{
	const llama_vocab *vocab = llama_model_get_vocab(model);
	int nMax = (int)text.size() + 64;
	std::vector<int32_t> tokens(nMax);
	int n = llama_tokenize(vocab, text.c_str(), (int)text.size(),
	                       tokens.data(), nMax, addBos, parseSpecial);
	if (n < 0)
	{
		tokens.resize(-n);
		n = llama_tokenize(vocab, text.c_str(), (int)text.size(),
		                   tokens.data(), -n, addBos, parseSpecial);
	}
	tokens.resize(n > 0 ? n : 0);
	return tokens;
}

// ─── Async model loading ─────────────────────────────────────────────────────

bool LLamaBackend_LlamaCpp::LlamaProgressCallback(float progress, void *userData)
{
	auto *self = static_cast<LLamaBackend_LlamaCpp *>(userData);
	self->loadProgress = progress;
	return !self->cancelLoadRequested.load();
}

bool LLamaBackend_LlamaCpp::TryLoadModelAsync(const std::string &modelPath, const MT_LlamaLoadParams &params)
{
	if (loadingModel || generating)
		return false;

	if (loadThread.joinable())
		loadThread.join();

	StopGeneration();
	UnloadModel();

	loadProgress = 0.0f;
	cancelLoadRequested = false;
	loadingModel = true;
	{
		std::lock_guard<std::mutex> lk(loadErrorMutex);
		loadError.clear();
	}

	loadThread = std::thread(&LLamaBackend_LlamaCpp::LoadModelThread, this, modelPath, params);
	return true;
}

bool LLamaBackend_LlamaCpp::IsLoadingModel() const
{
	return loadingModel;
}

float LLamaBackend_LlamaCpp::GetLoadProgress() const
{
	return loadProgress;
}

void LLamaBackend_LlamaCpp::CancelLoadModel()
{
	cancelLoadRequested = true;
	if (loadThread.joinable())
		loadThread.join();
	cancelLoadRequested = false;
	loadingModel = false;
}

std::string LLamaBackend_LlamaCpp::GetLoadError() const
{
	std::lock_guard<std::mutex> lk(const_cast<std::mutex &>(loadErrorMutex));
	return loadError;
}

void LLamaBackend_LlamaCpp::LoadModelThread(std::string modelPath, MT_LlamaLoadParams params)
{
	std::call_once(s_llamaBackendInitOnce, []() {
		llama_log_set([](enum ggml_log_level level, const char *text, void *) {
			(void)level;
			(void)text;
		}, nullptr);
		llama_backend_init();
	});

	llama_model_params mparams = llama_model_default_params();
	mparams.n_gpu_layers = params.n_gpu_layers;
	mparams.progress_callback = LlamaProgressCallback;
	mparams.progress_callback_user_data = this;

	llama_model *m = llama_model_load_from_file(modelPath.c_str(), mparams);
	if (!m)
	{
		std::lock_guard<std::mutex> lk(loadErrorMutex);
		if (cancelLoadRequested)
			loadError = "Model loading cancelled";
		else
			loadError = "Failed to load model: " + modelPath;
		loadingModel = false;
		return;
	}

	llama_context_params cparams = llama_context_default_params();
	if (params.n_ctx > 0)
		cparams.n_ctx = params.n_ctx;

	llama_context *c = llama_init_from_model(m, cparams);
	if (!c)
	{
		llama_model_free(m);
		std::lock_guard<std::mutex> lk(loadErrorMutex);
		loadError = "Failed to init context from model";
		loadingModel = false;
		return;
	}

	// Success — commit to instance state
	model = m;
	ctx = c;
	n_past = 0;

	if (params.n_gpu_layers != 0 && llama_supports_gpu_offload())
	{
#if defined(__APPLE__)
		activeBackendName = "llama.cpp (metal)";
#else
		activeBackendName = "llama.cpp (gpu)";
#endif
	}
	else
	{
		activeBackendName = "llama.cpp (cpu)";
	}

	loadProgress = 1.0f;
	loadingModel = false;
}

// ─── Chat template ───────────────────────────────────────────────────────────

std::string LLamaBackend_LlamaCpp::ApplyChatTemplate(
	const std::vector<std::pair<std::string,std::string>> &messages, bool addAssistantPrefix) const
{
	if (!model) return "";

	const char *tmpl = llama_model_chat_template(model, nullptr);
	if (!tmpl) return "";

	// Build llama_chat_message array
	std::vector<llama_chat_message> chatMsgs(messages.size());
	for (size_t i = 0; i < messages.size(); i++)
	{
		chatMsgs[i].role    = messages[i].first.c_str();
		chatMsgs[i].content = messages[i].second.c_str();
	}

	// First call to measure required size
	int32_t needed = llama_chat_apply_template(
		tmpl, chatMsgs.data(), chatMsgs.size(), addAssistantPrefix, nullptr, 0);
	if (needed <= 0) return "";

	std::string buf(needed + 1, '\0');
	int32_t written = llama_chat_apply_template(
		tmpl, chatMsgs.data(), chatMsgs.size(), addAssistantPrefix, buf.data(), (int32_t)buf.size());
	if (written <= 0) return "";

	buf.resize(written);
	return buf;
}

std::string LLamaBackend_LlamaCpp::GetChatStopSequence() const
{
	if (!model) return "";

	const char *tmpl = llama_model_chat_template(model, nullptr);
	if (!tmpl) return "";

	// Format a dummy assistant message to discover the end-of-turn delimiter.
	// The stop sequence is whatever comes after the content in the formatted output.
	const std::string marker = "___DUMMY___";
	llama_chat_message msg = { "assistant", marker.c_str() };

	int32_t needed = llama_chat_apply_template(tmpl, &msg, 1, false, nullptr, 0);
	if (needed <= 0) return "";

	std::string buf(needed + 1, '\0');
	int32_t written = llama_chat_apply_template(tmpl, &msg, 1, false, buf.data(), (int32_t)buf.size());
	if (written <= 0) return "";
	buf.resize(written);

	// Find the marker and return everything after it
	size_t pos = buf.find(marker);
	if (pos == std::string::npos) return "";

	std::string suffix = buf.substr(pos + marker.size());
	// Trim trailing whitespace (newlines after the delimiter are not part of content)
	while (!suffix.empty() && (suffix.back() == '\n' || suffix.back() == '\r'))
		suffix.pop_back();

	return suffix;
}

std::vector<std::pair<int32_t, std::string>> LLamaBackend_LlamaCpp::GetRawTokens()
{
	std::lock_guard<std::mutex> lock(rawTokensMutex);
	return rawTokens;
}

void LLamaBackend_LlamaCpp::ClearRawTokens()
{
	std::lock_guard<std::mutex> lock(rawTokensMutex);
	rawTokens.clear();
}

bool LLamaBackend_LlamaCpp::SupportsThinking() const
{
	if (!model) return false;
	const char *tmpl = llama_model_chat_template(model, nullptr);
	if (!tmpl) return false;
	std::string t(tmpl);
	return t.find("enable_thinking") != std::string::npos
	    || t.find("<think>") != std::string::npos
	    || t.find("</think>") != std::string::npos;
}

std::string LLamaBackend_LlamaCpp::TokenToString(int32_t tokenId, bool special) const
{
	if (!model) return "";
	const llama_vocab *vocab = llama_model_get_vocab(model);
	char buf[256];
	int len = llama_token_to_piece(vocab, tokenId, buf, (int)sizeof(buf), 0, special);
	if (len > 0) return std::string(buf, len);
	return "";
}

// ─── Inference ───────────────────────────────────────────────────────────────

void LLamaBackend_LlamaCpp::InferenceThread(std::string prompt,
                                             MT_LlamaGenerateParams params,
                                             MT_LlamaTokenCallback tokenCb,
                                             std::function<void(MT_LlamaStopReason)> doneCb)
{
	const llama_vocab *vocab = llama_model_get_vocab(model);

	// Debug: log chat template info
	{
		const char *tmpl = llama_model_chat_template(model, nullptr);
		LOGD("LLama InferenceThread: chat template %s", tmpl ? "found" : "NOT FOUND (will use naive fallback)");
		if (tmpl)
		{
			std::string tmplStr(tmpl);
			if (tmplStr.size() > 200) tmplStr = tmplStr.substr(0, 200) + "...";
			LOGD("LLama InferenceThread: template (first 200 chars): %s", tmplStr.c_str());
		}
		LOGD("LLama InferenceThread: stop sequences count=%zu", params.stopSequences.size());
		for (size_t si = 0; si < params.stopSequences.size(); si++)
			LOGD("LLama InferenceThread:   stop[%zu] = \"%s\"", si, params.stopSequences[si].c_str());
		LOGD("LLama InferenceThread: prompt length=%zu chars, max_tokens=%d", prompt.size(), params.max_tokens);
	}

	// Clear raw token log for this generation
	{
		std::lock_guard<std::mutex> lock(rawTokensMutex);
		rawTokens.clear();
	}

	// Always re-process the full prompt from scratch to avoid KV cache misalignment.
	// The token round-trip (generated token IDs → text → re-tokenize on next turn)
	// can produce a different number of tokens, causing n_past to be wrong and the
	// model to attend to misaligned keys/values, producing garbage (e.g., Gemma4
	// "own own own..." loops on second turn).
	if (n_past > 0)
	{
		LOGD("LLama InferenceThread: clearing KV cache (n_past=%d) for fresh prompt processing", n_past);
		llama_memory_clear(llama_get_memory(ctx), true);
		n_past = 0;
	}

	// Tokenize the full prompt.
	// parseSpecial=true: chat template special tokens (<bos>, <|im_start|>, etc.)
	// are converted to their proper token IDs, not split into text pieces.
	// addBos=false here — we handle BOS manually below to avoid double-BOS:
	// Gemma3/4 Jinja templates include {{ bos_token }} which tokenizes to the
	// BOS id via parseSpecial, so a second BOS from addBos=true would corrupt the
	// context and produce garbage output ("a a a a...").
	auto tokens = Tokenize(prompt, false, true);

	// Smart BOS insertion: add BOS only if (a) the vocab/model is configured to
	// prepend BOS, and (b) the chat template didn't already include it.
	// Old Gemma / non-Jinja C++ templates have no BOS in output → insert it.
	// Gemma3/4 Jinja templates output <bos> → tokens[0] == bos_id → skip.
	const llama_vocab *vocab_ptr = llama_model_get_vocab(model);
	if (llama_vocab_get_add_bos(vocab_ptr))
	{
		llama_token bos_id = llama_vocab_bos(vocab_ptr);
		if (tokens.empty() || tokens[0] != bos_id)
			tokens.insert(tokens.begin(), bos_id);
	}

	int nTokens = (int)tokens.size();

	// Decode only the new suffix (tokens[n_past..nTokens))
	for (int i = n_past; i < nTokens && !stopRequested; )
	{
		int batchSize = std::min(nTokens - i, 512);
		llama_batch batch = llama_batch_get_one(tokens.data() + i, batchSize);
		if (llama_decode(ctx, batch) != 0)
		{
			generating = false;
			if (doneCb) doneCb(MT_LlamaStopReason::None);
			return;
		}
		i += batchSize;
		n_past = i;
	}

	// If max_tokens == 0, caller just wanted prompt processing (context rehydration)
	if (params.max_tokens == 0 || stopRequested)
	{
		generating = false;
		if (doneCb) doneCb(stopRequested ? MT_LlamaStopReason::UserStop : MT_LlamaStopReason::None);
		return;
	}

	// Build sampler chain
	// Order: penalties → dry → top_k → top_p → min_p → temperature → dist
	// Matches official llama.cpp recommended order.
	llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
	llama_sampler *smpl = llama_sampler_chain_init(sparams);
	if (params.repeat_penalty != 1.0f && params.repeat_last_n != 0)
		llama_sampler_chain_add(smpl, llama_sampler_init_penalties(
			params.repeat_last_n, params.repeat_penalty, 0.0f, 0.0f));
	// DRY sampler: exponential penalty for repeated token sequences.
	// Breaks "own own own..." loops that repeat_penalty alone cannot stop
	// (repeat_penalty divides logits once per type, not per occurrence count).
	{
		const char *seqBreakers[] = { "\n", ":", "\"", "*" };
		llama_sampler_chain_add(smpl, llama_sampler_init_dry(
			vocab_ptr,
			llama_model_n_ctx_train(model),
			params.dry_multiplier,     // 0.0 = disabled
			params.dry_base,           // exponential base
			params.dry_allowed_length, // tokens up to this length are exempt
			params.dry_penalty_last_n, // look-back window (-1 = context size)
			seqBreakers, 4));
	}
	if (params.top_k > 0)
		llama_sampler_chain_add(smpl, llama_sampler_init_top_k(params.top_k));
	if (params.top_p < 1.0f)
		llama_sampler_chain_add(smpl, llama_sampler_init_top_p(params.top_p, 1));
	if (params.min_p > 0.0f)
		llama_sampler_chain_add(smpl, llama_sampler_init_min_p(params.min_p, 1));
	llama_sampler_chain_add(smpl, llama_sampler_init_temp(params.temperature));
	llama_sampler_chain_add(smpl, llama_sampler_init_dist(params.seed));

	// Determine max stop sequence length for tail buffer sizing
	size_t maxStopLen = 0;
	for (const auto &s : params.stopSequences)
		if (s.size() > maxStopLen) maxStopLen = s.size();

	std::string tailBuf; // accumulates last N chars for stop sequence matching

	int generated = 0;
	bool hitStop = false;
	while (!stopRequested && !hitStop && generated < params.max_tokens)
	{
		llama_token id = llama_sampler_sample(smpl, ctx, -1);
		llama_sampler_accept(smpl, id);

		// Record raw token with special tokens visible (for diagnostics dump)
		{
			char sbuf[256];
			int slen = llama_token_to_piece(vocab, id, sbuf, (int)sizeof(sbuf), 0, true);
			std::string specialPiece(slen > 0 ? std::string(sbuf, slen) : "");
			std::lock_guard<std::mutex> lock(rawTokensMutex);
			rawTokens.push_back({id, specialPiece});
		}

		if (llama_vocab_is_eog(vocab, id))
			break;

		// Piece to string
		char buf[256];
		int len = llama_token_to_piece(vocab, id, buf, (int)sizeof(buf), 0, false);
		if (len > 0)
		{
			std::string piece(buf, len);

			if (maxStopLen > 0)
			{
				tailBuf += piece;

				// Check if any stop sequence appears in tailBuf
				for (const auto &stop : params.stopSequences)
				{
					size_t pos = tailBuf.find(stop);
					if (pos != std::string::npos)
					{
						// Emit only the text before the stop sequence
						std::string before = tailBuf.substr(0, pos);
						if (!before.empty() && tokenCb)
							tokenCb(before);
						tailBuf.clear();
						hitStop = true;
						break;
					}
				}

				if (!hitStop)
				{
					// Find the longest suffix of tailBuf that is a prefix of any stop sequence.
					// We must hold back that many bytes — they might be the beginning of a stop
					// sequence that hasn't been fully received yet.
					// Everything before that suffix is safe to emit immediately.
					size_t holdBack = 0;
					for (const auto &stop : params.stopSequences)
					{
						size_t checkLen = std::min(stop.size() - 1, tailBuf.size());
						for (size_t prefLen = checkLen; prefLen > 0; prefLen--)
						{
							if (tailBuf.compare(tailBuf.size() - prefLen, prefLen,
							                    stop, 0, prefLen) == 0)
							{
								if (prefLen > holdBack)
									holdBack = prefLen;
								break;
							}
						}
					}

					size_t safeToEmit = tailBuf.size() - holdBack;
					if (safeToEmit > 0)
					{
						if (tokenCb)
							tokenCb(tailBuf.substr(0, safeToEmit));
						tailBuf.erase(0, safeToEmit);
					}
				}
			}
			else
			{
				if (tokenCb)
					tokenCb(piece);
			}
		}

		// Feed generated token back into the context
		llama_batch batch = llama_batch_get_one(&id, 1);
		if (llama_decode(ctx, batch) != 0)
			break;
		n_past++;
		generated++;
	}

	// Flush any remaining tail buffer (no stop sequence found in it)
	if (!hitStop && !tailBuf.empty() && tokenCb)
		tokenCb(tailBuf);

	MT_LlamaStopReason reason = MT_LlamaStopReason::None;
	if (stopRequested) reason = MT_LlamaStopReason::UserStop;
	else if (hitStop) reason = MT_LlamaStopReason::StopSequence;
	else if (generated >= params.max_tokens) reason = MT_LlamaStopReason::MaxTokens;
	else reason = MT_LlamaStopReason::EndOfTurn; // the generator broke out of the loop typically because of llama_vocab_is_eog

	llama_sampler_free(smpl);
	generating = false;
	if (doneCb) doneCb(reason);
}
