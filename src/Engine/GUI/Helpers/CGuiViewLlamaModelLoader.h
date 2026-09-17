#pragma once


// CAPABILITY GUARD (MT_ENABLE_LLAMA_CPP). Includable either way, declares nothing when off.
// Its dependency is CFileDownloader, hence httplib. Guarded on the LLM flag rather than the HTTP one because MT_CAP_LLM implies MT_CAP_HTTP: with LLM on the downloader is guaranteed to exist, and with LLM off this is dead weight regardless.
#if MT_ENABLE_LLAMA_CPP

#include "CGuiView.h"
#include "CSystemFileDialogCallback.h"

#include <list>
#include <string>

#include "Sci/Llama/CLlamaService.h"
#include "Sci/Llama/CLlamaModelManager.h"
#include "Sci/Llama/CLlamaModelDownloader.h"

class CGuiViewLlamaModelLoader : public CGuiView, public CSystemFileDialogCallback
{
public:
	// Self-contained: creates its own CLlamaService and CLlamaModelManager.
	CGuiViewLlamaModelLoader(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY);

	// Externally-provided: uses the given service and (optionally) model manager.
	// The caller retains ownership — the view will NOT delete them.
	// If modelManager is nullptr, the view creates and owns its own CLlamaModelManager.
	CGuiViewLlamaModelLoader(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY,
							 CLlamaService *externalService,
							 CLlamaModelManager *externalModelManager = nullptr);

	virtual ~CGuiViewLlamaModelLoader();

	virtual void RenderImGui() override;

	// CSystemFileDialogCallback
	virtual void SystemDialogFileOpenSelected(CSlrString *path) override;
	virtual void SystemDialogFileOpenCancelled() override;
	virtual void SystemDialogPickFolderSelected(CSlrString *path) override;
	virtual void SystemDialogPickFolderCancelled() override;

	void AutoLoadIfNeeded();

	CLlamaModelManager *GetModelManager() const { return modelManager; }

	// Generation settings — edited in the AI Setup panel, read by the chat view / game AI.
	MT_LlamaGenerateParams genParams;

private:
	void Init();
	void EnsureSelectionInitialized();
	void UpdateDisplayedFileName();
	void OpenModelDialog();
	void OpenModelsDirDialog();
	void OpenAutoSaveChatFolderDialog();
	void SwitchToModelId(const std::string &modelId);
	bool SelectedModelIsValidOnDisk() const;
	bool SelectedModelIsLoaded() const;
	std::string SelectedModelId() const;

	// Saves current loadParams/genParams to the currently-selected custom model (if any).
	void SaveParamsToCurrentCustomModel();

	// Pointers to active service and manager.
	// When owns* is true the view allocated the instance and must delete it.
	CLlamaService *llama;
	CLlamaModelManager *modelManager;
	bool ownsLlama;
	bool ownsModelManager;

	CLlamaModelDownloader downloader; // always owned by the view

	std::string modelFileName;
	std::string lastMessage;
	std::string loadedModelId;
	std::string requestedDownloadModelId;

	// Model id queued for removal after the combo closes (cannot mutate list mid-render).
	std::string pendingRemoveModelId;

	MT_LlamaLoadParams loadParams;

	char systemPromptBuf[4096] = {};
	bool systemPromptDirty = false;
	bool persistSystemPrompt = true;

	bool autoLoadAttempted = false;
	bool asyncLoadPending = false;

	enum class FolderPickTarget { None, ModelsDir, AutoSaveChat };
	FolderPickTarget folderPickTarget = FolderPickTarget::None;

	std::list<CSlrString *> openExtensions;
};

#endif // MT_ENABLE_LLAMA_CPP
