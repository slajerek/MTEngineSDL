#include "CGuiViewLlamaModelLoader.h"

#include "SYS_FileSystem.h"

#include "DBG_Log.h"
#include "Sci/Llama/llama_cpp_version.h"

#include <algorithm>
#include <cstdlib>

// ─── Constructors ─────────────────────────────────────────────────────────────

CGuiViewLlamaModelLoader::CGuiViewLlamaModelLoader(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
, llama(new CLlamaService())
, modelManager(new CLlamaModelManager())
, ownsLlama(true)
, ownsModelManager(true)
{
	Init();
}

CGuiViewLlamaModelLoader::CGuiViewLlamaModelLoader(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY,
												   CLlamaService *externalService,
												   CLlamaModelManager *externalModelManager)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
, llama(externalService)
, modelManager(externalModelManager ? externalModelManager : new CLlamaModelManager())
, ownsLlama(false)
, ownsModelManager(externalModelManager == nullptr)
{
	Init();
}

CGuiViewLlamaModelLoader::~CGuiViewLlamaModelLoader()
{
	downloader.Shutdown();

	if (ownsLlama)
		delete llama;

	if (ownsModelManager)
		delete modelManager;

	for (auto *e : openExtensions)
		delete e;
	openExtensions.clear();
}

void CGuiViewLlamaModelLoader::Init()
{
	// file extensions for common llama model formats
	openExtensions.push_back(new CSlrString("gguf"));
	openExtensions.push_back(new CSlrString("bin"));

	// Restore persisted load + gen params (with struct defaults as fallback)
	modelManager->LoadGlobalParams(loadParams, genParams);

	EnsureSelectionInitialized();
	UpdateDisplayedFileName();

	// Restore persisted system prompt settings
	persistSystemPrompt = modelManager->GetPersistSystemPrompt();
	if (persistSystemPrompt)
	{
		std::string sp = modelManager->GetSystemPrompt();
		size_t len = std::min(sp.size(), sizeof(systemPromptBuf) - 1);
		memcpy(systemPromptBuf, sp.c_str(), len);
		systemPromptBuf[len] = '\0';
		if (llama)
		{
			llama->SetSystemPrompt(sp);
			llama->SetPersistSystemPrompt(true);
		}
	}
	else
	{
		// Not persisted — start with empty system prompt
		systemPromptBuf[0] = '\0';
		if (llama)
			llama->SetPersistSystemPrompt(false);
	}

	// Restore thinking mode setting
	if (llama)
		llama->SetEnableThinking(modelManager->GetEnableThinking());

	// If a download was in progress when the app exited, resume it.
	downloader.TryAutoResume();
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

void CGuiViewLlamaModelLoader::EnsureSelectionInitialized()
{
	std::string selected = modelManager->GetSelectedModelId();
	if (!selected.empty())
		return;

	// default to first known model
	const auto &models = modelManager->GetDefaultModels();
	if (!models.empty())
	{
		modelManager->SetSelectedModelId(models[0].id);
	}
}

void CGuiViewLlamaModelLoader::UpdateDisplayedFileName()
{
	modelFileName.clear();
	modelFileName = modelManager->GetModelFileNameFromPath(SelectedModelId());
}

void CGuiViewLlamaModelLoader::OpenModelDialog()
{
	CSlrString title("Load Llama Model");
	CSlrString *defaultFolder = NULL;
	std::string dir = modelManager->GetModelFolder();
	if (!dir.empty())
		defaultFolder = new CSlrString(dir.c_str());
	SYS_DialogOpenFile(this, &openExtensions, defaultFolder, &title);
	if (defaultFolder)
		delete defaultFolder;
}

void CGuiViewLlamaModelLoader::OpenModelsDirDialog()
{
	folderPickTarget = FolderPickTarget::ModelsDir;
	CSlrString *defaultFolder = NULL;
	std::string dir = modelManager->GetModelFolder();
	if (!dir.empty())
		defaultFolder = new CSlrString(dir.c_str());

	SYS_DialogPickFolder(this, defaultFolder);
	if (defaultFolder)
		delete defaultFolder;
}

void CGuiViewLlamaModelLoader::SaveParamsToCurrentCustomModel()
{
	std::string cur = SelectedModelId();
	if (modelManager->IsCustomModelId(cur))
		modelManager->SetCustomModelParams(cur, loadParams, genParams);
}

void CGuiViewLlamaModelLoader::SystemDialogFileOpenSelected(CSlrString *path)
{
	if (!path)
		return;

	char *c = path->GetUTF8();
	if (c)
	{
		std::string modelPath = c;
		free(c);

		// Build a unique id from the filename (sanitize to alphanumeric + underscore)
		char *rawName = SYS_GetFileName(modelPath.c_str());
		std::string safeName = rawName ? rawName : "custom";
		if (rawName) free(rawName);
		for (auto &ch : safeName)
			if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')))
				ch = '_';

		std::string newId = "custom_" + safeName;

		MT_LlamaCustomModel cm;
		cm.id          = newId;
		cm.displayName = safeName; // pretty display name = raw filename
		cm.path        = modelPath;
		cm.loadParams        = loadParams;  // inherit current UI params
		cm.defaultGenParams  = genParams;

		// Use actual filename for display name (before sanitization)
		char *dispRaw = SYS_GetFileName(modelPath.c_str());
		if (dispRaw) { cm.displayName = dispRaw; free(dispRaw); }

		modelManager->AddCustomModel(cm);
		SwitchToModelId(newId);
	}
}

void CGuiViewLlamaModelLoader::SystemDialogFileOpenCancelled()
{
	lastMessage = "File selection cancelled";
}

void CGuiViewLlamaModelLoader::SystemDialogPickFolderSelected(CSlrString *path)
{
	if (!path)
		return;
	char *c = path->GetUTF8();
	if (c)
	{
		std::string folderPath = c;
		free(c);
		if (folderPickTarget == FolderPickTarget::AutoSaveChat)
		{
			modelManager->SetAutoSaveChatFolder(folderPath);
			lastMessage = "Auto-save folder set";
		}
		else
		{
			modelManager->SetModelFolder(folderPath);
			int n = modelManager->ScanModelsFolder();
			lastMessage = "Models folder set, found " + std::to_string(n) + " model" + (n == 1 ? "" : "s");
		}
	}
	folderPickTarget = FolderPickTarget::None;
}

void CGuiViewLlamaModelLoader::SystemDialogPickFolderCancelled()
{
	lastMessage = "Folder selection cancelled";
}

void CGuiViewLlamaModelLoader::OpenAutoSaveChatFolderDialog()
{
	folderPickTarget = FolderPickTarget::AutoSaveChat;
	CSlrString *defaultFolder = NULL;
	std::string dir = modelManager->GetAutoSaveChatFolder();
	if (!dir.empty())
		defaultFolder = new CSlrString(dir.c_str());

	SYS_DialogPickFolder(this, defaultFolder);

	if (defaultFolder)
		delete defaultFolder;
}

std::string CGuiViewLlamaModelLoader::SelectedModelId() const
{
	return modelManager->GetSelectedModelId();
}

bool CGuiViewLlamaModelLoader::SelectedModelIsValidOnDisk() const
{
	return modelManager->IsModelPathValid(SelectedModelId());
}

bool CGuiViewLlamaModelLoader::SelectedModelIsLoaded() const
{
	return llama->HasModelLoaded() && loadedModelId == SelectedModelId();
}

void CGuiViewLlamaModelLoader::SwitchToModelId(const std::string &modelId)
{
	// Before leaving current model: persist its UI params
	SaveParamsToCurrentCustomModel();              // per-custom-model record
	modelManager->SaveGlobalParams(loadParams, genParams); // global (survives restarts)

	// Cancel any in-progress async load
	if (llama->IsLoadingModel())
	{
		llama->CancelLoadModel();
		asyncLoadPending = false;
	}

	// Unload any existing model
	if (llama->HasModelLoaded())
	{
		llama->UnloadModel();
		loadedModelId.clear();
		modelManager->SetWasModelLoaded(false);
	}

	modelManager->SetSelectedModelId(modelId);

	// Restore per-model params if switching to a custom model
	{
		MT_LlamaLoadParams    lp;
		MT_LlamaGenerateParams gp;
		if (modelManager->GetCustomModelParams(modelId, lp, gp))
		{
			loadParams = lp;
			genParams  = gp;
		}
	}

	UpdateDisplayedFileName();

	if (!modelManager->IsModelPathValid(modelId))
	{
		lastMessage = "Model file missing. Set path or Download.";
		return;
	}

	std::string path = modelManager->GetModelPath(modelId);

	// Try async loading first
	if (llama->TryLoadModelAsync(path, loadParams))
	{
		asyncLoadPending = true;
		lastMessage = "Loading...";
		return;
	}

	// Fallback to sync loading (stub/WinCuda backends)
	std::string err;
	if (llama->TryLoadModel(path, loadParams, &err))
	{
		loadedModelId = modelId;
		modelManager->SetWasModelLoaded(true);
		lastMessage = "";  // status indicator shows "Loaded: <filename>"
	}
	else
	{
		lastMessage = err.empty() ? "Load failed" : err;
	}
}

void CGuiViewLlamaModelLoader::AutoLoadIfNeeded()
{
	if (autoLoadAttempted)
		return;
	autoLoadAttempted = true;

	if (!modelManager->GetWasModelLoaded())
		return;

	std::string modelId = SelectedModelId();
	if (modelId.empty())
		return;

	if (!modelManager->IsModelPathValid(modelId))
		return;

	SwitchToModelId(modelId);
}

// ─── Render ───────────────────────────────────────────────────────────────────

void CGuiViewLlamaModelLoader::RenderImGui()
{
	PreRenderImGui();

	// Update downloader state
	downloader.Update(modelManager);

	// Poll async load completion
	if (asyncLoadPending && !llama->IsLoadingModel())
	{
		asyncLoadPending = false;
		if (llama->HasModelLoaded())
		{
			loadedModelId = SelectedModelId();
			modelManager->SetWasModelLoaded(true);
			lastMessage = "";  // status indicator shows "Loaded: <filename>"
		}
		else
		{
			std::string err = llama->GetLoadError();
			lastMessage = err.empty() ? "Load failed" : err;
		}
	}

	bool isLoading = llama->IsLoadingModel();

	ImGui::TextUnformatted("llama.cpp");
	ImGui::Separator();

	const bool compiledIn = CLlamaService::IsCompiledIn();
	ImGui::Text("Compiled: %s  Backend: %s  Available: %s  Model loaded: %s", compiledIn ? MT_LLAMA_CPP_VERSION : "no", llama->GetBackendName().c_str(), llama->IsAvailable() ? "yes" : "no", llama->HasModelLoaded() ? "yes" : "no");

	ImGui::Spacing();
	
	// ── Auto-save chat folder ──────────────────────────────────────────────
	if (ImGui::Button("Set chat folder"))
		OpenAutoSaveChatFolderDialog();
	ImGui::SameLine();
	{
		std::string chatDir = modelManager->GetAutoSaveChatFolder();
		ImGui::TextUnformatted(chatDir.empty() ? "(chat folder not set \xe2\x80\x94 auto-save disabled)" : chatDir.c_str());
	}
	
	///
	if (isLoading)
		ImGui::BeginDisabled();

	// Process any pending removal (from inside last frame's combo)
	if (!pendingRemoveModelId.empty())
	{
		const std::string removeId = pendingRemoveModelId;
		pendingRemoveModelId.clear();
		modelManager->RemoveCustomModel(removeId);
		// If we just removed the selected model, fall back to first available
		if (SelectedModelId() == removeId)
		{
			EnsureSelectionInitialized();
			SwitchToModelId(SelectedModelId());
		}
	}

	EnsureSelectionInitialized();
	const std::string selectedId = SelectedModelId();
	const auto &models       = modelManager->GetDefaultModels();
	const auto &customModels = modelManager->GetCustomModels();

	// Build combo preview label
	std::string preview = "(none)";
	for (const auto &m : models)
		if (m.id == selectedId) { preview = m.displayName; break; }
	for (const auto &cm : customModels)
		if (cm.id == selectedId) { preview = cm.displayName; break; }

	if (ImGui::BeginCombo("Model", preview.c_str()))
	{
		// ── Default models ────────────────────────────────────────────
		for (const auto &m : models)
		{
			bool hasValidPath = modelManager->IsModelPathValid(m.id);
			if (!hasValidPath)
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.65f, 1.0f));

			bool isSelected = (m.id == selectedId);
			if (ImGui::Selectable(m.displayName.c_str(), isSelected))
			{
				requestedDownloadModelId.clear();
				SwitchToModelId(m.id);
				if (!hasValidPath)
					requestedDownloadModelId = m.id;
			}
			if (isSelected)
				ImGui::SetItemDefaultFocus();

			if (!hasValidPath)
				ImGui::PopStyleColor();
		}

		// ── Custom models (after separator) ───────────────────────────
		if (!customModels.empty())
		{
			ImGui::Separator();
			for (const auto &cm : customModels)
			{
				bool isSelected = (cm.id == selectedId);

				// Selectable occupies all but the X button width
				const float removeW = ImGui::GetFrameHeight();
				const float spacing = ImGui::GetStyle().ItemSpacing.x;
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - removeW - spacing);

				// Dim if file missing
				bool valid = modelManager->IsModelPathValid(cm.id);
				if (!valid)
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.65f, 1.0f));

				std::string label = cm.displayName + "##cm_" + cm.id;
				if (ImGui::Selectable(label.c_str(), isSelected, 0,
									  ImVec2(ImGui::GetContentRegionAvail().x - removeW - spacing, 0)))
				{
					SwitchToModelId(cm.id);
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();

				if (!valid)
					ImGui::PopStyleColor();

				// ✕ removal button
				ImGui::SameLine();
				std::string btnId = "X##rm_" + cm.id;
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.1f, 0.1f, 0.6f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 0.9f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
				if (ImGui::Button(btnId.c_str(), ImVec2(removeW, 0)))
					pendingRemoveModelId = cm.id; // defer to after combo closes
				ImGui::PopStyleColor(3);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Remove this custom model from the list");
			}
		}

		ImGui::EndCombo();
	}

	ImGui::SameLine();
	if (ImGui::Button("Set path"))
	{
		OpenModelsDirDialog();
	}
	ImGui::SameLine();
	{
		std::string dir = modelManager->GetModelsDir();
		ImGui::Text("%s", dir.empty() ? "(models folder not set)" : dir.c_str());
	}

	{
		std::string url = modelManager->GetDownloadUrl(selectedId);
		if (!url.empty())
		{
			ImGui::TextUnformatted("Source:");
			ImGui::SameLine();
			ImGui::TextDisabled("%s", url.c_str());
		}
	}

	// Status label above buttons
	{
		bool fileExists = SelectedModelIsValidOnDisk();
		bool modelLoaded = SelectedModelIsLoaded();
		if (isLoading)
		{
			ImGui::TextUnformatted("Loading...");
		}
		else if (modelLoaded)
		{
			std::string fn = modelManager->GetModelFileNameFromPath(loadedModelId);
			if (fn.empty()) fn = loadedModelId;
			ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Loaded: %s", fn.c_str());
		}
		else if (!fileExists)
		{
			ImGui::TextDisabled("Not Downloaded");
		}
		else
		{
			ImGui::TextDisabled("Unloaded");
		}
	}

	// Download stub
	bool canDownload = true;
	if (SelectedModelIsLoaded() && SelectedModelIsValidOnDisk())
		canDownload = false;
	if (downloader.IsDownloading())
		canDownload = false;
	if (modelManager->IsCustomModelId(selectedId))
		canDownload = false;

	if (!canDownload)
		ImGui::BeginDisabled();
	if (ImGui::Button("Download"))
	{
		std::string url = modelManager->GetDownloadUrl(selectedId);
		std::string dst = modelManager->GetPlannedDownloadPath(selectedId);
		if (url.empty())
			lastMessage = "Download: URL not configured";
		else if (dst.empty())
			lastMessage = "Download: set models folder first";
		else
		{
			// Store final destination as model path (file becomes valid once download completes).
			modelManager->SetModelPath(selectedId, dst);
			UpdateDisplayedFileName();

			if (downloader.StartDownload(selectedId, url, dst))
				lastMessage = "Downloading... (supports resume after restart)";
			else
				lastMessage = downloader.GetLastError().empty() ? "Failed to start download" : downloader.GetLastError();
		}
	}
		
	ImGui::SameLine();
	if (!canDownload)
		ImGui::EndDisabled();

	if (ImGui::Button("Browse model file"))
	{
		OpenModelDialog();
	}
	// For default models that have a stale path, allow clearing it
	if (!modelManager->IsCustomModelId(selectedId) && modelManager->IsModelPathValid(selectedId))
	{
		ImGui::SameLine();
		if (ImGui::Button("Clear path"))
		{
			modelManager->SetModelPath(selectedId, "");
			UpdateDisplayedFileName();
			if (SelectedModelIsLoaded())
			{
				llama->UnloadModel();
				loadedModelId.clear();
				modelManager->SetWasModelLoaded(false);
			}
			lastMessage = "Path cleared";
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Remove the stored file path for this model (useful if pointing to wrong file)");
	}
	ImGui::SameLine();
	// Toggle button: shows "Unload" when loaded/loading, "Load" otherwise
	const float loadBtnW = 70.f;
	bool modelIsActive = llama->HasModelLoaded() || llama->IsLoadingModel();
	if (modelIsActive)
	{
		if (ImGui::Button("Unload", ImVec2(loadBtnW, 0)))
		{
			if (llama->IsLoadingModel())
			{
				llama->CancelLoadModel();
				asyncLoadPending = false;
			}
			llama->UnloadModel();
			loadedModelId.clear();
			modelManager->SetWasModelLoaded(false);
			lastMessage = "Unloaded";
		}
	}
	else
	{
		if (ImGui::Button("Load", ImVec2(loadBtnW, 0)))
		{
			SwitchToModelId(selectedId);
		}
	}

	// Download progress
	if (downloader.IsActive())
	{
		uint64_t dl = downloader.GetDownloadedBytes();
		uint64_t tot = downloader.GetTotalBytes();
		float frac = (tot > 0) ? (float)((double)dl / (double)tot) : 0.0f;
		if (frac < 0.0f) frac = 0.0f;
		if (frac > 1.0f) frac = 1.0f;

		double dlMb = (double)dl / (1024.0 * 1024.0);
		double totMb = (double)tot / (1024.0 * 1024.0);
		char label[128];
		if (tot > 0)
			snprintf(label, sizeof(label), "%.1f / %.1f MB  (%.1f%%)", dlMb, totMb, frac * 100.0f);
		else
			snprintf(label, sizeof(label), "%.1f MB  Querying size...", dlMb);

		ImGui::ProgressBar(tot > 0 ? frac : 0.0f, ImVec2(-1, 0), label);
	}

	if (isLoading)
		ImGui::EndDisabled();

	// Model loading progress bar / status — fixed-height slot to prevent layout shift.
	// Always rendered as ProgressBar height: use a visible bar while loading,
	// or an invisible dummy + text when not loading.
	{
		const float barHeight = ImGui::GetFrameHeight();
		if (isLoading)
		{
			float progress = llama->GetLoadProgress();
			char label[64];
			snprintf(label, sizeof(label), "Loading model: %3.0f%%", progress * 100.0f);
			ImGui::ProgressBar(progress, ImVec2(-1, barHeight), label);
		}
		else if (!lastMessage.empty())
		{
			// Same height as ProgressBar so the row never resizes
			float pad = (barHeight - ImGui::GetTextLineHeight()) * 0.5f;
			ImGui::Dummy(ImVec2(0, pad));
			ImGui::TextWrapped("%s", lastMessage.c_str());
		}
		else
		{
			ImGui::Dummy(ImVec2(0, barHeight));
		}
	}

	// Downloader error (if any)
	if (!downloader.GetLastError().empty())
	{
		ImGui::Spacing();
		ImGui::TextWrapped("%s", downloader.GetLastError().c_str());
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::TextUnformatted("Model parameters");
	ImGui::Separator();
	bool loadParamsChanged = false;
	if (ImGui::InputInt("Context (n_ctx)", &loadParams.n_ctx)) loadParamsChanged = true;
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Context window size in tokens.\nLarger values allow longer conversations but require more VRAM/RAM.\nMust be at least as large as the longest prompt + response.");

	if (ImGui::InputInt("Threads (n_threads)", &loadParams.n_threads)) loadParamsChanged = true;
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Number of CPU threads for inference.\n0 = let the backend choose automatically.");

	if (ImGui::InputInt("GPU layers (-1=all, 0=CPU)", &loadParams.n_gpu_layers)) loadParamsChanged = true;
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	
	if (loadParamsChanged)
	{
		SaveParamsToCurrentCustomModel();
		modelManager->SaveGlobalParams(loadParams, genParams);
	}

	if (ImGui::IsItemHovered())
	{
		bool gpuSupported = llama->IsGpuOffloadSupported();
		ImGui::BeginTooltip();
		if (!CLlamaService::IsCompiledIn())
			ImGui::TextUnformatted("GPU unavailable: llama.cpp not compiled in.");
		else if (!gpuSupported)
			ImGui::TextUnformatted("GPU unavailable: backend reports no GPU offload support.\nThe library may have been built without Metal/CUDA.");
		else
			ImGui::TextUnformatted("GPU offload supported.\n-1 = all layers on GPU (Metal on macOS).\n 0 = CPU only.");
		ImGui::EndTooltip();
	}

	ImGui::Spacing();
	ImGui::TextUnformatted("Generation settings");
	ImGui::SameLine();
	if (ImGui::SmallButton("Reset to defaults"))
	{
		genParams = MT_LlamaGenerateParams{};
		SaveParamsToCurrentCustomModel();
		modelManager->SaveGlobalParams(loadParams, genParams);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Reset all generation parameters to their default values.");
	ImGui::Separator();

	bool genParamsChanged = false;

	if (ImGui::BeginTable("genParams", 4, ImGuiTableFlags_None))
	{
		// 4 columns: Label | Control | Label | Control
		// Labels get fixed width, controls stretch to fill remaining space.
		ImGui::TableSetupColumn("lbl0", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("ctl0", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("lbl1", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("ctl1", ImGuiTableColumnFlags_WidthStretch, 1.0f);

		// Row 1: Temperature | Max tokens
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Temperature");
		ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
		if (ImGui::SliderFloat("##temperature", &genParams.temperature, 0.0f, 2.0f, "%.2f")) genParamsChanged = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Controls randomness.\n0.0 = deterministic, 1.0 = full randomness.");
		ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Max tokens");
		ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
		if (ImGui::InputInt("##maxtokens", &genParams.max_tokens)) genParamsChanged = true;
		if (genParams.max_tokens < 1) genParams.max_tokens = 1;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Maximum number of tokens to generate.\nOne token is roughly 0.75 words.");

		// Row 2: Top-P | Top-K
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Top-P");
		ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
		if (ImGui::SliderFloat("##topp", &genParams.top_p, 0.0f, 1.0f, "%.2f")) genParamsChanged = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep tokens summing to P probability.\n1.0 = disabled.");
		ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Top-K");
		ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
		if (ImGui::SliderInt("##topk", &genParams.top_k, 0, 200)) genParamsChanged = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep only K most likely tokens.\n0 = disabled.");

		// Row 3: Min-P | Seed
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Min-P");
		ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
		if (ImGui::SliderFloat("##minp", &genParams.min_p, 0.0f, 1.0f, "%.2f")) genParamsChanged = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep tokens with prob >= min_p * top_token_prob.\n0.0 = disabled.");
		ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Seed");
		ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
		if (ImGui::InputScalar("##seed", ImGuiDataType_U32, &genParams.seed, nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal)) genParamsChanged = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Random seed for sampling.\n0xFFFFFFFF = random each run.\nFixed value = reproducible output.");

		// Row 4: Repeat penalty | Repeat last-N
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Repeat penalty");
		ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
		if (ImGui::SliderFloat("##reppnlty", &genParams.repeat_penalty, 1.0f, 2.0f, "%.2f")) genParamsChanged = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Penalizes recently used tokens to reduce repetition.\n1.0 = disabled, 1.1 = typical default.");
		ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Repeat last-N");
		ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
		if (ImGui::InputInt("##replastn", &genParams.repeat_last_n)) genParamsChanged = true;
		if (genParams.repeat_last_n < 0) genParams.repeat_last_n = 0;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Number of recent tokens to check for repetition.\n0 = disabled, 64 = typical default.");

		// Row 5: DRY multiplier | DRY base
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("DRY multiplier");
		ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
		if (ImGui::SliderFloat("##drymul", &genParams.dry_multiplier, 0.0f, 2.0f, "%.2f")) genParamsChanged = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("DRY sampler: exponential penalty for repeated token sequences.\nBreaks 'own own own...' loops.\n0.0 = disabled, 0.8 = default.");
		ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("DRY base");
		ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
		if (ImGui::SliderFloat("##drybase", &genParams.dry_base, 1.0f, 3.0f, "%.2f")) genParamsChanged = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("DRY exponential base. Higher = harsher penalty for longer repeated runs.\n1.75 = default.");

		ImGui::EndTable();
	}

	if (genParamsChanged)
	{
		SaveParamsToCurrentCustomModel();
		modelManager->SaveGlobalParams(loadParams, genParams);
	}

	// Thinking mode toggle
	{
		bool supportsThinking = llama->HasModelLoaded() && llama->SupportsThinking();
		if (!supportsThinking) ImGui::BeginDisabled();

		bool enableThinking = llama->GetEnableThinking();
		if (ImGui::Checkbox("Enable thinking", &enableThinking))
		{
			llama->SetEnableThinking(enableThinking);
			modelManager->SetEnableThinking(enableThinking);
			// Clear KV cache so the new system prompt (with /think or /no_think) is re-processed
			if (!llama->IsGenerating())
				llama->ClearContext();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered())
		{
			if (supportsThinking)
				ImGui::SetTooltip("When enabled, the model generates a <think> reasoning block before answering.\nDisable to get faster, more concise responses.\nChanging this clears the model's context cache.");
			else
				ImGui::SetTooltip("The loaded model does not support thinking mode.");
		}

		if (!supportsThinking) ImGui::EndDisabled();
	}

	ImGui::Spacing();
	ImGui::TextUnformatted("System prompt");
	ImGui::SameLine();
	if (ImGui::Checkbox("Remember", &persistSystemPrompt))
	{
		modelManager->SetPersistSystemPrompt(persistSystemPrompt);
		if (llama)
			llama->SetPersistSystemPrompt(persistSystemPrompt);
		if (persistSystemPrompt)
		{
			// Save current buffer contents to config
			std::string sp(systemPromptBuf);
			modelManager->SetSystemPrompt(sp);
		}
		else
		{
			// Clear persisted value
			modelManager->SetSystemPrompt("");
		}
	}
	if (ImGui::InputTextMultiline("##systemPrompt", systemPromptBuf, sizeof(systemPromptBuf),
								  ImVec2(-1, ImGui::GetTextLineHeight() * 4)))
	{
		systemPromptDirty = true;
	}
	if (systemPromptDirty && ImGui::IsItemDeactivatedAfterEdit())
	{
		std::string sp(systemPromptBuf);
		if (persistSystemPrompt)
			modelManager->SetSystemPrompt(sp);
		if (llama)
			llama->SetSystemPrompt(sp);
		systemPromptDirty = false;
	}

	ImGui::Separator();


	PostRenderImGui();
}
