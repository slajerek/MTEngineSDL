#pragma once

#include "CLlamaTypes.h"

#include <string>
#include <vector>

struct MT_LlamaModelDef {
        std::string id;
        std::string displayName;
        std::string hfRepo;
        std::string hfFilename;
};

// A model added manually by the user (via Browse).
// Stores per-model load and generation params so they can be restored on selection.
struct MT_LlamaCustomModel {
        std::string id;           // e.g. "custom_my_model_gguf"
        std::string displayName;  // filename
        std::string path;         // absolute path to the .gguf file
        MT_LlamaLoadParams    loadParams;
        MT_LlamaGenerateParams defaultGenParams;

	std::string autoSaveChatFolder;
};

class CLlamaModelManager {
public:
        CLlamaModelManager();
        ~CLlamaModelManager();

        // ── Default models ──────────────────────────────────────────────────
        const std::vector<MT_LlamaModelDef> &GetDefaultModels() const;

        // ── Shared model data (default + custom) ────────────────────────────
        std::string GetModelsDir() const;
        void SetModelFolder(const std::string &path);
	std::string GetModelFolder() const;

	void SetAutoSaveChatFolder(const std::string &path);
	std::string GetAutoSaveChatFolder() const;
	std::string GetSelectedModelId() const;
	void SetSelectedModelId(const std::string &modelId);

        std::string GetModelPath(const std::string &modelId) const;
        void SetModelPath(const std::string &modelId, const std::string &path);

        bool IsModelPathValid(const std::string &modelId) const;
        std::string GetModelFileNameFromPath(const std::string &modelId) const;

        std::string GetDownloadUrl(const std::string &modelId) const;
        std::string GetPlannedDownloadPath(const std::string &modelId) const;

        bool GetWasModelLoaded() const;
        void SetWasModelLoaded(bool loaded);

        std::string GetSystemPrompt() const;
        void SetSystemPrompt(const std::string &prompt);

        bool GetPersistSystemPrompt() const;
        void SetPersistSystemPrompt(bool persist);

        bool GetEnableThinking() const;
        void SetEnableThinking(bool enable);

        // ── Global (view-level) load + generation params ─────────────────────
        // These are the "working" params shown in the UI. Persisted so they
        // survive app restarts regardless of which model is selected.
        void SaveGlobalParams(const MT_LlamaLoadParams    &lp,
                              const MT_LlamaGenerateParams &gp);
        void LoadGlobalParams(MT_LlamaLoadParams    &lpOut,
                              MT_LlamaGenerateParams &gpOut) const;

        // ── Custom models ────────────────────────────────────────────────────
        const std::vector<MT_LlamaCustomModel> &GetCustomModels() const;

        // Add a new custom model (or replace if id already exists) and persist.
        void AddCustomModel(const MT_LlamaCustomModel &m);

        // Remove custom model by id and persist.
        void RemoveCustomModel(const std::string &id);

        // Save/load the whole custom-model list from gApplicationDefaultConfig.
        void SaveCustomModels();
        void LoadCustomModels();

        // Returns true if the given id belongs to a custom model.
        bool IsCustomModelId(const std::string &id) const;

        // Scan the models folder for .gguf files. Matches default models by
        // hfRepo/hfFilename path and sets their paths; adds unrecognized files
        // as custom models. Returns the number of newly discovered models.
        int ScanModelsFolder();

        // Update the stored per-model params for a custom model.
        void SetCustomModelParams(const std::string &id,
                                  const MT_LlamaLoadParams    &lp,
                                  const MT_LlamaGenerateParams &gp);

        // Retrieve per-model params for a custom model (returns false if not found).
        bool GetCustomModelParams(const std::string &id,
                                  MT_LlamaLoadParams    &lpOut,
                                  MT_LlamaGenerateParams &gpOut) const;

private:
        std::vector<MT_LlamaModelDef>    defaultModels;
        std::vector<MT_LlamaCustomModel> customModels;

        std::string MakeModelKeySuffix(const std::string &modelId) const;
};
